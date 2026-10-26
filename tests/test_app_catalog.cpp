#include "app_catalog.h"
#include <cassert>
#include <iostream>
#include <set>

int main(int argc, char **argv) {
    using app_catalog::parse;
    // Optional read-only format probe: no package names/UIDs are printed or
    // persisted. Keep the input allowance identical to the production parser.
    if (argc == 2 && std::string(argv[1]) == "--check-stdin") {
        std::string text; char chunk[4096];
        while (std::cin) {
            std::cin.read(chunk, sizeof(chunk));
            const auto size = static_cast<size_t>(std::cin.gcount());
            if (size > app_catalog::MAX_BYTES - text.size()) { std::cout << "{\"known\":false,\"error\":\"input_limit\"}\n"; return 1; }
            text.append(chunk, size);
        }
        if (!std::cin.eof()) { std::cout << "{\"known\":false,\"error\":\"input_read_failed\"}\n"; return 1; }
        const auto catalog = parse(text);
        std::set<uint32_t> users;
        for (const auto &instance : catalog.instances) users.insert(instance.user);
        std::cout << "{\"known\":" << (catalog.known ? "true" : "false") << ",\"instances\":" << catalog.instances.size()
                  << ",\"users\":" << users.size() << ",\"without_uid\":" << catalog.without_uid.size()
                  << ",\"error\":\"" << catalog.error << "\"}\n";
        return catalog.known ? 0 : 1;
    }
    assert(argc == 1);
    const auto platform = parse("package:android uid:1000\npackage:vendor uid:1000\npackage:com.example.app uid:10001\n");
    assert(platform.known && platform.instances.size() == 3 && platform.instances[0].package == "android");
    assert(!app_catalog::resolve_primary(platform, {"vendor"}).known); // no change to selectable keys
    const auto catalog = parse("package:com.example.app uid:10001,1010001,99910001\n"
                               "package:com.example.shared uid:10001\npackage:com.example.apex\n");
    assert(catalog.known && catalog.instances.size() == 4 && catalog.without_uid.size() == 1);
    assert(catalog.instances[0].uid == 10001 && catalog.instances[1].user == 10 && catalog.instances[2].user == 999);
    const auto reordered = parse("package:com.example.apex\npackage:com.example.shared uid:10001\n"
                                 "package:com.example.app uid:99910001,10001,1010001,10001\n");
    assert(reordered.known && reordered.revision == catalog.revision);
    auto selected = app_catalog::resolve_primary(catalog, {"com.example.app", "com.example.app", "com.example.absent"});
    assert(selected.known && selected.mappings.size() == 1 && selected.mappings[0].uid == 10001);
    assert(selected.uninstalled == std::vector<std::string>{"com.example.absent"});
    assert(selected.shared_packages == std::vector<std::string>{"com.example.shared"} && selected.other_user_instances == 2);
    assert(app_catalog::resolve_primary(reordered, {"com.example.absent", "com.example.app"}).revision == selected.revision);
    const auto changed = parse("package:com.example.app uid:10002,1010001\npackage:com.example.reused uid:10001\n");
    const auto reassigned = app_catalog::resolve_primary(changed, {"com.example.app", "com.example.absent"});
    assert(reassigned.known && reassigned.mappings[0].uid == 10002 && reassigned.revision != selected.revision);
    const auto clone_only = parse("package:com.example.app uid:1010001,99910001\npackage:com.example.other uid:10001\n");
    const auto removed = app_catalog::resolve_primary(clone_only, {"com.example.app"});
    assert(removed.known && removed.mappings.empty() && removed.uninstalled.size() == 1 && removed.other_user_instances == 2);
    assert(!app_catalog::resolve_primary(catalog, {"com.example.app", "com.example.apex"}).known);
    assert(!app_catalog::resolve_primary({}, {"com.example.app"}).known);
    assert(!app_catalog::resolve_primary(catalog, {"invalid"}).known);
    assert(!app_catalog::resolve_primary(parse("package:com.example.root uid:0\n"), {"com.example.root"}).known);
    assert(app_catalog::resolve_primary({}, {}).known); // empty selection never needs Binder
    for (const auto &bad : {"", "Success\n", "package:bad/name uid:10001\n", "package:com.test uid:-1\n",
                            "package:com.test uid:1,\n", "package:com.test uid:1,,2\n", "package:com.test uid:1 2\n",
                            "package:com.test uid:2147483648\n", "package:com.test uid:1,2\n",
                            "package:com.test uid:1\npackage:com.test uid:2\n",
                            "package:com.test\npackage:com.test uid:1\n"}) {
        const auto invalid = parse(bad);
        assert(!invalid.known && invalid.instances.empty() && invalid.revision.empty());
    }
    assert(!parse(std::string("package:com.test uid:1\0\n", 24)).known);
    assert(!parse(std::string(app_catalog::MAX_BYTES + 1, 'x')).known);
    unsigned calls = 0;
    auto result = app_catalog::query([&](const auto &arguments, unsigned timeout, size_t limit) {
        ++calls; assert(timeout <= 2000 && limit == app_catalog::MAX_BYTES);
        assert(arguments[arguments.size()-2] == "--user" && arguments.back() == "all");
        ProcessResult response;
        if (calls == 1) { response.exit_code = 127; return response; }
        response.exit_code = 0; response.output = "package:com.test uid:10001\n"; return response;
    });
    assert(result.known && calls == 2);
    calls = 0;
    result = app_catalog::query([&](const auto &, unsigned, size_t limit) {
        ++calls; ProcessResult response;
        if (calls == 1) { response.exit_code = 1; response.output = "failed"; response.diagnostic = "query"; }
        else {
            assert(limit == app_catalog::MAX_BYTES - 11); // both streams consume the shared allowance
            response.exit_code = 0; response.output = "package:com.test uid:10001\n";
        }
        return response;
    });
    assert(result.known && calls == 2);
    calls = 0;
    result = app_catalog::query([&](const auto &, unsigned, size_t limit) {
        ++calls; ProcessResult response; response.exit_code = 0; response.output.assign(limit + 1, 'x'); return response;
    });
    assert(!result.known && calls == 1);
    for (unsigned mode = 0; mode < 6; ++mode) {
        calls = 0;
        result = app_catalog::query([&](const auto &, unsigned, size_t) {
            ++calls; ProcessResult response; response.exit_code = 0;
            if (mode == 0) response.timed_out = true;
            if (mode == 1) response.overflow = true;
            if (mode == 2) response.reaped = false;
            if (mode == 3) response.output = "package:com.test uid:1,2\n";
            if (mode == 4) response.cancelled = true;
            if (mode == 5) { response.output = "package:com.test uid:10001\n"; response.diagnostic = "partial query failure\n"; }
            return response;
        });
        assert(!result.known && result.instances.empty() && calls == 1);
    }
    std::cout << "package catalogs: owner-only selection, reinstall/reuse, shared UID, multiuser comma UIDs, missing/unknown identities, stable revisions and bounded query fallback passed\n";
}
