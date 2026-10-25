#include "source_cache.h"
#include "work_budget.h"
#include "operation_gate.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
static std::string failure;
static bool gate(const char *phase) { return failure != phase; }
static void write_file(const fs::path &path, const std::string &text) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    assert(!close(fd));
}
static std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
static CachedComponent input(const fs::path &path, const std::string &url, int64_t at = 1000) {
    CachedComponent component;
    component.path = path; component.url = component.effective_url = url;
    component.bytes = fs::file_size(path); component.fetched_at = at;
    component.validators.etag = "\"v1\"";
    return component;
}
int main() {
    char directory[] = "/tmp/adk-source-cache-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path base(directory);
    // A normal absent uninstall marker sets errno=ENOENT on each cancellation
    // poll. Directory iteration must test readdir's errno, not that poll's.
    operation_marker_path = (base / "closing").string();
    const auto pro = base / "pro", fake = base / "fake";
    write_file(pro, "keep.example.org\nfake.example.org\n");
    write_file(fake, "fake.example.org\n");
    const std::vector<std::string> urls {"https://example.org/pro", "https://example.org/fake"};
    const auto key = source_cache_key("builtin:test", "hagezi", urls);
    assert(key != source_cache_key("builtin:test", "auto", urls));
    assert(key != source_cache_key("builtin:test", "hagezi", {urls[1], urls[0]}));
    {
        SourceCache cache(base);
        assert(cache.available());
        SourceCache busy(base);
        assert(!busy.available());
        std::string error;
        CachedSource entry;
        assert(!cache.load(key, urls, 1000, entry, error));
        assert(cache.commit(key, {input(pro, urls[0]), input(fake, urls[1])}, 1000, {key}, error));
        assert(cache.load(key, urls, 1001, entry, error) && entry.components.size() == 2 && entry.fresh(1001));
        const auto first = entry.generation;
        assert(read_file(entry.components[0].path) == read_file(pro));
        assert(!cache.load(key, {urls[1], urls[0]}, 1001, entry, error));
        assert(cache.load(key, urls, 1001, entry, error));
        assert(cache.load(key, urls, 1000 + 86401, entry, error) && !entry.fresh(1000 + 86401));
        assert(!cache.load(key, urls, 999, entry, error));
        assert(!cache.load(key, urls, 1000 + 7 * 86400 + 1, entry, error));
        assert(cache.load(key, urls, 1001, entry, error));
        // Both successful 304s refresh one metadata record, without copying
        // body files or changing the committed group identity.
        assert(cache.commit(key, entry.components, 2000, {key}, error));
        assert(cache.load(key, urls, 2001, entry, error) && entry.generation == first && entry.verified_at == 2000);
        assert(entry.components[0].fetched_at == 1000);
        const auto old_manifest = read_file(base / "source_cache" / (key + ".current"));
        source_cache_test_gate(gate);
        for (const char *phase : {"body_copy", "metadata_write", "manifest_rename"}) {
            failure = phase;
            assert(!cache.commit(key, {input(pro, urls[0], 2000), input(fake, urls[1], 2000)}, 2000, {key}, error));
            assert(!error.empty() && read_file(base / "source_cache" / (key + ".current")) == old_manifest);
            assert(cache.load(key, urls, 2001, entry, error) && entry.generation == first);
        }
        // Public visibility after rename is uncertain on a failed directory
        // sync. It is still a complete group, never an old/new component mix.
        failure = "root_sync_after_manifest";
        write_file(pro, "new.example.org\n");
        assert(!cache.commit(key, {input(pro, urls[0], 2000), input(fake, urls[1], 2000)}, 2000, {key}, error));
        assert(error == "cache_manifest_visible_durability_unknown");
        assert(cache.load(key, urls, 2001, entry, error) && entry.generation != first);
        assert(read_file(entry.components[0].path) == "new.example.org\n");
        failure.clear(); source_cache_test_gate(nullptr);
        assert(cache.collect(2001, {key}, error));
        size_t directories = 0;
        for (const auto &path : fs::directory_iterator(base / "source_cache")) if (path.is_directory()) ++directories;
        assert(directories == 1);
        // Corrupt body, same size: digest rather than size detects it.
        const auto body = entry.components[0].path;
        write_file(body, std::string(fs::file_size(body), 'x'));
        assert(!cache.load(key, urls, 2001, entry, error) && error == "cache_integrity_or_identity");
        assert(cache.commit(key, {input(pro, urls[0], 2000), input(fake, urls[1], 2000)}, 2000, {key}, error));
        assert(cache.load(key, urls, 2001, entry, error));
        const auto symlink_body = entry.components[0].path;
        fs::remove(symlink_body);
        fs::create_symlink(pro, symlink_body);
        assert(!cache.load(key, urls, 2001, entry, error));
        assert(!cache.collect(2001, {key}, error) && fs::exists(pro));
        fs::remove(symlink_body);
    }
    fs::remove_all(base / "source_cache");
    {
        SourceCachePolicy policy; policy.bytes = 24; policy.entries = 2;
        SourceCache cache(base, policy);
        std::string error;
        CachedSource entry;
        const auto small = base / "small";
        write_file(small, "12345678");
        std::vector<std::string> keys;
        for (int i = 0; i < 3; ++i) {
            const auto url = "https://example.org/" + std::to_string(i);
            const auto id = source_cache_key(url, "auto", {url}); keys.push_back(id);
            assert(cache.commit(id, {input(small, url)}, 1000 + i, {id}, error));
        }
        assert(!cache.load(keys[0], {"https://example.org/0"}, 1003, entry, error));
        assert(cache.load(keys[1], {"https://example.org/1"}, 1003, entry, error));
        const auto protected_manifest = read_file(base / "source_cache" / (keys[1] + ".current"));
        const auto extra = source_cache_key("extra", "auto", {"https://example.org/extra"});
        assert(!cache.commit(extra, {input(small, "https://example.org/extra")}, 1003, {keys[1], keys[2]}, error));
        assert(read_file(base / "source_cache" / (keys[1] + ".current")) == protected_manifest);
        { WorkBudget expired(0, 100); assert(!cache.load(keys[1], {"https://example.org/1"}, 1003, entry, error)); }
        write_file(base / "source_cache" / "unknown", "do not delete");
        assert(!cache.collect(1004, {}, error));
        assert(read_file(base / "source_cache" / "unknown") == "do not delete");
    }
    fs::remove_all(base);
    operation_marker_path.clear();
    std::cout << "source cache: complete paired transactions, 304 refresh, TTL/rollback, digests, locks, quota/protection, corruption/symlinks and crash recovery passed\n";
}
