#include "rule_bundle.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
void expect(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
struct Fixture {
    std::string root;
    Fixture() {
        char name[] = "/tmp/adk-rule-gc.XXXXXX";
        auto *created = mkdtemp(name); expect(created, "mkdtemp"); root = created;
    }
    ~Fixture() { fs::remove_all(root); }
    std::string manifest() const { return root + "/rules.current"; }
    RuleBundlePaths read() const { return resolve_rule_bundle(root, manifest(), "", ""); }
    void publish(const std::string &name, const std::string &content = "") {
        StagedRuleBundle staged; std::string error;
        expect(stage_rule_bundle(root, name, "hosts-" + (content.empty() ? name : content),
            "adguard-" + (content.empty() ? name : content), staged, &error), error.c_str());
        expect(commit_rule_bundle(manifest(), staged, &error), error.c_str());
    }
};
std::string text(const std::string &path) { std::ifstream file(path); return {std::istreambuf_iterator<char>(file), {}}; }
void put(const std::string &path, const std::string &value) { std::ofstream file(path); file << value; expect(file.good(), "fixture write"); }
std::string failed_phase;
bool gate(const char *phase) { if (phase == failed_phase) { errno = ENOSPC; return false; } return true; }
void fault(const char *phase) { failed_phase = phase; rule_bundle_test_gate(gate); }
void clear_fault() { rule_bundle_test_gate(nullptr); failed_phase.clear(); }

void retention_and_readers() {
    Fixture f; f.publish("original");
    int ready[2], resume[2]; expect(!pipe(ready) && !pipe(resume), "pipes");
    const pid_t child = fork(); expect(child >= 0, "fork");
    if (!child) {
        close(ready[0]); close(resume[1]);
        auto old = f.read(); char value = old.from_manifest ? '1' : '0';
        if (write(ready[1], &value, 1) != 1 || read(resume[0], &value, 1) != 1) _exit(2);
        _exit(text(old.hosts_path) == "hosts-original" && text(old.adguard_path) == "adguard-original" ? 0 : 3);
    }
    close(ready[1]); close(resume[0]); char value = 0;
    expect(read(ready[0], &value, 1) == 1 && value == '1', "child captured paths before opening content");
    for (int i = 0; i < 100; ++i) {
        f.publish("gen-" + std::to_string(i));
        const auto gc = collect_rule_bundles(f.root);
        expect(gc.ok && !gc.over_budget && gc.retained <= 4 && gc.pinned >= 1, "100 commits stay bounded with a pinned historical reader");
        expect(fs::exists(f.root + "/rulesets/original/hosts_rules.gz"), "GC cannot delete captured-but-not-open paths");
    }
    expect(text(f.manifest() + ".previous") == "gen-98\n", "last committed predecessor retained");
    expect(write(resume[1], "1", 1) == 1, "release reader");
    int status = 0; expect(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "pinned reader loaded both old files");
    close(ready[0]); close(resume[1]);
    RuleBundleRetention two; two.generations = 2;
    const auto gc = collect_rule_bundles(f.root, two);
    expect(gc.ok && gc.retained == 2 && !fs::exists(f.root + "/rulesets/original"), "released lease can be reclaimed");
    StagedRuleBundle same; std::string error;
    expect(stage_rule_bundle(f.root, "unused", "hosts-gen-99", "adguard-gen-99", same, &error) && same.reused && same.generation == "gen-99", "identical content reuses current");
    expect(commit_rule_bundle(f.manifest(), same, &error) && text(f.manifest() + ".previous") == "gen-98\n", "no-op does not rotate rollback");
    expect(!fs::exists(f.root + "/rulesets/unused"), "no duplicate storage");
    two.bytes = 1;
    const auto mandatory = collect_rule_bundles(f.root, two);
    expect(mandatory.ok && mandatory.over_budget && mandatory.retained == 2, "byte pressure cannot delete current/rollback");
}

void malformed_and_staging() {
    Fixture f; f.publish("one"); f.publish("two");
    Fixture outside; put(outside.root + "/sentinel", "safe");
    expect(!symlink(outside.root.c_str(), (f.root + "/rulesets/link").c_str()), "symlink fixture");
    fs::create_directory(f.root + "/rulesets/unexpected");
    put(f.root + "/rulesets/unexpected/user-data", "preserve");
    fs::create_directory(f.root + "/rulesets/linked-file");
    expect(!symlink((outside.root + "/sentinel").c_str(), (f.root + "/rulesets/linked-file/hosts_rules.gz").c_str()), "file symlink");
    put(f.root + "/rulesets/linked-file/adguard_rules.gz", "a");
    StagedRuleBundle active; std::string error;
    expect(stage_rule_bundle(f.root, "active", "h", "a", active, &error), "active writer");
    RuleBundleRetention p; p.generations = 2; p.max_age_seconds = 0; p.staging_age_seconds = 0;
    auto gc = collect_rule_bundles(f.root, p);
    expect(gc.ok && gc.skipped >= 3 && fs::exists(active.hosts_path), "unexpected entries and active staging protected");
    StagedRuleBundle collision;
    expect(!stage_rule_bundle(f.root, "active", "replace", "replace", collision, &error) && text(active.hosts_path) == "h", "staging collision never overwrites another worker");
    const auto abandoned = active.staging_dir; active = {};
    gc = collect_rule_bundles(f.root, p);
    expect(gc.ok && !fs::exists(abandoned), "abandoned staging reclaimed after lease release");
    fs::create_directory(f.root + "/rulesets/.gc-interrupted");
    put(f.root + "/rulesets/.gc-interrupted/adguard_rules.gz", "half removed");
    expect(collect_rule_bundles(f.root, p).ok && !fs::exists(f.root + "/rulesets/.gc-interrupted"), "interrupted deletion resumes even when one expected file is gone");
    expect(text(outside.root + "/sentinel") == "safe" && text(f.root + "/rulesets/unexpected/user-data") == "preserve", "no foreign data touched");
    put(f.manifest(), "../../outside\n");
    expect(!collect_rule_bundles(f.root, p).ok && !f.read().from_manifest, "bad manifest aborts collection");
    put(f.manifest(), "two\n");
    put(f.manifest() + ".previous", "bad/name\n");
    expect(!collect_rule_bundles(f.root, p).ok, "bad rollback pointer aborts collection");
    expect(!rule_bundle_space_available(f.root, std::numeric_limits<uint64_t>::max(), &error) && !error.empty(), "no space fails closed with reason");
    expect(!rule_bundle_space_available(f.root + "/missing", 1, &error), "statvfs error is not free space");
}

void durability() {
    Fixture f; f.publish("one"); std::string error;
    for (const char *phase : {"file_write", "staging_sync"}) {
        StagedRuleBundle staged; fault(phase);
        expect(!stage_rule_bundle(f.root, "fault", "h", "a", staged, &error), "staging I/O failure rejected");
        clear_fault(); expect(f.read().generation == "one" && !fs::exists(f.root + "/rulesets/.staging-fault"), "failed staging keeps current, removes owned partial");
    }
    for (const char *phase : {"generation_sync", "previous_sync", "current_sync"}) {
        StagedRuleBundle staged;
        expect(stage_rule_bundle(f.root, phase, "h-" + std::string(phase), "a", staged, &error), "fault candidate");
        fault(phase); expect(!commit_rule_bundle(f.manifest(), staged, &error), "commit sync failure not success"); clear_fault();
        expect(f.read().generation == (std::string(phase) == "current_sync" ? phase : "one"), "visible pointer retained even if its directory sync is unconfirmed");
    }
    f.publish("next"); f.publish("latest");
    RuleBundleRetention p; p.generations = 2; p.max_age_seconds = 0;
    fault("gc_rename_sync"); const auto interrupted = collect_rule_bundles(f.root, p); clear_fault();
    expect(interrupted.ok && interrupted.skipped > 0, "GC rename fault retains recoverable tombstones");
    expect(collect_rule_bundles(f.root, p).ok && f.read().generation == "latest", "next pass recovers interrupted GC");
}
}

int main() {
    try { retention_and_readers(); malformed_and_staging(); durability(); }
    catch (const std::exception &error) { clear_fault(); std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    std::cout << "rule generation GC: 100 changes, reader/writer leases, dedup, rollback, bounds, unsafe entries and durability faults passed\n";
}
