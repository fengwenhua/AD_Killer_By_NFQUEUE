#pragma once

#include "source_cache.h"
#include "source_validator.h"
#include <set>

struct SourceSyncResult {
    SourceValidationResult validation;
    size_t downloaded_bytes = 0, consumed_bytes = 0, consumed_lines = 0;
    std::string provenance = "downloaded", warning;
    int64_t cache_verified_at = 0, cache_fetched_at = 0, cache_age_seconds = 0;
    bool download_failed = false;
};

std::vector<std::string> source_component_urls(const std::string &url);
std::string source_identity_key(const std::string &url);
// Caller supplies an enclosing source/task WorkBudget and holds .update.lock.
// Null cache is the test-only path: fresh requests, no cache reads or writes.
SourceSyncResult synchronize_rule_source(const std::string &directory, const std::string &url,
                                         const RuleStreamLimits &limits, SourceCache *cache,
                                         const std::set<std::string> &protected_keys);
