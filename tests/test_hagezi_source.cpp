#include "hagezi_source.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static void write(const std::string &path, const std::string &text) { std::ofstream(path) << text; }
int main(int argc, char **argv) {
    if (argc == 4) {
        auto result = merge_hagezi_pair(argv[1], argv[2]);
        if (!result.accepted) { std::cerr << result.reason << '\n'; return 1; }
        write(argv[3], result.hosts_rules);
        std::cout << result.stats.difference_base_unique << " - " << result.stats.difference_removed
                  << " = " << result.stats.accepted_hosts << '\n';
        return 0;
    }
    char temporary[] = "/tmp/ad_killer_by_nfqueue_hagezi.XXXXXX";
    assert(mkdtemp(temporary));
    const std::string dir = temporary, pro = dir + "/pro", fake = dir + "/fake";
    const std::string pro_text = "# Pro\n*.KEEP.example.com\n*.fake.example.com\nkeep.example.com\n0.0.0.0 other.example.com\ninvalid/path\n";
    const std::string fake_text = "# Fake\n*.fake.example.com\nnot-in-pro.example.org\n";
    write(pro, pro_text); write(fake, fake_text);
    auto result = merge_hagezi_pair(pro, fake);
    assert(result.accepted && result.hosts_rules == "keep.example.com\nother.example.com\n");
    assert(result.adguard_rules.empty()); // Not a global allow list.
    assert(result.stats.difference_base_unique == 3 && result.stats.difference_exclude_unique == 2);
    assert(result.stats.difference_removed == 1 && result.stats.accepted_hosts == 2);
    assert(result.stats.bytes == pro_text.size() + fake_text.size());
    RuleStreamLimits limits;
    limits.output_bytes = pro_text.size() + fake_text.size() - 1;
    assert(!merge_hagezi_pair(pro, fake, limits).accepted);
    limits = {}; limits.lines = 7;
    assert(!merge_hagezi_pair(pro, fake, limits).accepted);
    for (const auto &bad : {"", "# empty\n", "<!doctype html><html>bad</html>\n", "garbage\n"}) {
        write(fake, bad);
        result = merge_hagezi_pair(pro, fake);
        assert(!result.accepted && result.hosts_rules.empty());
    }
    write(fake, pro_text);
    result = merge_hagezi_pair(pro, fake);
    assert(result.accepted && result.hosts_rules.empty() && result.stats.difference_removed == 3);
    size_t bytes = 0, calls = 0;
    auto download = [&](const std::string &url, const std::string &path) {
        ++calls;
        assert(url == HAGEZI_PRO_URL || url == HAGEZI_FAKE_URL);
        write(path, url == HAGEZI_PRO_URL ? pro_text : fake_text);
        return std::string{};
    };
    result = fetch_hagezi_source(dir, {}, download, bytes);
    assert(result.accepted && calls == 2 && bytes == pro_text.size() + fake_text.size());
    assert(access((dir + "/hagezi-pro.part").c_str(), F_OK) != 0);
    assert(access((dir + "/hagezi-fake.part").c_str(), F_OK) != 0);
    result = fetch_hagezi_source(dir, {}, [&](const std::string &url, const std::string &path) {
        if (url == HAGEZI_FAKE_URL) return std::string("HTTP 404");
        return download(url, path);
    }, bytes);
    assert(!result.accepted && result.hosts_rules.empty() && result.reason.find("fake_download") != std::string::npos);
    assert(access((dir + "/hagezi-pro.part").c_str(), F_OK) != 0);
    assert(unlink(pro.c_str()) == 0 && unlink(fake.c_str()) == 0 && rmdir(dir.c_str()) == 0);
    std::cout << "HaGeZi paired validation, exact difference, duplicates, bounds, missing Fake and cleanup passed\n";
}
