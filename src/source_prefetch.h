#pragma once
#include "source_sync.h"
#include "work_budget.h"
#include <memory>

constexpr int SYNC_CONCURRENCY_DEFAULT = 10;
constexpr int SYNC_CONCURRENCY_MAX = 20;
int sync_concurrency_value(const std::string &value);

// Only network/file streaming is parallel. Parsing and cache publication run
// on the calling thread, preserving the existing aggregate memory/line bounds.
class SourcePrefetch {
public:
    SourcePrefetch(const std::vector<std::string> &urls, const std::string &directory,
                   SourceCache *cache, int concurrency, uint64_t source_ms, WorkBudget &task);
    ~SourcePrefetch();
    bool next(size_t &index, SourceDownload &download);
    void stop();
    bool started(size_t index) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
