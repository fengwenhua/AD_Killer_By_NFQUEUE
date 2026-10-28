#pragma once

#include "rule_stream.h"
#include "rule_recipe.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

// Own validated, LF-terminated chunks without a whole-category text copy.
// Small allocations may coalesce when combined text is at most 8 MiB.
// Sorting uses views into these immutable chunks; successful compression releases
// the source buffers only after zlib has consumed every view. No temporary files.
class CanonicalRuleChunks {
public:
    explicit CanonicalRuleChunks(size_t byte_limit = MAX_RULE_DECOMPRESSED_BYTES,
                                 size_t line_limit = MAX_RULE_LINES)
        : byte_limit_(byte_limit), line_limit_(line_limit) {}
    CanonicalRuleChunks(const CanonicalRuleChunks &) = delete;
    CanonicalRuleChunks &operator=(const CanonicalRuleChunks &) = delete;
    bool append(std::string &&chunk);
    bool empty() const { return bytes_ == 0; }
    size_t size() const { return bytes_; }
    bool compress_gzip(std::string_view prefix, std::string &output);

private:
    std::vector<std::string> chunks_;
    size_t bytes_ = 0, lines_ = 0;
    size_t byte_limit_, line_limit_;
};

std::string serialize_source_reports(const std::vector<SourceUpdateReport> &reports,
                                     const std::string &publish_status,
                                     int64_t updated_at,
                                     const PublishedSourceStats &published = {},
                                     const std::string &job_id = {});
