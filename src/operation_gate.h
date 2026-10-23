#pragma once

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Shared by accepted writers and their descendants. The uninstall controller
// closes admission first, drains writers, then takes an exclusive lease. The
// lock is outside the data tree, so deleting data cannot replace its inode.
inline int operation_lease_fd = -1;
inline std::string operation_marker_path;
inline bool operation_closing(const std::string &marker) {
    if (marker.empty()) return false;
    struct stat info {};
    if (lstat(marker.c_str(), &info) == 0) return true;
    return errno != ENOENT;
}
inline bool operation_closing() { return operation_closing(operation_marker_path); }

inline std::string operation_executable(const char *argument) {
#ifdef __linux__
    (void)argument;
    char path[4096];
    const auto size = readlink("/proc/self/exe", path, sizeof(path));
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(path)) return "";
    return std::string(path, static_cast<size_t>(size));
#else
    char *path = argument ? realpath(argument, nullptr) : nullptr;
    const std::string result = path ? path : ""; free(path); return result;
#endif
}

class OperationGate {
public:
    OperationGate(const std::string &data, const std::string &marker, const std::string &executable,
                  unsigned timeout_ms = 1000) {
        const auto slash = data.rfind('/');
        if (slash == std::string::npos || slash == 0 || executable.empty() || operation_closing(marker)) {
            error_ = "模块正在卸载，或无法核实运行映像"; return;
        }
        const auto parent = data.substr(0, slash);
        lock_path_ = parent + "/.AD_Killer_By_NFQUEUE.operations.lock";
        const int root = open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
        struct stat trusted {};
        if (root < 0) { error_ = "运行数据父目录不可用"; return; }
        bool safe = !fstat(root, &trusted) && trusted.st_uid == geteuid() && !(trusted.st_mode & 0022);
        close(root);
        const char *running_image = executable.c_str();
#ifdef __linux__
        running_image = "/proc/self/exe";
#endif
        if (!safe || stat(running_image, &image_) || !S_ISREG(image_.st_mode)) {
            error_ = "运行目录或映像身份不可用"; return;
        }
        fd_ = open(lock_path_.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        const bool created = fd_ >= 0;
        if (fd_ < 0 && errno == EEXIST) fd_ = open(lock_path_.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd_ >= 0 && fd_ <= STDERR_FILENO) {
            const int moved = fcntl(fd_, F_DUPFD_CLOEXEC, 3); close(fd_); fd_ = moved;
        }
        struct stat lease {};
        if (fd_ < 0 || fstat(fd_, &lease) || !S_ISREG(lease.st_mode) || lease.st_uid != geteuid() ||
            lease.st_nlink != 1 || (lease.st_mode & 0022)) { fail("模块写入租约无法核实"); return; }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (flock(fd_, LOCK_SH | LOCK_NB)) {
            if ((errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) || std::chrono::steady_clock::now() >= deadline) {
                fail("模块正在维护，写入入口尚未开放"); return;
            }
            usleep(1000);
        }
        struct stat current {};
        if (operation_closing(marker) || stat(executable.c_str(), &current) ||
            current.st_dev != image_.st_dev || current.st_ino != image_.st_ino) {
            // A late caller that creates a fresh lock after successful removal
            // is rejected before any data mkdir/write, and removes only its
            // own just-created inode while admission remains closed.
            if (created && operation_closing(marker)) {
                struct stat named {};
                if (!lstat(lock_path_.c_str(), &named) && named.st_dev == lease.st_dev && named.st_ino == lease.st_ino)
                    unlink(lock_path_.c_str());
            }
            fail("模块正在卸载或运行映像已替换，未派发写入"); return;
        }
        previous_fd_ = operation_lease_fd; previous_marker_ = operation_marker_path;
        operation_lease_fd = fd_; operation_marker_path = marker;
    }
    ~OperationGate() {
        if (fd_ >= 0) {
            if (operation_lease_fd == fd_) { operation_lease_fd = previous_fd_; operation_marker_path = previous_marker_; }
            close(fd_);
        }
    }
    OperationGate(const OperationGate &) = delete;
    OperationGate &operator=(const OperationGate &) = delete;
    bool ok() const { return fd_ >= 0 && error_.empty(); }
    const std::string &error() const { return error_; }
    const std::string &path() const { return lock_path_; }
private:
    void fail(const char *message) { error_ = message; if (fd_ >= 0) close(fd_); fd_ = -1; }
    int fd_ = -1, previous_fd_ = -1;
    std::string lock_path_, error_, previous_marker_;
    struct stat image_ {};
};
