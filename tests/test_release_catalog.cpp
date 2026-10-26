#include "bounded_json.h"
#include "release_catalog.h"
#include "source_config.h"
#include "work_budget.h"
#include <cassert>
#include <iostream>
#include <thread>

static const std::string repo = "https://github.com/fengwenhua/AD_Killer_By_NFQUEUE";
static std::string release(const std::string &version = "1.2", const std::string &body = "notes") {
    const auto name = "AD_Killer_By_NFQUEUE-v" + version + ".zip", url = repo + "/releases/download/v" + version + "/" + name;
    return "{\"tag_name\":\"v" + version + "\",\"draft\":false,\"prerelease\":false,\"html_url\":\"" + repo + "/releases/tag/v" + version +
        "\",\"body\":" + source_json_string(body) + ",\"assets\":[{\"name\":\"" + name + "\",\"size\":500,\"browser_download_url\":\"" + url +
        "\"},{\"name\":\"" + name + ".sha256\",\"size\":96,\"browser_download_url\":\"" + url + ".sha256\"}]}";
}
int main() {
    using bounded_json::Value;
    Value value;
    for (const auto &good : {"null", "false", "true", "-1.25e+2", "[0,1,{},[]]", "{\"\\u0061\":\"\\ud83d\\ude00你好\\n\"}"})
        assert(bounded_json::parse(good, value));
    assert(value.get("a")->text == "😀你好\n");
    for (const auto &bad : {"", "trueX", "01", "+1", "1.", "1e", "1e+", "[1,]", "{\"a\":1,}", "{}x", "{\"a\":1,\"\\u0061\":2}",
                            "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0061\"", "\"\\x20\"", "\"unclosed", "\"raw\ncontrol\""})
        assert(!bounded_json::parse(bad, value));
    for (const auto &bytes : {std::string("\"\xc0\x80\""), std::string("\"\xed\xa0\x80\""), std::string("\"\xf4\x90\x80\x80\""), std::string("\"\xe4\xbd\"")})
        assert(!bounded_json::parse(bytes, value));
    assert(!bounded_json::parse(std::string(34, '[') + "0" + std::string(34, ']'), value));
    std::string nodes = "[";
    for (unsigned i = 0; i < bounded_json::MAX_NODES; ++i) nodes += "0,";
    nodes.back() = ']'; assert(!bounded_json::parse(nodes, value));
    assert(!bounded_json::parse(std::string(bounded_json::MAX_BYTES + 1, ' '), value));
    assert(bounded_json::parse("18446744073709551615", value)); uint64_t number = 0;
    assert(value.integer(number) && number == UINT64_MAX);
    for (const auto &bad : {"18446744073709551616", "1e3", "1.0", "-1"}) {
        assert(bounded_json::parse(bad, value)); assert(!value.integer(number));
    }
    const auto original = release("1.2", std::string(100000, 'n'));
    const auto page = release_catalog::project("[" + original + "]", repo);
    assert(page.valid && page.candidates == 1 && page.json.size() < 1000 && page.json.find("body") == std::string::npos);
    auto notes = release_catalog::project("[" + release("1.2", "你好😀" + std::string(20000, 'x')) + "]", repo, true);
    assert(notes.valid && notes.candidates == 1 && bounded_json::parse(notes.json, value));
    assert(value.items[0].get("body_truncated")->boolean && value.items[0].get("body")->text.size() <= release_catalog::NOTES_BYTES);
    const auto check_notes = [&](const std::string &body, const std::string &expected, bool truncated) {
        auto record = release("1.2", body);
        // Independently truncated title/date must not change the body flag.
        record.insert(1, "\"name\":" + source_json_string(std::string(300, 't')) +
            ",\"published_at\":" + source_json_string(std::string(80, 'd')) + ",");
        const auto projected = release_catalog::project("[" + record + "]", repo, true);
        Value parsed;
        assert(projected.valid && projected.candidates == 1 && bounded_json::parse(projected.json, parsed));
        const auto &entry = parsed.items[0];
        assert(entry.get("body")->text == expected);
        assert(entry.get("body_truncated")->boolean == truncated);
        assert(entry.get("name")->text == std::string(200, 't'));
        assert(entry.get("published_at")->text == std::string(40, 'd'));
    };
    check_notes("", "", false);
    check_notes("short notes", "short notes", false);
    const std::string exact(release_catalog::NOTES_BYTES, 'x');
    check_notes(exact, exact, false);
    check_notes(exact + "y", exact, true);
    const std::string before_three(release_catalog::NOTES_BYTES - 1, 'x');
    check_notes(before_three + "中", before_three, true);
    const std::string before_four(release_catalog::NOTES_BYTES - 2, 'x');
    check_notes(before_four + "😀", before_four, true);
    const std::string full_character = std::string(release_catalog::NOTES_BYTES - 3, 'x') + "中";
    check_notes(full_character + "y", full_character, true);
    assert(!release_catalog::project("[" + release() + "," + release() + "]", repo, true).valid);
    auto invalid = release();
    invalid.replace(invalid.find("500"), 3, "5e2");
    assert(release_catalog::project("[" + invalid + "]", repo).candidates == 0);
    invalid = release();
    invalid.insert(invalid.size() - 2, ",{\"name\":\"AD_Killer_By_NFQUEUE-v1.2.zip\"}");
    assert(release_catalog::project("[" + invalid + "]", repo).candidates == 0);
    invalid = release(); invalid.replace(invalid.find("\"size\":96"), 9, "\"size\":0");
    assert(release_catalog::project("[" + invalid + "]", repo).candidates == 0);
    invalid = release(); invalid.replace(invalid.find("\"prerelease\":false"), 18, "\"prerelease\":true");
    assert(release_catalog::project("[" + invalid + "]", repo).candidates == 1);
    assert(release_catalog::project(invalid, repo, false, true).candidates == 0);
    assert(!release_catalog::project("[" + release() + "]junk", repo).valid);
    unsigned calls = 0;
    const std::string empty_page = "[{},{},{},{},{}]";
    auto result = release_catalog::scan("https://api.example/releases", repo, [&](const auto &url, auto &body, size_t limit, long &status) {
        ++calls; assert(limit == release_catalog::PAGE_BYTES);
        assert(url == "https://api.example/releases?per_page=5&page=" + std::to_string(calls));
        body = calls == 1 ? empty_page : "[" + release("1.3") + "]"; status = 200; return "";
    });
    assert(result.ok && calls == 2 && result.pages == 2 && result.json.find("v1.3") != std::string::npos);
    calls = 0;
    result = release_catalog::scan("api", repo, [&](const auto &, auto &body, size_t, long &status) {
        ++calls; body = empty_page; status = 200; return "";
    });
    assert(!result.ok && calls == release_catalog::MAX_PAGES && !result.error.empty());
    calls = 0;
    result = release_catalog::scan("api", repo, [&](const auto &, auto &body, size_t, long &status) {
        ++calls; body = calls == 1 ? empty_page : "[" + release() + "]"; status = calls == 1 ? 200 : 429;
        return calls == 1 ? "" : "HTTP 429";
    });
    assert(!result.ok && result.http_status == 429 && calls == 2 && result.json.empty());
    result = release_catalog::scan("api", repo, [&](const auto &, auto &body, size_t, long &status) {
        body = "[" + release() + "]"; status = 206; return "";
    });
    assert(!result.ok && result.json.empty());
    {
        WorkBudget outer(1000, 10);
        result = release_catalog::scan("api", repo, [&](const auto &, auto &body, size_t limit, long &status) {
            assert(limit == 10); body = "[" + release() + "]"; status = 200; return "";
        });
        assert(!result.ok && result.json.empty());
    }
    {
        WorkBudget outer(1, 1000);
        result = release_catalog::scan("api", repo, [&](const auto &, auto &body, size_t, long &) {
            body = "[" + release() + "]"; std::this_thread::sleep_for(std::chrono::milliseconds(5)); return "";
        });
        assert(!result.ok && result.json.empty());
    }
    std::cout << "release catalog: strict bounded JSON/UTF-8, projection, pagination, end/limits, throttling and shared deadlines passed\n";
}
