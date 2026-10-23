#include "rule_bundle.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

std::string read_text(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void cleanup_root(const std::string &root) {
    for (const char *generation : {"gen1", "gen2", "gen3"}) {
        std::string dir = root + "/rulesets/" + generation;
        unlink((dir + "/hosts_rules.gz").c_str());
        unlink((dir + "/adguard_rules.gz").c_str());
        rmdir(dir.c_str());
        std::string staging = root + "/rulesets/.staging-" + generation;
        unlink((staging + "/hosts_rules.gz").c_str());
        unlink((staging + "/adguard_rules.gz").c_str());
        rmdir(staging.c_str());
    }
    unlink((root + "/rules.current").c_str());
    unlink((root + "/rules.current.previous").c_str());
    unlink((root + "/legacy-hosts.gz").c_str());
    unlink((root + "/legacy-adguard.gz").c_str());
    rmdir((root + "/rulesets").c_str());
    rmdir(root.c_str());
}

}  // namespace

int main() {
    char root_template[] = "/tmp/ad_killer_by_nfqueue_bundle.XXXXXX";
    char *created = mkdtemp(root_template);
    expect(created != nullptr, "temporary bundle root should be created");
    if (!created) return EXIT_FAILURE;
    std::string root = created;
    std::string manifest = root + "/rules.current";
    std::string legacy_hosts = root + "/legacy-hosts.gz";
    std::string legacy_adguard = root + "/legacy-adguard.gz";

    RuleBundlePaths fallback = resolve_rule_bundle(
        root, manifest, legacy_hosts, legacy_adguard);
    expect(!fallback.from_manifest && fallback.hosts_path == legacy_hosts &&
               fallback.adguard_path == legacy_adguard,
           "missing manifest should preserve legacy fixed-file compatibility");

    StagedRuleBundle first;
    std::string error;
    expect(stage_rule_bundle(root, "gen1", "hosts-one", "adguard-one",
                             first, &error),
           "first generation should stage");
    RuleBundlePaths before_commit = resolve_rule_bundle(
        root, manifest, legacy_hosts, legacy_adguard);
    expect(!before_commit.from_manifest,
           "staged generation must remain invisible before pointer commit");
    expect(commit_rule_bundle(manifest, first, &error),
           "first generation pointer should commit");

    RuleBundlePaths active = resolve_rule_bundle(
        root, manifest, legacy_hosts, legacy_adguard);
    expect(active.from_manifest && active.generation == "gen1",
           "committed manifest should select generation one");
    expect(read_text(active.hosts_path) == "hosts-one" &&
               read_text(active.adguard_path) == "adguard-one",
           "both active files must come from the same committed generation");

    StagedRuleBundle second;
    expect(stage_rule_bundle(root, "gen2", "hosts-two", "adguard-two",
                             second, &error),
           "second generation should stage");
    discard_staged_rule_bundle(second);  // 模拟校验失败
    active = resolve_rule_bundle(root, manifest, legacy_hosts, legacy_adguard);
    expect(active.from_manifest && active.generation == "gen1" &&
               read_text(active.hosts_path) == "hosts-one",
           "discarded validation failure must leave old generation active");

    StagedRuleBundle third;
    expect(stage_rule_bundle(root, "gen3", "hosts-three", "adguard-three",
                             third, &error),
           "third generation should stage");
    expect(!commit_rule_bundle(root + "/missing/rules.current", third, &error),
           "manifest write failure should reject the commit");
    active = resolve_rule_bundle(root, manifest, legacy_hosts, legacy_adguard);
    expect(active.from_manifest && active.generation == "gen1" &&
               read_text(active.adguard_path) == "adguard-one",
           "failed pointer commit must retain both old active files");

    {
        std::ofstream corrupt(manifest, std::ios::binary | std::ios::trunc);
        corrupt << "../../escape\n";
    }
    RuleBundlePaths corrupt = resolve_rule_bundle(
        root, manifest, legacy_hosts, legacy_adguard);
    expect(!corrupt.from_manifest && corrupt.hosts_path == legacy_hosts,
           "invalid generation pointer must not escape the rulesets directory");

    cleanup_root(root);
    if (failures != 0) {
        std::cerr << failures << " rule bundle test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "rule bundle tests passed\n";
    return EXIT_SUCCESS;
}
