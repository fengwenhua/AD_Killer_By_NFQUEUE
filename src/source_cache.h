#pragma once

#include "http_validators.h"
#include <cstdint>
#include <set>
#include <string>
#include <vector>

struct SourceCachePolicy {
    uint64_t bytes = 256u * 1024u * 1024u;
    size_t entries = 65;
    int64_t fallback_seconds = 24 * 60 * 60;
    int64_t retention_seconds = 7 * 24 * 60 * 60;
};
struct CachedComponent {
    std::string url, effective_url, path, digest;
    HttpValidators validators;
    uint64_t bytes = 0;
    int64_t fetched_at = 0;
};
struct CachedSource {
    std::string key, generation;
    std::vector<CachedComponent> components;
    int64_t verified_at = 0;
    bool fresh(int64_t now, int64_t ttl = 86400) const {
        return verified_at > 0 && verified_at <= now && now - verified_at <= ttl;
    }
};
bool hash_source_component(CachedComponent &component);
bool source_component_intact(const CachedComponent &component);
std::string source_cache_key(const std::string &identity, const std::string &format,
                             const std::vector<std::string> &urls);

// Construct only for formal synchronization while .update.lock and the module
// operation lease are held. An additional directory flock serializes cache
// transactions/GC. Source tests and prefix previews never instantiate it.
class SourceCache {
public:
    explicit SourceCache(const std::string &data, SourceCachePolicy policy = {});
    ~SourceCache();
    SourceCache(const SourceCache &) = delete;
    SourceCache &operator=(const SourceCache &) = delete;
    bool available() const { return root_ >= 0; }
    bool load(const std::string &key, const std::vector<std::string> &urls,
              int64_t now, CachedSource &entry, std::string &error) const;
    // Inputs must already pass whole-source validation. Components may be
    // freshly downloaded files or verified 304 representations from this cache.
    bool commit(const std::string &key, const std::vector<CachedComponent> &inputs,
                int64_t now, const std::set<std::string> &protected_keys, std::string &error);
    bool collect(int64_t now, const std::set<std::string> &protected_keys, std::string &error);
    const SourceCachePolicy &policy() const { return policy_; }
private:
    std::string path_;
    int root_ = -1;
    SourceCachePolicy policy_;
};

#ifdef ADK_SOURCE_CACHE_TESTING
void source_cache_test_gate(bool (*gate)(const char *));
#endif
