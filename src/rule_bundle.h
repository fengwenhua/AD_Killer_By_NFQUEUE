#pragma once

#include <string>
#include <memory>
#include <cstdint>

struct RuleBundlePaths {
    std::string generation;
    std::string hosts_path;
    std::string adguard_path;
    bool from_manifest = false;
    // Cross-process shared lease on the selected directory, acquired before
    // releasing the catalog lock. Keep it alive until both files are loaded.
    std::shared_ptr<void> reader_lease;
};

struct StagedRuleBundle {
    std::string generation;
    std::string staging_dir;
    std::string final_dir;
    std::string hosts_path;
    std::string adguard_path;
    bool reused = false;
    std::shared_ptr<void> writer_lease;
};

struct RuleBundleRetention {
    size_t generations = 4; // current + previous are always protected
    uint64_t bytes = 256ull * 1024 * 1024;
    uint64_t max_age_seconds = 7 * 24 * 60 * 60;
    uint64_t staging_age_seconds = 24 * 60 * 60;
};
struct RuleBundleCollection {
    bool ok = false, over_budget = false;
    size_t removed = 0, retained = 0, pinned = 0, skipped = 0;
    uint64_t retained_bytes = 0;
    std::string error;
};

// Catalog lock -> nonblocking generation lease. Never deletes current,
// previous, active readers/writers, symlinks, or unexpected directory contents.
RuleBundleCollection collect_rule_bundles(const std::string &data_dir,
    const RuleBundleRetention &retention = {}, uint64_t now = 0);

// Read-only free-space gate (also used by staging). An error is not free space.
bool rule_bundle_space_available(const std::string &data_dir, uint64_t required,
    std::string *error = nullptr, uint64_t reserve = 16ull * 1024 * 1024);

// 解析单文件 generation 指针；无有效指针时兼容旧版固定文件。
RuleBundlePaths resolve_rule_bundle(const std::string &data_dir,
                                    const std::string &manifest_path,
                                    const std::string &legacy_hosts,
                                    const std::string &legacy_adguard);

// staging 写完并 fsync 后仍不可见；commit 只通过一次 manifest rename 发布。
bool stage_rule_bundle(const std::string &data_dir,
                       const std::string &generation,
                       const std::string &hosts_gzip,
                       const std::string &adguard_gzip,
                       StagedRuleBundle &staged,
                       std::string *error = nullptr);
bool commit_rule_bundle(const std::string &manifest_path,
                        const StagedRuleBundle &staged,
                        std::string *error = nullptr);
void discard_staged_rule_bundle(const StagedRuleBundle &staged);

#ifdef ADK_RULE_BUNDLE_TESTING
void rule_bundle_test_gate(bool (*gate)(const char *phase));
#endif
