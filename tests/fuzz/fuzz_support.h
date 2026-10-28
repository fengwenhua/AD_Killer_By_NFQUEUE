#pragma once
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>

// Files are private fixtures only. No archive entry is ever extracted.
struct FuzzFile {
    char path[64] = "/tmp/adk-fuzz-input.XXXXXX";
    explicit FuzzFile(const uint8_t *data, size_t size) {
        int fd = mkstemp(path);
        if (fd < 0) std::abort();
        size_t done = 0;
        while (done < size) {
            const auto count = write(fd, data + done, size - done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) std::abort();
            done += static_cast<size_t>(count);
        }
        if (close(fd)) std::abort();
    }
    ~FuzzFile() { unlink(path); }
    FuzzFile(const FuzzFile &) = delete;
    FuzzFile &operator=(const FuzzFile &) = delete;
};

inline void fuzz_require(bool condition) {
    if (!condition) std::abort();
}
