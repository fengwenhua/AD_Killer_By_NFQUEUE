#pragma once

#include <string>

struct RuleBundlePaths {
    std::string generation;
    std::string hosts_path;
    std::string adguard_path;
    bool from_manifest = false;
};

struct StagedRuleBundle {
    std::string generation;
    std::string staging_dir;
    std::string final_dir;
    std::string hosts_path;
    std::string adguard_path;
};

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
