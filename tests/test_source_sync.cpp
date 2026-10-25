#include "source_sync.h"
#include "net.h"
#include "operation_gate.h"
#include "work_budget.h"
#include <cassert>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace fs = std::filesystem;
static std::string corrupt;
static void put(const std::string &path, const std::string &body) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()));
    assert(!close(fd));
}
DownloadResponse download_file_response(const std::string &url, const std::string &path, const HttpValidators &validators) {
    DownloadResponse response;
    response.effective_url = url; response.validators.etag = "\"v1\"";
    if (validators.etag == "\"v1\"") {
        // Simulate persistent storage damage during a conditional request,
        // after cache admission checked its hash and before rule validation.
        if (!corrupt.empty()) put(corrupt, "evil.example.org\n");
        response.http_status = 304; response.not_modified = true;
    } else {
        put(path, "good.example.org\n");
        response.http_status = 200; response.bytes = 17;
    }
    return response;
}
int main() {
    char temporary[] = "/tmp/adk-source-sync-XXXXXX";
    assert(mkdtemp(temporary));
    const fs::path base(temporary), downloads = base / "downloads";
    fs::create_directory(downloads);
    operation_marker_path = (base / "closing").string();
    const std::string url = "https://example.org/source";
    const auto key = source_identity_key(url);
    bool rejected = false;
    {
        WorkBudget task(10000, 1024 * 1024);
        SourceCache cache(base);
        assert(cache.available());
        const auto fresh = synchronize_rule_source(downloads, url, {}, &cache, {key});
        assert(fresh.validation.accepted && fresh.validation.hosts_rules == "good.example.org\n");
        CachedSource entry; std::string error;
        assert(cache.load(key, {url}, time(nullptr), entry, error));
        corrupt = entry.components[0].path;
        const auto changed = synchronize_rule_source(downloads, url, {}, &cache, {key});
        rejected = !changed.validation.accepted;
        if (!rejected) std::cerr << "cache admission hash did not bind the parsed representation: " << changed.validation.hosts_rules;
    }
    operation_marker_path.clear();
    fs::remove_all(base);
    if (!rejected) return 1;
    std::cout << "source synchronization rejects cache content changed after admission and before a 304 validation\n";
}
