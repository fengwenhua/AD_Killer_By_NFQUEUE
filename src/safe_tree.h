#pragma once

#include <cerrno>
#include <chrono>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// An already-quiescent, owned tree only. Preflight holds directory/catalog and
// lock-file leases before the first unlink. All traversal is fd-relative;
// symlinks are unlinked as entries, never followed. No shell/recursive rm.
class SafeTreeRemoval {
    struct Entry {
        std::string name;
        struct stat identity {};
        int fd = -1;
        std::vector<std::unique_ptr<Entry>> children;
        ~Entry() { if (fd >= 0) close(fd); }
    };
public:
    bool prepare(int parent, const std::string &name, std::string &error) {
        if (root_ || name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) return fail(error, "invalid tree name");
        parent_ = parent;
        deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        root_ = std::make_unique<Entry>(); root_->name = name;
        struct stat parent_info {};
        if (fstat(parent, &parent_info) || fstatat(parent, name.c_str(), &root_->identity, AT_SYMLINK_NOFOLLOW) ||
            !S_ISDIR(root_->identity.st_mode) || root_->identity.st_dev != parent_info.st_dev)
            return fail(error, "data root unavailable, symlink or mount");
        device_ = root_->identity.st_dev;
        return inspect(parent, *root_, 0, error);
    }
    bool remove(std::string &error) {
        if (!root_ || !prepared_) return fail(error, "tree preflight not complete");
        return erase(parent_, *root_, error) && (fsync(parent_) == 0 || fail(error, "parent directory sync failed after deletion"));
    }
private:
    static bool fail(std::string &error, const char *message) { error = message; return false; }
    static bool same(const struct stat &a, const struct stat &b) {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && (a.st_mode & S_IFMT) == (b.st_mode & S_IFMT);
    }
    bool budget(std::string &error) {
        return std::chrono::steady_clock::now() < deadline_ || fail(error, "data traversal deadline exceeded");
    }
    bool inspect(int parent, Entry &entry, unsigned depth, std::string &error) {
        if (++count_ > 32768 || depth > 32 || !budget(error)) return fail(error, "data traversal resource limit exceeded");
        const auto &info = entry.identity;
        if (info.st_uid != geteuid() || info.st_dev != device_) return fail(error, "foreign owner or mounted data entry");
        if (!S_ISLNK(info.st_mode) && (info.st_mode & 0022)) return fail(error, "data entry permits uncoordinated foreign writes");
        if (S_ISDIR(info.st_mode)) {
            if (++directories_ > 512) return fail(error, "too many data directories");
            entry.fd = openat(parent, entry.name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
            struct stat opened {};
            if (entry.fd < 0 || fstat(entry.fd, &opened) || !same(info, opened) || flock(entry.fd, LOCK_EX | LOCK_NB))
                return fail(error, "data directory changed or reader/writer lease still held");
            const int scan = openat(entry.fd, ".", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
            DIR *directory = scan >= 0 ? fdopendir(scan) : nullptr;
            if (!directory) { if (scan >= 0) close(scan); return fail(error, "cannot enumerate data directory"); }
            bool ok = true;
            while (ok) {
                errno = 0; const auto *next = readdir(directory);
                if (!next) { if (errno) ok = fail(error, "incomplete data enumeration"); break; }
                const std::string name = next->d_name;
                if (name == "." || name == "..") continue;
                auto child = std::make_unique<Entry>(); child->name = name;
                if (fstatat(entry.fd, name.c_str(), &child->identity, AT_SYMLINK_NOFOLLOW) || !inspect(entry.fd, *child, depth + 1, error)) ok = false;
                if (ok) entry.children.push_back(std::move(child));
            }
            closedir(directory);
            if (depth == 0 && ok) prepared_ = true;
            return ok;
        }
        if (!S_ISREG(info.st_mode) && !S_ISLNK(info.st_mode)) return fail(error, "unexpected special data entry");
        const bool lock = entry.name.size() >= 5 &&
            (entry.name.compare(entry.name.size() - 5, 5, ".lock") == 0 ||
             (entry.name.size() >= 6 && entry.name.compare(entry.name.size() - 6, 6, ".lease") == 0));
        if (lock) {
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || ++locks_ > 512) return fail(error, "untrusted data lock");
            entry.fd = openat(parent, entry.name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            struct stat opened {};
            if (entry.fd < 0 || fstat(entry.fd, &opened) || !same(info, opened) || flock(entry.fd, LOCK_EX | LOCK_NB))
                return fail(error, "data lock changed or still held");
        }
        return true;
    }
    bool erase(int parent, Entry &entry, std::string &error) {
        struct stat current {};
        if (!budget(error) || fstatat(parent, entry.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) || !same(current, entry.identity))
            return fail(error, "data entry changed during deletion; remaining entries retained");
        if (S_ISDIR(current.st_mode)) {
            for (auto &child : entry.children) if (!erase(entry.fd, *child, error)) return false;
            if (fsync(entry.fd)) return fail(error, "directory sync failed during deletion");
        }
        if (unlinkat(parent, entry.name.c_str(), S_ISDIR(current.st_mode) ? AT_REMOVEDIR : 0))
            return fail(error, "data deletion incomplete; remaining entries retained");
        return true;
    }
    int parent_ = -1;
    dev_t device_ {};
    size_t count_ = 0, directories_ = 0, locks_ = 0;
    bool prepared_ = false;
    std::chrono::steady_clock::time_point deadline_;
    std::unique_ptr<Entry> root_;
};
