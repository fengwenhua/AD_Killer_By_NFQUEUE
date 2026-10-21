#pragma once
#include <cstdint>
#include <climits>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

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
