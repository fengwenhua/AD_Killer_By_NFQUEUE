#pragma once
#include "source_sync.h"
#include "work_budget.h"
#include <memory>
#include <functional>

constexpr int SYNC_CONCURRENCY_DEFAULT = 10;
constexpr int SYNC_CONCURRENCY_MAX = 20;
int sync_concurrency_value(const std::string &value);
constexpr int SYNC_INTERVAL_DEFAULT = 30, SYNC_INTERVAL_MAX = 1440;
constexpr int SYNC_RETRIES_DEFAULT = 2, SYNC_RETRIES_MAX = 5;
int sync_interval_value(const std::string &value);
int sync_retries_value(const std::string &value);
using SourcePrefetchProgress = std::function<void(size_t, bool, unsigned)>;

// Only network/file streaming is parallel. Parsing and cache publication run
// on the calling thread, preserving the existing aggregate memory/line bounds.
class SourcePrefetch {
public:
    SourcePrefetch(const std::vector<std::string> &urls, const std::string &directory,
                   SourceCache *cache, int concurrency, uint64_t source_ms, WorkBudget &task,
                   int fresh_seconds = 0, unsigned retries = 0,
                   const std::set<std::string> &retry_urls = {}, SourcePrefetchProgress progress = {});
    ~SourcePrefetch();
    bool next(size_t &index, SourceDownload &download);
    void stop();
    bool started(size_t index) const;
    bool retry(size_t index, uint64_t elapsed_ms, uint64_t downloaded_bytes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
