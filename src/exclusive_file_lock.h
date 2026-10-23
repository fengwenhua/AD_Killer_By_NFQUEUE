#pragma once

#include <fcntl.h>
#include <cerrno>
#include <chrono>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// 非阻塞进程级独占锁。对象存活期间保持文件描述符打开并持锁。
class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const char *path, bool wait = false, unsigned timeout_ms = 0) {
        fd_ = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
        struct stat info {};
        if (fd_ >= 0 && (fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode))) {
            close(fd_); fd_ = -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int operation = LOCK_EX | (wait && !timeout_ms ? 0 : LOCK_NB);
        int result = -1;
        while (fd_ >= 0) {
            result = flock(fd_, operation);
            if (result == 0) break;
            if (errno == EINTR) {
                if (timeout_ms && std::chrono::steady_clock::now() >= deadline) break;
                continue;
            }
            if (!wait || !timeout_ms || (errno != EWOULDBLOCK && errno != EAGAIN) ||
                std::chrono::steady_clock::now() >= deadline) break;
            usleep(10 * 1000);
        }
        if (fd_ >= 0 && result != 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    ~ExclusiveFileLock() {
        if (fd_ >= 0) {
            // flock belongs to the open file description. Closing the last
            // inherited lease releases it; LOCK_UN here would prematurely
            // unlock an in-flight firewall command after its parent returns.
            close(fd_);
        }
    }

    ExclusiveFileLock(const ExclusiveFileLock &) = delete;
    ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;

    bool ok() const { return fd_ >= 0; }
    int native_fd() const { return fd_; }

private:
    int fd_ = -1;
};
