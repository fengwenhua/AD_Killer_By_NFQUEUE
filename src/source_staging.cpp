#include "source_staging.h"
#include "task_context.h"
#include "work_budget.h"
#include "storage_mounts.h"
#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {
bool safe_dir(int fd) {
    struct stat info {};
    return fd >= 0 && !fstat(fd, &info) && S_ISDIR(info.st_mode) && info.st_uid == geteuid() && !(info.st_mode & 0077);
}
int directory(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd >= 0 && fd < 3) { const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3); close(fd); fd = moved; }
    if (!safe_dir(fd)) { if (fd >= 0) close(fd); return -1; }
    return fd;
}
bool valid_name(const std::string &name) {
    return name.size() <= 96 && name.size() > 4 && name.compare(0, 4, "job-") == 0 && name.find_first_not_of("0123456789-", 4) == std::string::npos;
}
bool part_name(const std::string &name) {
    for (int index = 0; index < 2; ++index) {
        const auto part = "source-download-" + std::to_string(index) + ".part";
        if (name == part) return true;
        const auto prefix = part + ".tmp.";
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
            name.find_first_not_of("0123456789", prefix.size()) == std::string::npos) return true;
    }
    return false;
}
// Caller owns the exclusive directory lease, never a guessed writer PID.
bool remove_parts(int fd) {
    const int copy = openat(fd, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (copy < 0) return false;
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); return false; }
    bool ok = true;
    std::vector<std::string> files;
    while (true) {
        errno = 0;
        auto *item = readdir(entries);
        if (!item) { if (errno) ok = false; break; }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        struct stat info {};
        if (files.size() >= 4 || !part_name(name) || fstatat(fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 0077)) { ok = false; break; }
        files.push_back(name);
    }
    closedir(entries);
    if (!ok) return false; // whole inventory before the first unlink
    for (const auto &file : files) if (unlinkat(fd, file.c_str(), 0)) return false;
    return true;
}
bool collect(int root, const std::string &path, size_t &pinned) {
    const int copy = openat(root, ".", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (copy < 0) return false;
    DIR *entries = fdopendir(copy);
    if (!entries) { close(copy); return false; }
    size_t count = 0;
    bool ok = true;
    while (true) {
        errno = 0;
        auto *item = readdir(entries);
        if (!item) { if (errno) ok = false; break; }
        const std::string name = item->d_name;
        if (name == "." || name == "..") continue;
        if (++count > 128 || !valid_name(name)) { ok = false; break; }
        const int fd = directory(path + "/" + name);
        if (fd < 0) { ok = false; break; }
        if (flock(fd, LOCK_EX | LOCK_NB)) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) ++pinned;
            else ok = false;
        } else if (!remove_parts(fd) || unlinkat(root, name.c_str(), AT_REMOVEDIR)) ok = false;
        close(fd);
        if (!ok) break;
    }
    closedir(entries);
    return ok;
}
} // namespace

SourceStaging::SourceStaging(const std::string &data) : root_(data + "/source_staging") {
    if (WorkBudget::expired() || operation_closing() || (mkdir(root_.c_str(), 0700) && errno != EEXIST)) return;
    const int root = directory(root_);
    if (root < 0) return;
    size_t pinned = 0;
    struct statvfs space {};
    bool ok = !flock(root, LOCK_EX | LOCK_NB) && no_storage_mounts(root_) && collect(root, root_, pinned) && pinned < 2 &&
        !fstatvfs(root, &space) && space.f_frsize &&
        static_cast<uint64_t>(space.f_bavail) >= (16u * 1024u * 1024u + space.f_frsize - 1) / space.f_frsize;
    static uint64_t sequence = 0;
    if (ok) {
        name_ = "job-" + std::to_string(static_cast<long>(getpid())) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++sequence);
        path_ = root_ + "/" + name_;
        ok = !mkdirat(root, name_.c_str(), 0700);
    }
    if (ok) {
        lease_ = directory(path_);
        if (lease_ >= 0 && flock(lease_, LOCK_SH | LOCK_NB)) { close(lease_); lease_ = -1; }
    }
    if (lease_ >= 0) { previous_ = task_download_lease_fd; task_download_lease_fd = lease_; }
    close(root);
}
SourceStaging::~SourceStaging() {
    if (lease_ < 0) return;
    task_download_lease_fd = previous_;
    // Do not upgrade/unlock an inherited OFD; a live child still owns its SH
    // lease. Close ours, then prove quiescence using a distinct EX descriptor.
    close(lease_);
    const int root = directory(root_);
    if (root < 0) return;
    if (!flock(root, LOCK_EX | LOCK_NB) && no_storage_mounts(root_)) {
        const int owned = directory(path_);
        if (owned >= 0) {
            if (!flock(owned, LOCK_EX | LOCK_NB) && remove_parts(owned)) unlinkat(root, name_.c_str(), AT_REMOVEDIR);
            close(owned);
        }
        rmdir(root_.c_str()); // succeeds only when no other stage remains
    }
    close(root);
}
