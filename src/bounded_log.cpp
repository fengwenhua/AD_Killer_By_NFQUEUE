#include "bounded_log.h"

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

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

void rotate(const std::string &path, int backups) {
    if (backups <= 0) {
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
                      0600);
        if (fd >= 0) close(fd);
        return;
    }
    unlink((path + "." + std::to_string(backups)).c_str());
    for (int index = backups - 1; index >= 1; index--) {
        std::string from = path + "." + std::to_string(index);
        std::string to = path + "." + std::to_string(index + 1);
        rename(from.c_str(), to.c_str());
    }
    rename(path.c_str(), (path + ".1").c_str());
}

}  // namespace

bool append_bounded_log(const std::string &path, const std::string &input,
                        size_t max_bytes, int backups) {
    if (path.empty() || max_bytes == 0 || backups < 0) return false;
    const char *data = input.data();
    size_t size = input.size();
    if (size > max_bytes) {
        data += size - max_bytes;
        size = max_bytes;
    }

    struct stat info;
    off_t current = 0;
    if (lstat(path.c_str(), &info) == 0) {
        if (!S_ISREG(info.st_mode)) return false;
        current = info.st_size;
    }
    if (current < 0 || static_cast<uint64_t>(current) + size > max_bytes)
        rotate(path, backups);

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return false;
    bool ok = write_all(fd, data, size);
    if (close(fd) != 0) ok = false;
    return ok;
}
