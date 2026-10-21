#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// 非阻塞进程级独占锁。对象存活期间保持文件描述符打开并持锁。
class ExclusiveFileLock {
public:
    explicit ExclusiveFileLock(const char *path, bool wait = false) {
        fd_ = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0644);
        int operation = LOCK_EX | (wait ? 0 : LOCK_NB);
        if (fd_ >= 0 && flock(fd_, operation) != 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    ~ExclusiveFileLock() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
        }
    }

    ExclusiveFileLock(const ExclusiveFileLock &) = delete;
    ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;

    bool ok() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};
