#pragma once

#include "rule_stream.h"
#include "rule_recipe.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SourceRuleStats {
    size_t bytes = 0;
    size_t total_lines = 0;
    size_t ignored_lines = 0;
    size_t candidate_lines = 0;
    size_t accepted_hosts = 0;
    size_t accepted_adguard = 0;
    size_t rejected_unsupported = 0;
    size_t rejected_too_long = 0;
    uint32_t valid_ratio_ppm = 0;
    size_t difference_base_unique = 0;
    size_t difference_exclude_unique = 0;
    size_t difference_removed = 0;
};

struct SourceValidationResult {
    bool accepted = false;
    std::string reason;
    std::string hosts_rules;
    std::string adguard_rules;
    SourceRuleStats stats;
};

struct SourceUpdateReport {
    std::string url;
    std::string status;
    std::string reason;
    size_t downloaded_bytes = 0;
    SourceRuleStats stats;
    std::string format = "auto";
    int64_t checked_at = 0;
    std::string provenance = "downloaded", warning;
    int64_t cache_verified_at = 0, cache_fetched_at = 0, cache_age_seconds = 0;
};

struct PublishedSourceStats {
    std::string generation;
    size_t unique_black = 0;
    size_t unique_white = 0;
};

SourceValidationResult validate_rule_source(const std::string &content,
                                           SourceFormat format = SourceFormat::AUTO);
SourceValidationResult validate_rule_source_file(
    const char *path, const RuleStreamLimits &limits = {},
    SourceFormat format = SourceFormat::AUTO);

// Canonical, sorted unique lines (no header); used by live and offline merges.
void deduplicate_rule_text(std::string &text);

std::string serialize_source_reports(const std::vector<SourceUpdateReport> &reports,
                                     const std::string &publish_status,
                                     int64_t updated_at,
                                     const PublishedSourceStats &published = {},
                                     const std::string &job_id = {});
