#pragma once
#include <string>

// Each sync owns a private temporary directory; its shared directory lease is
// inherited by all actual writers, including curl after exec. GC only removes
// a known directory after acquiring an independent exclusive lease.
class SourceStaging {
public:
    explicit SourceStaging(const std::string &data);
    ~SourceStaging();
    SourceStaging(const SourceStaging &) = delete;
    SourceStaging &operator=(const SourceStaging &) = delete;
    bool ok() const { return lease_ >= 0; }
    const std::string &path() const { return path_; }
private:
    std::string root_, name_, path_;
    int lease_ = -1, previous_ = -1;
};
