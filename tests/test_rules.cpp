#include "rules.h"
#include "logging.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

}  // namespace

int main(int argc, char **argv) {
    RuleSet suffix;
    suffix.add_suffix("example.com");
    expect(suffix.classify("example.com") == RuleSet::Hit::SUFFIX,
           "suffix rule should match its root domain");
    expect(suffix.classify("ads.example.com") == RuleSet::Hit::SUFFIX,
           "suffix rule should match a child domain");
    expect(suffix.classify("deep.ads.example.com") == RuleSet::Hit::SUFFIX,
           "suffix rule should match a deep child domain");
    expect(suffix.classify("badexample.com") == RuleSet::Hit::NONE,
           "suffix rule should respect label boundaries");
    expect(suffix.classify("example.org") == RuleSet::Hit::NONE,
           "suffix rule should not match another domain");

    RuleSet compact;
    constexpr size_t compact_count = 50000;
    for (size_t i = 0; i < compact_count; ++i) {
        std::string domain = "rule-" + std::to_string(i) + ".example";
        compact.add_exact(domain);
        compact.add_exact(domain);  // 重复输入不得扩大统计或常驻数组
    }
    expect(compact.size() == compact_count,
           "compact rule storage should sort and deduplicate hashes");
    expect(compact.storage_bytes() <= compact_count * sizeof(uint64_t) * 2,
           "compact rule storage should stay within 16 bytes per unique hash");
    expect(compact.classify("rule-12345.example") == RuleSet::Hit::EXACT,
           "binary-search storage should preserve exact matching semantics");

    RuleDB db;
    RuleDB invalid;
    expect(parse_hosts_line("invalid..example", invalid) == 0 && invalid.black.bad_lines == 1,
           "pure-parser refactoring must preserve invalid Hosts diagnostics");
    expect(parse_adguard_line("@@||invalid..example^", invalid) == 0 && invalid.white.bad_lines == 1,
           "pure-parser refactoring must preserve invalid whitelist diagnostics");
    expect(parse_adguard_line("||Example.COM^", db) == 1,
           "black suffix rule should parse");
    expect(parse_adguard_line("@@||Allow.Example.COM^", db) == 1,
           "white suffix rule should parse");
    expect(db.black.classify("example.com") == RuleSet::Hit::SUFFIX,
           "parsed black suffix should match root domain");
    expect(db.black.classify("cdn.example.com") == RuleSet::Hit::SUFFIX,
           "parsed black suffix should match child domain");
    expect(db.white.classify("allow.example.com") == RuleSet::Hit::SUFFIX,
           "parsed white suffix should match root domain");
    expect(db.white.classify("api.allow.example.com") == RuleSet::Hit::SUFFIX,
           "parsed white suffix should match child domain");

    // 旧快照必须在发布新规则后仍可被在途读者安全使用。
    auto blocked = std::make_shared<RuleDB>();
    blocked->black.add_exact("flip.example");
    blocked->ok = true;
    publish_rules_snapshot(blocked);
    RuleSnapshot held = rules_snapshot();
    expect(match_domain("flip.example") == Verdict::BLOCK_EXACT,
           "published black snapshot should be visible");

    auto allowed = std::make_shared<RuleDB>();
    allowed->white.add_exact("flip.example");
    allowed->ok = true;
    publish_rules_snapshot(allowed);
    expect(match_domain("flip.example") == Verdict::PASS_WHITELIST,
           "new snapshot should replace the active snapshot atomically");
    expect(held->black.classify("flip.example") == RuleSet::Hit::EXACT,
           "an in-flight reader should retain the old immutable snapshot");

    // 多读者与发布者并发切换时，每次匹配只能观察到一份完整快照。
    std::atomic<bool> keep_reading{true};
    std::atomic<int> snapshot_errors{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&]() {
            while (keep_reading.load(std::memory_order_relaxed)) {
                Verdict verdict = match_domain("flip.example");
                if (verdict != Verdict::BLOCK_EXACT &&
                    verdict != Verdict::PASS_WHITELIST) {
                    snapshot_errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (int i = 0; i < 2000; ++i) {
        publish_rules_snapshot((i & 1) ? RuleSnapshot(allowed)
                                       : RuleSnapshot(blocked));
    }
    keep_reading.store(false, std::memory_order_relaxed);
    for (auto &reader : readers) reader.join();
    expect(snapshot_errors.load(std::memory_order_relaxed) == 0,
           "concurrent readers should only observe complete snapshots");

    expect(argc == 2, "rule case file path should be provided");
    if (argc == 2) {
        std::ifstream cases(argv[1]);
        expect(cases.is_open(), "rule case file should open");
        std::string line;
        while (std::getline(cases, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string input, accepted_text, kind, domain;
            std::getline(fields, input, '\t');
            std::getline(fields, accepted_text, '\t');
            std::getline(fields, kind, '\t');
            std::getline(fields, domain, '\t');

            RuleDB one;
            bool accepted = parse_adguard_line(input, one) == 1;
            bool expected = accepted_text == "1";
            expect(accepted == expected, ("fixture acceptance mismatch: " + input).c_str());
            if (!expected) continue;

            const RuleSet *set = kind.rfind("white_", 0) == 0
                ? &one.white : &one.black;
            RuleSet::Hit expected_hit = kind.find("suffix") != std::string::npos
                ? RuleSet::Hit::SUFFIX : RuleSet::Hit::EXACT;
            expect(set->classify(domain) == expected_hit,
                   ("fixture kind mismatch: " + input).c_str());
        }
    }

    char gzip_path[] = "/tmp/ad_killer_by_nfqueue_rules.XXXXXX";
    int gzip_fd = mkstemp(gzip_path);
    expect(gzip_fd >= 0, "temporary gzip path should be created");
    if (gzip_fd >= 0) {
        close(gzip_fd);
        std::string valid_gzip;
        expect(gz_compress_string("0.0.0.0 staged.example\n", valid_gzip),
               "valid staging gzip should compress");
        {
            std::ofstream output(gzip_path, std::ios::binary | std::ios::trunc);
            output.write(valid_gzip.data(), static_cast<std::streamsize>(valid_gzip.size()));
        }
        RuleDB staged;
        int loaded = 0;
        expect(load_rule_file_checked(gzip_path, false, staged, &loaded, true) &&
                   loaded == 1,
               "complete gzip should pass strict staging validation");

        valid_gzip.resize(valid_gzip.size() - 4);  // 截断 gzip trailer
        {
            std::ofstream output(gzip_path, std::ios::binary | std::ios::trunc);
            output.write(valid_gzip.data(), static_cast<std::streamsize>(valid_gzip.size()));
        }
        RuleDB truncated;
        truncated.black.add_exact("existing.example");
        expect(!load_rule_file_checked(gzip_path, false, truncated, &loaded, true) &&
                   truncated.black.classify("existing.example") == RuleSet::Hit::EXACT &&
                   truncated.black.classify("staged.example") == RuleSet::Hit::NONE,
               "truncated gzip must fail even if it contains parseable prefix data");

        {
            std::ofstream output(gzip_path, std::ios::binary | std::ios::trunc);
            output << "0.0.0.0 plain.example\n";
        }
        RuleDB plain;
        expect(!load_rule_file_checked(gzip_path, false, plain, &loaded, true),
               "staging validation must require gzip for generated artifacts");
        for (bool adguard : {false, true}) {
            std::string empty_category;
            expect(gz_compress_string("# AD_Killer_By_NFQUEUE generated empty category\n", empty_category),
                   "empty generated category should have a valid comment header");
            {
                std::ofstream output(gzip_path, std::ios::binary | std::ios::trunc);
                output.write(empty_category.data(), static_cast<std::streamsize>(empty_category.size()));
            }
            RuleDB empty;
            loaded = -1;
            expect(load_rule_file_checked(gzip_path, adguard, empty, &loaded, true) &&
                       loaded == 0 && empty.black.empty() && empty.white.empty(),
                   "an empty generated category must not reject an otherwise valid bundle");
        }
        const std::string adguard_path = std::string(gzip_path) + ".adguard";
        const std::string custom_path = std::string(gzip_path) + ".custom";
        std::string adguard_gzip;
        expect(gz_compress_string("||adguard-only.example^\n", adguard_gzip), "compress AdGuard fixture");
        {
            std::ofstream output(adguard_path, std::ios::binary);
            output.write(adguard_gzip.data(), static_cast<std::streamsize>(adguard_gzip.size()));
        }
        RuleDB combined, hosts_only;
        load_rule_files(combined, MODE_HOSTS_ADGUARD, gzip_path, adguard_path, custom_path, true);
        expect(combined.load_valid && combined.ok && combined.black.size() == 1,
               "combined mode must load AdGuard with an empty Hosts category");
        load_rule_files(hosts_only, MODE_HOSTS_ONLY, gzip_path, adguard_path, custom_path, true);
        expect(hosts_only.load_valid && !hosts_only.ok && hosts_only.black.empty(),
               "intentional Hosts-only empty snapshot must be publishable, not retain AdGuard");
        { std::ofstream empty_custom(custom_path); }
        load_rule_files(hosts_only, MODE_HOSTS_ONLY, gzip_path, adguard_path, custom_path, true);
        expect(hosts_only.load_valid && !hosts_only.ok, "cleared zero-byte custom file is valid empty input");
        {
            std::ofstream output(custom_path);
            output << "||custom.example^\n@@||safe.example^\n";
        }
        load_rule_files(hosts_only, MODE_HOSTS_ONLY, gzip_path, adguard_path, custom_path, true);
        expect(hosts_only.load_valid && hosts_only.black.size() == 1 && hosts_only.white.size() == 1,
               "custom black/white must apply independently of mode");
        {
            std::ofstream output(adguard_path, std::ios::binary | std::ios::trunc);
            output << "corrupt non-gzip";
        }
        load_rule_files(combined, MODE_HOSTS_ADGUARD, gzip_path, adguard_path, custom_path, true);
        expect(!combined.load_valid && combined.black.empty() && combined.white.empty(),
               "corrupt category must reject the whole candidate, not publish partial rules");
        load_rule_files(hosts_only, MODE_HOSTS_ONLY, gzip_path, adguard_path, custom_path, true);
        expect(hosts_only.load_valid, "unselected AdGuard category is not read in Hosts-only mode");
        unlink(custom_path.c_str()); unlink(adguard_path.c_str());
        load_rule_files(combined, MODE_HOSTS_ADGUARD, gzip_path, adguard_path, custom_path, true);
        expect(!combined.load_valid, "missing selected published category must be rejected");
        load_rule_files(combined, MODE_HOSTS_ADGUARD, gzip_path, adguard_path, custom_path, false);
        expect(combined.load_valid && !combined.ok, "absent pre-sync legacy files remain valid empty input");
        unlink(gzip_path);
    }

    if (failures != 0) {
        std::cerr << failures << " rule test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "rule tests passed\n";
    return EXIT_SUCCESS;
}
