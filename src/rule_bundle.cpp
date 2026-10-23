#include "rule_bundle.h"
#include "process_identity.h"
#include "rule_stream.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {
#ifdef ADK_RULE_BUNDLE_TESTING
bool (*test_gate)(const char *) = nullptr;
bool allowed(const char *phase) { return !test_gate || test_gate(phase); }
#else
bool allowed(const char *) { return true; }
#endif
constexpr const char *hosts_name = "hosts_rules.gz", *adguard_name = "adguard_rules.gz";
void set_error(std::string *error, const std::string &message) { if (error) *error = message; }
bool valid_generation(const std::string &name) {
    if (name.empty() || name.size() > 128 || name[0] == '.') return false;
    for (unsigned char ch : name)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    return true;
}
struct Directory {
    int fd = -1;
    explicit Directory(const std::string &path) {
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        struct stat info {};
        if (fd >= 0 && (fstat(fd, &info) || !S_ISDIR(info.st_mode) ||
            info.st_uid != geteuid() || (info.st_mode & 0022))) { close(fd); fd = -1; }
    }
    ~Directory() { if (fd >= 0) close(fd); }
    bool lock(int operation, unsigned timeout = 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        while (fd >= 0) {
            if (flock(fd, operation | LOCK_NB) == 0) return true;
            if ((errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) ||
                !timeout || std::chrono::steady_clock::now() >= deadline) return false;
            usleep(1000);
        }
        return false;
    }
};
using Lease = std::shared_ptr<Directory>;
Lease directory(const std::string &path, int operation, unsigned timeout = 0) {
    auto lease = std::make_shared<Directory>(path);
    return lease->lock(operation, timeout) ? lease : Lease{};
}
bool ensure_directory(const std::string &path, mode_t mode) {
    return mkdir(path.c_str(), mode) == 0 || (errno == EEXIST && Directory(path).fd >= 0);
}
bool sync_directory(const std::string &path, const char *phase = "directory_sync") {
    Directory dir(path);
    return allowed(phase) && dir.fd >= 0 && fsync(dir.fd) == 0;
}
std::string parent_directory(const std::string &path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}
bool write_new_file(const std::string &path, const std::string &data) {
    if (!allowed("file_write")) return false;
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    bool ok = true;
    size_t offset = 0;
    while (offset < data.size()) {
        const auto count = write(fd, data.data() + offset, data.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (!ok) unlink(path.c_str());
    return ok;
}
bool pointer(const std::string &path, std::string &name, bool &missing) {
    std::string text;
    const auto result = read_regular_bounded(path, text, 160);
    missing = result == BoundedRead::MISSING;
    if (result != BoundedRead::OK || text.empty()) return false;
    if (text.back() == '\n') text.pop_back();
    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
    if (!valid_generation(text)) return false;
    name = text;
    return true;
}
bool replace_pointer(const std::string &path, const std::string &name, bool &visible, const char *phase) {
    visible = false;
    const auto temp = path + ".tmp." + std::to_string(static_cast<long>(getpid()));
    // Never truncate or unlink a preexisting unknown temp object.
    if (!write_new_file(temp, name + "\n")) return false;
    if (rename(temp.c_str(), path.c_str())) { unlink(temp.c_str()); return false; }
    visible = true;
    return sync_directory(parent_directory(path), phase);
}
bool file_info(int fd, const char *name, uint64_t &bytes, bool optional = false) {
    struct stat info {};
    if (fstatat(fd, name, &info, AT_SYMLINK_NOFOLLOW)) return optional && errno == ENOENT;
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > MAX_RULE_INPUT_BYTES) return false;
    bytes += static_cast<uint64_t>(info.st_size);
    return true;
}
// Only the two expected regular files, no recursion or symlink traversal.
bool inventory(int fd, bool staging, uint64_t &bytes) {
    bytes = 0;
    if (!file_info(fd, hosts_name, bytes, staging) || !file_info(fd, adguard_name, bytes, staging)) return false;
    const int copy = openat(fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    DIR *entries = copy >= 0 ? fdopendir(copy) : nullptr;
    if (!entries) { if (copy >= 0) close(copy); return false; }
    bool ok = true; errno = 0;
    while (auto *entry = readdir(entries)) {
        const std::string name = entry->d_name;
        if (name != "." && name != ".." && name != hosts_name && name != adguard_name) { ok = false; break; }
        errno = 0;
    }
    if (errno) ok = false;
    closedir(entries);
    return ok;
}
bool remove_bundle_dir(const std::string &path, bool staging, const std::string &trash = "") {
    auto lease = directory(path, LOCK_EX);
    uint64_t bytes = 0;
    if (!lease || !inventory(lease->fd, staging, bytes)) return false;
    if (!trash.empty()) {
        struct stat info {};
        if (!lstat(trash.c_str(), &info) || errno != ENOENT || rename(path.c_str(), trash.c_str())) return false;
        if (!sync_directory(parent_directory(path), "gc_rename_sync")) return false;
    }
    for (const char *name : {hosts_name, adguard_name})
        if (unlinkat(lease->fd, name, 0) && errno != ENOENT) return false;
    return rmdir((trash.empty() ? path : trash).c_str()) == 0;
}
bool same_file(const std::string &path, const std::string &expected) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat info {};
    if (fd < 0) return false;
    bool equal = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size >= 0 &&
                 static_cast<uint64_t>(info.st_size) == expected.size();
    size_t offset = 0; char buffer[16384];
    while (equal && offset < expected.size()) {
        const auto count = read(fd, buffer, std::min(sizeof(buffer), expected.size() - offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || memcmp(buffer, expected.data() + offset, static_cast<size_t>(count))) equal = false;
        else offset += static_cast<size_t>(count);
    }
    close(fd);
    return equal;
}
} // namespace

#ifdef ADK_RULE_BUNDLE_TESTING
void rule_bundle_test_gate(bool (*gate)(const char *)) { test_gate = gate; }
#endif

RuleBundlePaths resolve_rule_bundle(const std::string &data_dir, const std::string &manifest_path,
                                    const std::string &legacy_hosts, const std::string &legacy_adguard) {
    RuleBundlePaths paths;
    paths.hosts_path = legacy_hosts; paths.adguard_path = legacy_adguard;
    auto catalog = directory(data_dir + "/rulesets", LOCK_SH, 250);
    if (!catalog) return paths;
    std::string generation; bool missing = false;
    if (!pointer(manifest_path, generation, missing)) return paths;
    const auto base = data_dir + "/rulesets/" + generation;
    auto lease = directory(base, LOCK_SH);
    uint64_t bytes = 0;
    if (!lease || !file_info(lease->fd, hosts_name, bytes) || !file_info(lease->fd, adguard_name, bytes)) return paths;
    paths.generation = generation; paths.hosts_path = base + "/" + hosts_name;
    paths.adguard_path = base + "/" + adguard_name;
    paths.from_manifest = true; paths.reader_lease = std::move(lease);
    return paths;
}

bool rule_bundle_space_available(const std::string &data_dir, uint64_t required,
                                 std::string *error, uint64_t reserve) {
    struct statvfs info {};
    if (statvfs(data_dir.c_str(), &info) || !info.f_frsize) {
        set_error(error, "无法检查规则目录可用空间"); return false;
    }
    const uint64_t unit = info.f_frsize;
    const uint64_t available = info.f_bavail > std::numeric_limits<uint64_t>::max() / unit
        ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(info.f_bavail) * unit;
    if (required > available || reserve > available - required) {
        set_error(error, "规则空间不足，保留当前规则"); return false;
    }
    if (error) error->clear();
    return true;
}

bool stage_rule_bundle(const std::string &data_dir, const std::string &generation,
                       const std::string &hosts_gzip, const std::string &adguard_gzip,
                       StagedRuleBundle &staged, std::string *error) {
    staged = {};
    if (!valid_generation(generation) || hosts_gzip.size() > MAX_RULE_INPUT_BYTES ||
        adguard_gzip.size() > MAX_RULE_INPUT_BYTES) { set_error(error, "generation 名称或规则大小非法"); return false; }
    const auto rulesets = data_dir + "/rulesets";
    if (!ensure_directory(data_dir, 0755) || !ensure_directory(rulesets, 0700)) {
        set_error(error, "无法创建可信 rulesets 目录"); return false;
    }
    // Identical content reuses the pinned current generation, even on a
    // nearly-full disk. No duplicate directory is written.
    auto current = resolve_rule_bundle(data_dir, data_dir + "/rules.current", "", "");
    if (current.from_manifest && same_file(current.hosts_path, hosts_gzip) && same_file(current.adguard_path, adguard_gzip)) {
        staged.generation = current.generation; staged.final_dir = parent_directory(current.hosts_path);
        staged.hosts_path = current.hosts_path; staged.adguard_path = current.adguard_path;
        staged.reused = true; staged.writer_lease = std::move(current.reader_lease);
        if (error) error->clear();
        return true;
    }
    if (!rule_bundle_space_available(data_dir, hosts_gzip.size() + adguard_gzip.size(), error)) return false;
    auto catalog = directory(rulesets, LOCK_EX, 1000);
    if (!catalog) { set_error(error, "规则目录正在使用，暂未写入"); return false; }
    staged.generation = generation; staged.staging_dir = rulesets + "/.staging-" + generation;
    staged.final_dir = rulesets + "/" + generation;
    staged.hosts_path = staged.staging_dir + "/" + hosts_name;
    staged.adguard_path = staged.staging_dir + "/" + adguard_name;
    if (mkdir(staged.staging_dir.c_str(), 0700)) { set_error(error, "无法创建独占规则 staging"); staged = {}; return false; }
    staged.writer_lease = directory(staged.staging_dir, LOCK_SH);
    catalog.reset(); // Long writes never hold the short catalog lock.
    if (!staged.writer_lease || !write_new_file(staged.hosts_path, hosts_gzip) ||
        !write_new_file(staged.adguard_path, adguard_gzip) || !sync_directory(staged.staging_dir, "staging_sync")) {
        discard_staged_rule_bundle(staged);
        set_error(error, "staging 规则写入或持久化失败"); staged = {}; return false;
    }
    if (error) error->clear();
    return true;
}

bool commit_rule_bundle(const std::string &manifest_path, const StagedRuleBundle &staged, std::string *error) {
    const auto data_dir = parent_directory(manifest_path), rulesets = data_dir + "/rulesets";
    if (!valid_generation(staged.generation) || !staged.writer_lease || staged.final_dir != rulesets + "/" + staged.generation ||
        (!staged.reused && staged.staging_dir != rulesets + "/.staging-" + staged.generation)) {
        set_error(error, "staging 状态非法"); return false;
    }
    auto catalog = directory(rulesets, LOCK_EX, 1000);
    if (!catalog) { set_error(error, "规则目录提交锁不可用"); return false; }
    auto own = std::static_pointer_cast<Directory>(staged.writer_lease);
    struct stat held {}, named {}; uint64_t own_bytes = 0;
    const auto named_path = staged.reused ? staged.final_dir : staged.staging_dir;
    if (fstat(own->fd, &held) || lstat(named_path.c_str(), &named) || !S_ISDIR(named.st_mode) ||
        held.st_ino != named.st_ino || held.st_dev != named.st_dev || !inventory(own->fd, false, own_bytes)) {
        set_error(error, "staging 身份或文件集合已改变，拒绝提交"); return false;
    }
    std::string old; bool missing = false;
    const bool has_old = pointer(manifest_path, old, missing);
    if (!has_old && !missing) { set_error(error, "当前规则指针不可读或损坏，拒绝覆盖"); return false; }
    if (staged.reused) {
        if (has_old && old == staged.generation) { if (error) error->clear(); return true; }
        set_error(error, "相同内容的当前代际已改变，请重新同步"); return false;
    }
    struct stat existing {};
    if (!lstat(staged.final_dir.c_str(), &existing) || errno != ENOENT ||
        rename(staged.staging_dir.c_str(), staged.final_dir.c_str()) || !sync_directory(rulesets, "generation_sync")) {
        set_error(error, "无法固化或持久化规则 generation，当前指针未切换"); return false;
    }
    if (has_old) {
        auto previous = directory(rulesets + "/" + old, LOCK_SH);
        uint64_t bytes = 0; bool visible = false;
        if (!previous || !inventory(previous->fd, false, bytes) ||
            !replace_pointer(manifest_path + ".previous", old, visible, "previous_sync")) {
            set_error(error, "无法持久化可回退代际，当前指针未切换"); return false;
        }
    }
    bool visible = false;
    if (!replace_pointer(manifest_path, staged.generation, visible, "current_sync")) {
        set_error(error, visible ? "新规则指针已可见，但目录持久化未确认；请核查后重试" : "无法原子切换规则指针");
        return false;
    }
    if (error) error->clear();
    return true;
}

void discard_staged_rule_bundle(const StagedRuleBundle &staged) {
    if (staged.reused || staged.staging_dir.empty()) return;
    auto catalog = directory(parent_directory(staged.staging_dir), LOCK_EX, 1000);
    auto own = std::static_pointer_cast<Directory>(staged.writer_lease);
    uint64_t bytes = 0;
    if (!catalog || !own || !inventory(own->fd, true, bytes)) return;
    struct stat held {}, named {};
    if (fstat(own->fd, &held) || lstat(staged.staging_dir.c_str(), &named) ||
        !S_ISDIR(named.st_mode) || held.st_ino != named.st_ino || held.st_dev != named.st_dev) return;
    for (const char *name : {hosts_name, adguard_name})
        if (unlinkat(own->fd, name, 0) && errno != ENOENT) return;
    rmdir(staged.staging_dir.c_str());
}

RuleBundleCollection collect_rule_bundles(const std::string &data_dir, const RuleBundleRetention &policy, uint64_t now) {
    RuleBundleCollection result;
    const auto rulesets = data_dir + "/rulesets";
    auto catalog = directory(rulesets, LOCK_EX, 1000);
    if (!catalog) { result.error = "rulesets 目录或清理锁不可用"; return result; }
    if (!now) now = static_cast<uint64_t>(time(nullptr));
    std::string current, previous; bool missing = false;
    if (!pointer(data_dir + "/rules.current", current, missing) && !missing) {
        result.error = "当前规则指针不可读，未清理历史"; return result;
    }
    if (!pointer(data_dir + "/rules.current.previous", previous, missing) && !missing) {
        result.error = "回退规则指针不可读，未清理历史"; return result;
    }
    for (const auto &name : {current, previous}) if (!name.empty()) {
        Directory dir(rulesets + "/" + name); uint64_t bytes = 0;
        if (dir.fd < 0 || !inventory(dir.fd, false, bytes)) { result.error = "受保护代际不可验证，未清理历史"; return result; }
    }
    struct Entry { std::string name; uint64_t bytes, stamp; bool staging, trash; };
    std::vector<Entry> candidates;
    int copy = openat(catalog->fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    DIR *entries = copy >= 0 ? fdopendir(copy) : nullptr;
    if (!entries) { if (copy >= 0) close(copy); result.error = "无法读取规则历史"; return result; }
    bool scanned = true; size_t count = 0; errno = 0;
    while (auto *entry = readdir(entries)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (++count > 4096) { scanned = false; break; }
        const bool staging = name.compare(0, 9, ".staging-") == 0;
        const bool trash = name.compare(0, 4, ".gc-") == 0;
        const auto generation = staging ? name.substr(9) : trash ? name.substr(4) : name;
        if (!valid_generation(generation)) { ++result.skipped; errno = 0; continue; }
        Directory dir(rulesets + "/" + name); uint64_t bytes = 0; struct stat info {};
        if (dir.fd < 0 || !inventory(dir.fd, staging || trash, bytes) || fstat(dir.fd, &info) || info.st_mtime < 0) {
            ++result.skipped; errno = 0; continue;
        }
        candidates.push_back({name, bytes, static_cast<uint64_t>(info.st_mtime), staging, trash});
        errno = 0;
    }
    if (errno) scanned = false;
    closedir(entries);
    if (!scanned) { result.error = "规则历史扫描失败或超过 4096 项，未执行清理"; return result; }
    std::sort(candidates.begin(), candidates.end(), [](const Entry &a, const Entry &b) {
        return a.stamp != b.stamp ? a.stamp > b.stamp : a.name > b.name;
    });
    std::vector<Entry> optional;
    for (const auto &entry : candidates) {
        auto exclusive = directory(rulesets + "/" + entry.name, LOCK_EX);
        if (entry.name == current || entry.name == previous || !exclusive) {
            ++result.retained; result.retained_bytes += entry.bytes;
            if (!exclusive) ++result.pinned;
        } else optional.push_back(entry);
    }
    for (const auto &entry : optional) {
        const uint64_t age = now > entry.stamp ? now - entry.stamp : 0;
        const bool keep = entry.trash ? false : entry.staging ? age < policy.staging_age_seconds :
            (age < policy.max_age_seconds && result.retained < std::max<size_t>(2, policy.generations) &&
             result.retained_bytes <= policy.bytes && entry.bytes <= policy.bytes - result.retained_bytes);
        if (keep) { ++result.retained; result.retained_bytes += entry.bytes; continue; }
        const auto trash = entry.trash ? "" : rulesets + "/.gc-" + (entry.staging ? entry.name.substr(9) : entry.name);
        if (remove_bundle_dir(rulesets + "/" + entry.name, entry.staging || entry.trash, trash)) ++result.removed;
        else { ++result.retained; result.retained_bytes += entry.bytes; ++result.skipped; }
    }
    result.over_budget = result.retained_bytes > policy.bytes || result.retained > std::max<size_t>(2, policy.generations);
    result.ok = sync_directory(rulesets);
    if (!result.ok) result.error = "历史清理目录持久化未确认";
    return result;
}
