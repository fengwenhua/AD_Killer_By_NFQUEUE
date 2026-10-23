#pragma once

#include <charconv>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

enum class BoundedRead { OK, MISSING, ERROR };
inline BoundedRead read_regular_bounded(const std::string &path, std::string &out, size_t limit) {
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? BoundedRead::MISSING : BoundedRead::ERROR;
    struct stat info {};
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        static_cast<uint64_t>(info.st_size) <= limit;
    char buffer[4096];
    while (ok) {
        const auto count = ::read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (count > 0 && static_cast<size_t>(count) > limit - out.size())) { ok = false; break; }
        if (!count) break;
        out.append(buffer, static_cast<size_t>(count));
    }
    if (close(fd) != 0) ok = false;
    if (!ok) out.clear();
    return ok ? BoundedRead::OK : BoundedRead::ERROR;
}

inline bool parse_u64(const std::string &text, uint64_t &number) {
    if (text.empty() || text.size() > 20 || text.find_first_not_of("0123456789") != std::string::npos) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

struct ProcessIdentity {
    int pid = 0;
    std::string boot;
    uint64_t started = 0;
    bool operator==(const ProcessIdentity &other) const {
        return pid == other.pid && boot == other.boot && started == other.started;
    }
    bool valid() const {
        if (pid <= 0 || !started || boot.size() != 36) return false;
        for (size_t i = 0; i < boot.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) { if (boot[i] != '-') return false; }
            else if (std::string("0123456789abcdef").find(boot[i]) == std::string::npos) return false;
        }
        return true;
    }
};

inline bool parse_process_start(const std::string &text, int pid, uint64_t &started) {
    const auto open = text.find('('), close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close <= open ||
        text.substr(0, open) != std::to_string(pid) + " ") return false;
    std::istringstream fields(text.substr(close + 1));
    std::string word;
    for (int field = 3; field <= 22; ++field) {
        if (!(fields >> word)) return false;
        if (field == 3 && (word.size() != 1 || std::string("RSDTWtKWPIN").find(word[0]) == std::string::npos)) return false;
        if (field == 22 && (!parse_u64(word, started) || !started)) return false;
    }
    return true;
}

// Re-read boot and stat to reject exit/reuse/migration of the observation. The
// caller still verifies exact module argv/executable; this is not that check.
inline ProcessIdentity process_identity(int pid, const std::string &proc = "/proc") {
    if (pid <= 0) return {};
    std::string boot, again, stat;
    const std::string boot_path = proc + "/sys/kernel/random/boot_id";
    if (read_regular_bounded(boot_path, boot, 64) != BoundedRead::OK) return {};
    if (!boot.empty() && boot.back() == '\n') boot.pop_back();
    ProcessIdentity identity{pid, boot, 0};
    const std::string stat_path = proc + "/" + std::to_string(pid) + "/stat";
    if (read_regular_bounded(stat_path, stat, 16384) != BoundedRead::OK ||
        !parse_process_start(stat, pid, identity.started) || !identity.valid()) return {};
    uint64_t started = 0;
    if (read_regular_bounded(stat_path, stat, 16384) != BoundedRead::OK ||
        !parse_process_start(stat, pid, started) || started != identity.started ||
        read_regular_bounded(boot_path, again, 64) != BoundedRead::OK) return {};
    if (!again.empty() && again.back() == '\n') again.pop_back();
    return again == identity.boot ? identity : ProcessIdentity{};
}
