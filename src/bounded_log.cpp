#include "bounded_log.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>

namespace {

bool write_all(int fd, const char *data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool rotate(const std::string &path, int backups) {
    if (backups <= 0) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
        return fd >= 0 && close(fd) == 0;
    }
    if (unlink((path + "." + std::to_string(backups)).c_str()) && errno != ENOENT) return false;
    for (int index = backups - 1; index >= 1; index--) {
        std::string from = path + "." + std::to_string(index);
        std::string to = path + "." + std::to_string(index + 1);
        if (rename(from.c_str(), to.c_str()) && errno != ENOENT) return false;
    }
    return rename(path.c_str(), (path + ".1").c_str()) == 0 || errno == ENOENT;
}

}  // namespace

bool append_bounded_log(const std::string &path, const std::string &input,
                        size_t max_bytes, int backups, bool generation_header) {
    if (path.empty() || max_bytes == 0 || backups < 0) return false;
    std::string header;
    if (generation_header) {
        static std::atomic<uint64_t> sequence{0};
        header = "# ADK_JOB_LOG=" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(static_cast<long>(getpid())) + "-" + std::to_string(sequence.fetch_add(1)) + "\n";
        if (header.size() >= max_bytes) return false;
    }
    const char *data = input.data();
    size_t size = input.size();
    const size_t capacity = max_bytes - header.size();
    if (size > capacity) {
        data += size - capacity;
        size = capacity;
    }

    struct stat info;
    off_t current = 0;
    if (lstat(path.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) return false;
        current = info.st_size;
    }
    if (current < 0 || static_cast<uint64_t>(current) + size > max_bytes) {
        if (!rotate(path, backups)) return false;
        current = 0;
    }

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return false;
    bool ok = (current != 0 || header.empty() || write_all(fd, header.data(), header.size())) && write_all(fd, data, size);
    if (close(fd) != 0) ok = false;
    return ok;
}
