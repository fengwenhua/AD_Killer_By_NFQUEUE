#include "source_cache.h"
#include "net_policy.h"
#include "process_identity.h"
#include "sha256.h"
#include "work_budget.h"
#include "operation_gate.h"
#include "storage_mounts.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
constexpr size_t metadata_limit = 32768, scan_limit = 512;
bool live() { return !WorkBudget::expired() && !operation_closing(); }
#ifdef ADK_SOURCE_CACHE_TESTING
bool (*gate_)(const char *) = nullptr;
bool allowed(const char *phase) { return !gate_ || gate_(phase); }
#else
bool allowed(const char *) { return true; }
#endif
bool hex_key(const std::string &key) { return key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos; }
bool generation_name(const std::string &name) {
    return name.size() > 65 && name.size() <= 128 && hex_key(name.substr(0, 64)) && name[64] == '-' &&
        name.find_first_not_of("0123456789-", 65) == std::string::npos;
}
bool trusted(const struct stat &info, bool directory = false) {
    return (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1) &&
        info.st_uid == geteuid() && !(info.st_mode & 0077);
}
int open_dir(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
    struct stat info {};
    if (fd >= 0 && (fstat(fd, &info) || !trusted(info, true))) { close(fd); return -1; }
    return fd;
}
std::string hex(const std::string &text) {
    const char *alphabet = "0123456789abcdef";
    std::string result;
    for (unsigned char ch : text) { result += alphabet[ch >> 4]; result += alphabet[ch & 15]; }
    return result;
}
bool unhex(const std::string &text, std::string &result) {
    if (text.size() % 2 || text.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    result.clear();
    auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; i < text.size(); i += 2) result += static_cast<char>(nibble(text[i]) * 16 + nibble(text[i + 1]));
    return result.find('\0') == std::string::npos;
}
bool read_at(int directory, const std::string &name, std::string &text, size_t limit) {
    text.clear();
    const int fd = openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info {};
    bool ok = !fstat(fd, &info) && trusted(info) && info.st_size >= 0 && static_cast<uint64_t>(info.st_size) <= limit;
    char buffer[4096];
    while (ok) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (count > 0 && static_cast<size_t>(count) > limit - text.size())) { ok = false; break; }
        if (!count) break;
        text.append(buffer, static_cast<size_t>(count));
    }
    if (close(fd)) ok = false;
    return ok;
}
bool write_at(int directory, const std::string &name, const std::string &text) {
    const int fd = openat(directory, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < text.size()) {
        const auto count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (!ok) unlinkat(directory, name.c_str(), 0);
    return ok;
}
std::string nonce() {
    static uint64_t sequence = 0;
    return std::to_string(static_cast<long>(getpid())) + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
}
bool digest_file(const std::string &path, uint64_t expected, std::string &digest, int copy = -1) {
    if (!expected || expected > MAX_RULE_DOWNLOAD_BYTES) return false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat info {};
    bool ok = !fstat(fd, &info) && trusted(info) && info.st_size >= 0 && static_cast<uint64_t>(info.st_size) == expected;
    Sha256 sha;
    char buffer[65536];
    uint64_t received = 0;
    while (ok && live()) {
        const auto count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (count > 0 && static_cast<uint64_t>(count) > expected - received)) { ok = false; break; }
        if (!count) break;
        sha.update(buffer, count); received += static_cast<uint64_t>(count);
        if (copy >= 0) {
            size_t offset = 0;
            while (offset < static_cast<size_t>(count)) {
                const auto written = write(copy, buffer + offset, static_cast<size_t>(count) - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { ok = false; break; }
                offset += static_cast<size_t>(written);
            }
        }
    }
    ok = ok && live() && received == expected;
    if (close(fd)) ok = false;
    if (ok) digest = sha.finish();
    return ok;
}
std::string metadata(const std::string &key, const std::vector<CachedComponent> &components, int64_t now) {
    std::string text = "ADK_SOURCE_CACHE_1\nkey=" + key + "\nverified_at=" + std::to_string(now) +
        "\ncount=" + std::to_string(components.size()) + "\n";
    for (size_t i = 0; i < components.size(); ++i) {
        const auto &component = components[i];
        const auto suffix = "_" + std::to_string(i) + "=";
        text += "url" + suffix + hex(component.url) + "\neffective" + suffix + hex(component.effective_url) +
            "\netag" + suffix + hex(component.validators.etag) + "\nmodified" + suffix + hex(component.validators.last_modified) +
            "\nbytes" + suffix + std::to_string(component.bytes) + "\ndigest" + suffix + component.digest +
            "\nfetched" + suffix + std::to_string(component.fetched_at) + "\n";
    }
    return text;
}
bool parse_metadata(const std::string &text, const std::string &key, CachedSource &entry) {
    const std::string header = "ADK_SOURCE_CACHE_1\n";
    if (text.compare(0, header.size(), header)) return false;
    std::map<std::string, std::string> fields;
    size_t offset = header.size();
    while (offset < text.size()) {
        const auto end = text.find('\n', offset), split = text.find('=', offset);
        if (end == std::string::npos || split == std::string::npos || split >= end ||
            !fields.emplace(text.substr(offset, split - offset), text.substr(split + 1, end - split - 1)).second) return false;
        offset = end + 1;
    }
    uint64_t count = 0, verified = 0;
    if (fields["key"] != key || !parse_u64(fields["count"], count) || !count || count > 2 ||
        !parse_u64(fields["verified_at"], verified) || !verified || verified > INT64_MAX || fields.size() != 3 + count * 7) return false;
    entry = {}; entry.key = key; entry.verified_at = static_cast<int64_t>(verified);
    for (size_t i = 0; i < count; ++i) {
        CachedComponent component;
        const auto suffix = "_" + std::to_string(i);
        uint64_t fetched = 0;
        if (!unhex(fields["url" + suffix], component.url) || !unhex(fields["effective" + suffix], component.effective_url) ||
            !unhex(fields["etag" + suffix], component.validators.etag) || !unhex(fields["modified" + suffix], component.validators.last_modified) ||
            !validate_download_url(component.url) || !validate_download_url(component.effective_url) || !valid_http_validators(component.validators) ||
            !parse_u64(fields["bytes" + suffix], component.bytes) || !component.bytes || component.bytes > MAX_RULE_DOWNLOAD_BYTES ||
            !parse_u64(fields["fetched" + suffix], fetched) || !fetched || fetched > verified || fetched > INT64_MAX ||
            !hex_key(fields["digest" + suffix])) return false;
        component.digest = fields["digest" + suffix]; component.fetched_at = static_cast<int64_t>(fetched);
        entry.components.push_back(std::move(component));
    }
    return fields.size() == 3 + count * 7;
}
bool pointer(int root, const std::string &key, std::string &generation) {
    std::string text;
    if (!read_at(root, key + ".current", text, 130) || text.empty() || text.back() != '\n') return false;
    text.pop_back();
    if (!generation_name(text) || text.substr(0, 64) != key) return false;
    generation = text; return true;
}
struct Inventory {
    std::string name, key;
    uint64_t bytes = 0;
    int64_t stamp = 0;
};
bool directory_inventory(const std::string &path, uint64_t &bytes, std::vector<std::string> *files = nullptr) {
    const int fd = open_dir(path);
    if (fd < 0) return false;
    DIR *directory = fdopendir(fd);
    if (!directory) { close(fd); return false; }
    bool ok = true;
    size_t count = 0;
    while (true) {
        errno = 0;
        auto *item = readdir(directory);
        if (!item) { if (errno) ok = false; break; }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        if (++count > 128 || (name != "metadata" && name != "body-0" && name != "body-1" &&
            (name.compare(0, 13, "metadata.tmp-") || name.find_first_not_of("0123456789-", 13) != std::string::npos))) { ok = false; break; }
        struct stat info {};
        if (fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) || !trusted(info) || info.st_size < 0 ||
            static_cast<uint64_t>(info.st_size) > (name.compare(0, 4, "body") == 0 ? MAX_RULE_DOWNLOAD_BYTES : metadata_limit)) { ok = false; break; }
        if (name.compare(0, 4, "body") == 0) bytes += static_cast<uint64_t>(info.st_size);
        if (files) files->push_back(name);
    }
    closedir(directory);
    return ok;
}
bool remove_directory(int root, const std::string &root_path, const std::string &name) {
    std::vector<std::string> files;
    uint64_t ignored = 0;
    if (!live() || !directory_inventory(root_path + "/" + name, ignored, &files)) return false;
    const int fd = open_dir(root_path + "/" + name);
    if (fd < 0) return false;
    bool ok = true;
    for (const auto &file : files) if (unlinkat(fd, file.c_str(), 0)) ok = false;
    close(fd);
    return ok && !unlinkat(root, name.c_str(), AT_REMOVEDIR);
}
bool scan(int root, const std::string &path, std::map<std::string, std::string> &references, std::vector<Inventory> &entries) {
    const int copy = openat(root, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (copy < 0) return false;
    DIR *directory = fdopendir(copy);
    if (!directory) { close(copy); return false; }
    size_t count = 0;
    bool ok = true;
    while (true) {
        errno = 0;
        auto *item = readdir(directory);
        if (!item) { if (errno) ok = false; break; }
        if (!live()) { ok = false; break; }
        std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        if (++count > scan_limit) { ok = false; break; }
        if (name.size() == 72 && name.substr(64) == ".current" && hex_key(name.substr(0, 64))) {
            std::string generation;
            if (!pointer(root, name.substr(0, 64), generation)) { ok = false; break; }
            references[name.substr(0, 64)] = generation;
        } else {
            const bool staging = name.compare(0, 5, ".new-") == 0;
            if (staging ? !generation_name(name.substr(5)) : !generation_name(name)) {
                if (name.size() > 77 && hex_key(name.substr(0, 64)) && name.compare(64, 13, ".current.tmp-") == 0 &&
                    name.find_first_not_of("0123456789-", 77) == std::string::npos) {
                    std::string ignored;
                    if (!read_at(root, name, ignored, 130) || unlinkat(root, name.c_str(), 0)) { ok = false; break; }
                    continue;
                }
                ok = false; break;
            }
            Inventory entry; entry.name = name; entry.key = (staging ? name.substr(5) : name).substr(0, 64);
            std::vector<std::string> files;
            if (!directory_inventory(path + "/" + name, entry.bytes, &files)) { ok = false; break; }
            const int child = open_dir(path + "/" + name);
            std::string text;
            CachedSource record;
            if (child >= 0) {
                for (const auto &file : files) if (file.compare(0, 13, "metadata.tmp-") == 0 && unlinkat(child, file.c_str(), 0)) ok = false;
                if (read_at(child, "metadata", text, metadata_limit) && parse_metadata(text, entry.key, record)) entry.stamp = record.verified_at;
                close(child);
            }
            if (!ok) break;
            entries.push_back(std::move(entry));
        }
    }
    closedir(directory);
    return ok;
}
} // namespace

#ifdef ADK_SOURCE_CACHE_TESTING
void source_cache_test_gate(bool (*gate)(const char *)) { gate_ = gate; }
#endif

bool hash_source_component(CachedComponent &component) {
    std::string digest;
    if (!digest_file(component.path, component.bytes, digest)) return false;
    component.digest = std::move(digest); return true;
}
bool source_component_intact(const CachedComponent &component) {
    std::string digest;
    return hex_key(component.digest) && digest_file(component.path, component.bytes, digest) && digest == component.digest;
}

std::string source_cache_key(const std::string &identity, const std::string &format, const std::vector<std::string> &urls) {
    Sha256 sha;
    const auto add = [&](const std::string &text) { sha.update(text.data(), text.size()); const char zero = 0; sha.update(&zero, 1); };
    add("ADK_SOURCE_CACHE_1_recipe1"); add(identity); add(format);
    for (const auto &url : urls) add(url);
    return sha.finish();
}
SourceCache::SourceCache(const std::string &data, SourceCachePolicy policy) : path_(data + "/source_cache"), policy_(policy) {
    if (!live() || !policy.bytes || !policy.entries || policy.entries > 65 || policy.fallback_seconds < 0 ||
        policy.retention_seconds < policy.fallback_seconds || (mkdir(path_.c_str(), 0700) && errno != EEXIST)) return;
    root_ = open_dir(path_);
    if (root_ >= 0 && (flock(root_, LOCK_EX | LOCK_NB) || !no_storage_mounts(path_))) { close(root_); root_ = -1; }
}
SourceCache::~SourceCache() { if (root_ >= 0) close(root_); }

bool SourceCache::load(const std::string &key, const std::vector<std::string> &urls, int64_t now,
                       CachedSource &entry, std::string &error) const {
    entry = {}; error = "cache_missing_or_untrusted";
    if (root_ < 0 || !live() || !hex_key(key) || urls.empty() || urls.size() > 2 || !no_storage_mounts(path_)) return false;
    std::string generation;
    if (!pointer(root_, key, generation)) return false;
    const int directory = open_dir(path_ + "/" + generation);
    if (directory < 0) return false;
    std::string text;
    bool ok = read_at(directory, "metadata", text, metadata_limit) && parse_metadata(text, key, entry);
    close(directory);
    if (!ok || entry.components.size() != urls.size() || entry.verified_at > now || now <= 0 ||
        now - entry.verified_at > policy_.retention_seconds) { entry = {}; error = "cache_schema_or_age"; return false; }
    entry.generation = generation;
    for (size_t i = 0; i < urls.size(); ++i) {
        auto &component = entry.components[i];
        component.path = path_ + "/" + generation + "/body-" + std::to_string(i);
        std::string digest;
        if (component.url != urls[i] || !digest_file(component.path, component.bytes, digest) || digest != component.digest) {
            entry = {}; error = "cache_integrity_or_identity"; return false;
        }
    }
    error.clear(); return true;
}

bool SourceCache::collect(int64_t now, const std::set<std::string> &protected_keys, std::string &error) {
    error = "cache_catalogue_untrusted_or_busy";
    if (root_ < 0 || !live() || !no_storage_mounts(path_) || fsync(root_)) return false;
    std::map<std::string, std::string> references;
    std::vector<Inventory> entries;
    if (!scan(root_, path_, references, entries)) return false;
    uint64_t bytes = 0;
    std::vector<Inventory> current;
    for (const auto &entry : entries) {
        if (references[entry.key] != entry.name) {
            if (!remove_directory(root_, path_, entry.name)) return false;
        } else { bytes += entry.bytes; current.push_back(entry); }
    }
    std::sort(current.begin(), current.end(), [](const Inventory &a, const Inventory &b) {
        return a.stamp != b.stamp ? a.stamp < b.stamp : a.name < b.name;
    });
    size_t count = current.size();
    for (const auto &entry : current) {
        const bool expired = entry.stamp > 0 && entry.stamp <= now && now - entry.stamp > policy_.retention_seconds;
        if (protected_keys.count(entry.key) || (!expired && bytes <= policy_.bytes && count <= policy_.entries)) continue;
        if (unlinkat(root_, (entry.key + ".current").c_str(), 0) || fsync(root_) || !remove_directory(root_, path_, entry.name)) return false;
        bytes -= entry.bytes; --count;
    }
    if (fsync(root_)) return false;
    if (bytes > policy_.bytes || count > policy_.entries) { error = "cache_protected_entries_over_quota"; return false; }
    error.clear(); return true;
}

bool SourceCache::commit(const std::string &key, const std::vector<CachedComponent> &inputs, int64_t now,
                         const std::set<std::string> &protected_keys, std::string &error) {
    error = "cache_write_rejected";
    if (root_ < 0 || !live() || !hex_key(key) || now <= 0 || inputs.empty() || inputs.size() > 2) return false;
    auto protected_all = protected_keys; protected_all.insert(key);
    if (!collect(now, protected_all, error)) return false;
    error = "cache_transaction_failed";
    std::map<std::string, std::string> references;
    std::vector<Inventory> entries;
    if (!scan(root_, path_, references, entries)) return false;
    uint64_t other_bytes = 0, new_bytes = 0;
    size_t other_count = 0;
    for (const auto &entry : entries) if (references[entry.key] == entry.name && entry.key != key) { other_bytes += entry.bytes; ++other_count; }
    for (const auto &input : inputs) {
        if (!validate_download_url(input.url) || !validate_download_url(input.effective_url) || !valid_http_validators(input.validators) ||
            !input.bytes || input.bytes > MAX_RULE_DOWNLOAD_BYTES || input.fetched_at <= 0 || input.fetched_at > now) return false;
        new_bytes += input.bytes;
    }
    if (new_bytes > policy_.bytes) { error = "cache_entry_exceeds_quota"; return false; }
    std::sort(entries.begin(), entries.end(), [](const Inventory &a, const Inventory &b) {
        return a.stamp != b.stamp ? a.stamp < b.stamp : a.name < b.name;
    });
    for (const auto &entry : entries) {
        const bool needs_room = new_bytes > policy_.bytes || other_bytes > policy_.bytes - new_bytes || other_count >= policy_.entries;
        if (!needs_room) break;
        if (entry.key == key || protected_all.count(entry.key) || references[entry.key] != entry.name) continue;
        if (unlinkat(root_, (entry.key + ".current").c_str(), 0) || fsync(root_) || !remove_directory(root_, path_, entry.name)) return false;
        other_bytes -= entry.bytes; --other_count;
    }
    if (new_bytes > policy_.bytes || other_bytes > policy_.bytes - new_bytes || other_count >= policy_.entries) {
        error = "cache_quota_no_room_for_source"; return false;
    }
    std::string old_generation;
    const bool old = pointer(root_, key, old_generation);
    bool same = old;
    for (size_t i = 0; i < inputs.size(); ++i)
        same = same && inputs[i].path == path_ + "/" + old_generation + "/body-" + std::to_string(i);
    const auto token = nonce();
    const std::string generation = key + "-" + token, staging = ".new-" + generation;
    int directory = -1;
    if (same) directory = open_dir(path_ + "/" + old_generation);
    else {
        struct statvfs space {};
        if (fstatvfs(root_, &space) || !space.f_frsize ||
            static_cast<uint64_t>(space.f_bavail) < (new_bytes + 16u * 1024u * 1024u + space.f_frsize - 1) / space.f_frsize ||
            mkdirat(root_, staging.c_str(), 0700)) { error = "cache_storage_reserve"; return false; }
        directory = open_dir(path_ + "/" + staging);
    }
    if (directory < 0) return false;
    auto components = inputs;
    bool ok = true;
    for (size_t i = 0; i < components.size() && ok; ++i) {
        const auto name = "body-" + std::to_string(i);
        int copy = -1;
        if (!same && allowed("body_copy")) copy = openat(directory, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (!same && copy < 0) { ok = false; break; }
        std::string digest;
        ok = digest_file(components[i].path, components[i].bytes, digest, copy);
        if (copy >= 0) { if (fsync(copy)) ok = false; if (close(copy)) ok = false; }
        if (!components[i].digest.empty() && components[i].digest != digest) ok = false;
        components[i].digest = digest;
    }
    const auto text = metadata(key, components, now);
    const auto meta_name = same ? "metadata.tmp-" + token : "metadata";
    ok = ok && live() && text.size() <= metadata_limit && allowed("metadata_write") && write_at(directory, meta_name, text);
    if (ok && same) ok = !renameat(directory, meta_name.c_str(), directory, "metadata");
    if (ok) ok = !fsync(directory);
    close(directory);
    if (same) {
        if (!ok) { const int cleanup = open_dir(path_ + "/" + old_generation); if (cleanup >= 0) { unlinkat(cleanup, meta_name.c_str(), 0); close(cleanup); } }
        if (ok) error.clear();
        return ok;
    }
    if (!ok) { remove_directory(root_, path_, staging); return false; }
    struct stat exists {};
    if (!fstatat(root_, generation.c_str(), &exists, AT_SYMLINK_NOFOLLOW) || errno != ENOENT ||
        renameat(root_, staging.c_str(), root_, generation.c_str()) || fsync(root_)) return false;
    const auto temporary = key + ".current.tmp-" + token;
    if (!write_at(root_, temporary, generation + "\n")) return false;
    if (!live() || !allowed("manifest_rename") || renameat(root_, temporary.c_str(), root_, (key + ".current").c_str())) {
        unlinkat(root_, temporary.c_str(), 0); return false;
    }
    if (!allowed("root_sync_after_manifest") || fsync(root_)) { error = "cache_manifest_visible_durability_unknown"; return false; }
    return collect(now, protected_all, error);
}
