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
static std::string failed_endpoint, unsolicited_304;
static std::vector<std::pair<std::string, std::string>> requests;
static void put(const std::string &path, const std::string &body) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()));
    assert(!close(fd));
}
DownloadResponse download_file_response(const std::string &url, const std::string &path, const HttpValidators &validators, bool) {
    requests.emplace_back(url, validators.etag);
    DownloadResponse response;
    response.effective_url = url; response.validators.etag = "\"v1\"";
    if (url == failed_endpoint) { response.error = "HTTP 503"; response.http_status = 503; return response; }
    if (url == unsolicited_304) { response.http_status = 304; response.not_modified = true; return response; }
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
    corrupt.clear();
    {
        WorkBudget task(10000, 1024 * 1024);
        SourceCache cache(base);
        const std::string original = "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts";
        const auto endpoints = rule_source_endpoints(original);
        const auto known_key = source_identity_key(original);
        assert(known_key == source_identity_key(endpoints[0]) && known_key == source_identity_key(endpoints[1]));
        const auto attempt = [&](unsigned number, bool reuse = false) {
            auto download = prepare_source_download(downloads, original, &cache);
            download.attempt = number; download.max_attempts = 3; download.managed_retries = true;
            download.reuse_fresh_cache = reuse;
            if (!reuse) fetch_source_download(original, download, true);
            return synchronize_rule_source(downloads, original, {}, &cache, {known_key}, &download, false);
        };
        requests.clear(); failed_endpoint = endpoints[0];
        assert(!attempt(1).validation.accepted);
        const auto fallback = attempt(2);
        assert(fallback.validation.accepted && fallback.validation.hosts_rules == "good.example.org\n");
        assert(fallback.effective_urls == std::vector<std::string>({endpoints[1]}));
        assert(requests.size() == 2 && requests[0].first == endpoints[0] && requests[1].first == endpoints[1]);
        assert(requests[0].second.empty() && requests[1].second.empty());
        failed_endpoint.clear(); unsolicited_304 = endpoints[0];
        const auto foreign_304 = attempt(1);
        assert(!foreign_304.validation.accepted && foreign_304.validation.reason == "304_without_matching_endpoint");
        assert(requests.size() == 3 && requests.back().second.empty());
        unsolicited_304.clear();
        const auto switched = attempt(1);
        assert(switched.validation.accepted && !switched.validation.hosts_rules.empty() && requests.back().second.empty());
        const auto conditional = attempt(1);
        assert(conditional.validation.accepted && conditional.provenance == "revalidated" && requests.back().second == "\"v1\"");
        failed_endpoint = endpoints[0];
        assert(!attempt(1).validation.accepted);
        const auto alternate = attempt(2);
        assert(alternate.validation.accepted && requests.back().first == endpoints[1] && requests.back().second.empty());
        const auto before = requests.size();
        const auto reused = attempt(0, true);
        assert(reused.validation.accepted && reused.provenance == "cache_fresh" && requests.size() == before);
        assert(reused.effective_urls == std::vector<std::string>({endpoints[1]}));
    }
    operation_marker_path.clear();
    fs::remove_all(base);
    if (!rejected) return 1;
    std::cout << "source sync: cache integrity, mirror identity/failover, endpoint-bound validators, unsolicited 304 and zero-request fresh reuse passed\n";
}
