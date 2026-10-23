#pragma once
#include "common.h"
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <cstdint>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>

struct QueueOwner {
    uint16_t queue;
    uint32_t port_id;  // Netlink port IDs are NOT necessarily process IDs.
};

inline std::vector<QueueOwner> parse_queue_owners(const std::string &text) {
    std::vector<QueueOwner> result;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        uint64_t queue, port;
        if (row >> queue >> port && queue <= UINT16_MAX && port > 0 && port <= UINT32_MAX)
            result.push_back({static_cast<uint16_t>(queue), static_cast<uint32_t>(port)});
    }
    return result;
}

// Resolve only after verifying the socket inode; never call a port ID a PID
// just because a process with the same number happens to exist.
inline std::string queue_owner_process(uint32_t port, const std::string &proc) {
    if (port > INT_MAX) return "";
    std::ifstream netlink(proc + "/net/netlink");
    std::string line, inode;
    while (std::getline(netlink, line)) {
        std::istringstream row(line);
        std::string sk, groups, rmem, wmem, dump, locks, drops, current_inode;
        uint64_t protocol, peer;
        if (row >> sk >> protocol >> peer >> groups >> rmem >> wmem >> dump >> locks >> drops >> current_inode)
            if (protocol == 12 && peer == port) { inode = current_inode; break; }
    }
    if (inode.empty()) return "";
    std::string process = proc + "/" + std::to_string(port);
    DIR *directory = opendir((process + "/fd").c_str());
    if (!directory) return "";
    bool owns = false;
    while (dirent *entry = readdir(directory)) {
        char target[256];
        ssize_t n = readlink((process + "/fd/" + entry->d_name).c_str(), target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = '\0';
        if (std::string(target) == "socket:[" + inode + "]") { owns = true; break; }
    }
    closedir(directory);
    if (!owns) return "";
    std::ifstream comm(process + "/comm");
    std::string name;
    std::getline(comm, name);
    if (name.size() > 48) name.resize(48);
    for (char &c : name) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return name;
}

inline std::string queue_conflict_message(const std::vector<QueueOwner> &owners,
                                          const std::string &proc = "/proc", int only_queue = -1) {
    for (const auto &owner : owners) {
        if (owner.queue != 0 && owner.queue != 1) continue;
        if (only_queue >= 0 && owner.queue != only_queue) continue;
        std::string process = queue_owner_process(owner.port_id, proc);
        return "NFQUEUE 队列 " + std::to_string(owner.queue) + " 已被其他程序占用（" +
               (process.empty() ? "" : process + "，") + "netlink port ID " +
               std::to_string(owner.port_id) + "）。请先停用旧版或其他 NFQUEUE 拦截模块，再启动 AD_Killer_By_NFQUEUE；不会抢占或停止其他程序。";
    }
    return "";
}

inline std::string find_queue_conflict(const std::string &proc = "/proc", int only_queue = -1) {
    std::ifstream input(proc + "/net/netfilter/nfnetlink_queue");
    std::ostringstream text;
    text << input.rdbuf();
    return queue_conflict_message(parse_queue_owners(text.str()), proc, only_queue);
}

enum class QueueReleaseState { RELEASED, FOREIGN, OCCUPIED, UNKNOWN };
struct QueueReleaseResult {
    QueueReleaseState state = QueueReleaseState::UNKNOWN;
    std::string message;
    bool ok() const { return state == QueueReleaseState::RELEASED || state == QueueReleaseState::FOREIGN; }
    const char *name() const {
        return state == QueueReleaseState::RELEASED ? "released" : state == QueueReleaseState::FOREIGN ? "foreign" :
            state == QueueReleaseState::OCCUPIED ? "occupied" : "unknown";
    }
};
// Read-only post-stop verification. A netlink port ID alone is never a PID.
// Foreign listeners are preserved; unresolvable ownership stays unknown.
inline QueueReleaseResult inspect_queue_release(const std::function<bool(int)> &is_ours,
        const std::string &path = PROC_NFQUEUE_FILE, const std::string &proc = "/proc") {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return {QueueReleaseState::UNKNOWN, "无法读取 NFQUEUE 队列，不能确认释放"};
    struct stat info {};
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    std::string text;
    char buffer[4096];
    while (ok) {
        const auto n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && text.size() + static_cast<size_t>(n) > 1024 * 1024)) { ok = false; break; }
        if (!n) break;
        text.append(buffer, static_cast<size_t>(n));
    }
    if (close(fd) != 0) ok = false;
    if (!ok) return {QueueReleaseState::UNKNOWN, "NFQUEUE 队列读取不完整或超出限制"};
    bool foreign = false, occupied = false, unknown = false;
    std::istringstream rows(text);
    std::string line;
    while (std::getline(rows, line)) {
        if (line.size() > 4096) return {QueueReleaseState::UNKNOWN, "NFQUEUE 状态行超出限制"};
        std::istringstream row(line);
        std::string token;
        std::vector<uint64_t> numbers;
        while (row >> token) {
            if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos || token.size() > 20)
                return {QueueReleaseState::UNKNOWN, "NFQUEUE 状态格式无效"};
            errno = 0; char *end = nullptr;
            const auto number = strtoull(token.c_str(), &end, 10);
            if (errno || !end || *end || numbers.size() >= 16) return {QueueReleaseState::UNKNOWN, "NFQUEUE 字段无效"};
            numbers.push_back(number);
        }
        if (numbers.empty()) continue;
        if (numbers.size() < 2 || numbers[0] > UINT16_MAX || !numbers[1] || numbers[1] > UINT32_MAX)
            return {QueueReleaseState::UNKNOWN, "NFQUEUE 归属字段无效"};
        if (numbers[0] != QUEUE_DNS && numbers[0] != QUEUE_TLS_HTTP) continue;
        const uint32_t port = static_cast<uint32_t>(numbers[1]);
        if (queue_owner_process(port, proc).empty()) unknown = true;
        else if (is_ours(static_cast<int>(port))) occupied = true;
        else foreign = true;
    }
    if (occupied) return {QueueReleaseState::OCCUPIED, "仍有本模块监听器持有 NFQUEUE 队列"};
    if (unknown) return {QueueReleaseState::UNKNOWN, "队列 0/1 仍被占用但归属未能核实；未抢占或停止其他程序"};
    if (foreign) return {QueueReleaseState::FOREIGN, "队列已由其他已核实监听器持有；未处理它们"};
    return {QueueReleaseState::RELEASED, ""};
}
