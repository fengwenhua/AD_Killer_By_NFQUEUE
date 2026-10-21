#include "cgroup_isolation.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <unistd.h>
#if defined(__linux__)
#include <sys/vfs.h>
#endif

namespace {
bool token(const std::string &list, const std::string &wanted) {
    std::istringstream in(list);
    std::string item;
    while (std::getline(in, item, ',')) if (item == wanted) return true;
    return false;
}
bool absolute_path(const std::string &path) {
    if (path.empty() || path[0] != '/' || path.size() > 4096) return false;
    if (path == "/") return true;
    std::istringstream in(path.substr(1));
    std::string part;
    while (std::getline(in, part, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char c : part) if (c < 32 || c == 127) return false;
    }
    return path.back() != '/';
}
bool unescape_mount(const std::string &input, std::string &output) {
    output.clear();
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\') { output += input[i]; continue; }
        if (i + 3 >= input.size()) return false;
        const std::string octal = input.substr(i + 1, 3);
        if (octal == "040") output += ' ';
        else if (octal == "011") output += '\t';
        else if (octal == "012") output += '\n';
        else if (octal == "134") output += '\\';
        else return false;
        i += 3;
    }
    return absolute_path(output);
}
bool read_bounded(const std::string &path, std::string &out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    out.clear();
    char buffer[4096];
    bool ok = true;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && out.size() + static_cast<size_t>(n) > 256 * 1024)) {
            ok = false; break;
        }
        if (n == 0) break;
        out.append(buffer, static_cast<size_t>(n));
    }
    close(fd);
    return ok;
}
#if defined(__linux__)
bool move_self(const FreezerHierarchy &group, int pid, std::string &error) {
    if (pid != getpid()) { error = "refusing to migrate another process"; return false; }
    int dir = open(group.mount.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { error = std::strerror(errno); return false; }
    struct statfs fs {};
    const long magic = group.unified ? 0x63677270L : 0x27e0ebL;
    if (fstatfs(dir, &fs) != 0 || static_cast<long>(fs.f_type) != magic) {
        close(dir); error = "target is not the discovered cgroup filesystem"; return false;
    }
    int fd = openat(dir, "cgroup.procs", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    close(dir);
    if (fd < 0) { error = std::strerror(errno); return false; }
    const std::string value = std::to_string(pid) + "\n";
    ssize_t n;
    do { n = write(fd, value.data(), value.size()); } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        error = n < 0 ? std::strerror(saved) : "short cgroup PID write";
        return false;
    }
    return true;
}
#endif
}

bool discover_freezer_hierarchies(const std::string &membership,
                                 const std::string &mountinfo,
                                 std::vector<FreezerHierarchy> &groups,
                                 std::string &error) {
    groups.clear(); error.clear();
    std::istringstream rows(membership);
    std::string row;
    bool saw_membership = false, unified = false, freezer = false;
    while (std::getline(rows, row)) {
        if (row.empty()) continue;
        size_t a = row.find(':'), b = row.find(':', a == std::string::npos ? 0 : a + 1);
        if (a == std::string::npos || b == std::string::npos || a == 0 ||
            row.substr(0, a).find_first_not_of("0123456789") != std::string::npos) {
            error = "invalid /proc cgroup membership"; return false;
        }
        saw_membership = true;
        const std::string controllers = row.substr(a + 1, b - a - 1);
        bool v2 = row.substr(0, a) == "0" && controllers.empty();
        if (!v2 && !token(controllers, "freezer")) continue;
        bool &seen = v2 ? unified : freezer;
        if (seen || !absolute_path(row.substr(b + 1))) {
            error = "ambiguous or unsafe freezer membership"; return false;
        }
        seen = true;
        groups.push_back({v2, row.substr(b + 1), ""});
    }
    if (!saw_membership) { error = "missing cgroup membership"; return false; }
    for (auto &group : groups) {
        std::istringstream mounts(mountinfo);
        while (std::getline(mounts, row)) {
            size_t separator = row.find(" - ");
            if (separator == std::string::npos) continue;
            std::istringstream right(row.substr(separator + 3));
            std::string type, source, options;
            if (!(right >> type >> source >> options)) continue;
            if (group.unified ? type != "cgroup2" :
                (type != "cgroup" || !token(options, "freezer"))) continue;
            std::istringstream left(row.substr(0, separator));
            std::string id, parent, device, root, mount;
            if (!(left >> id >> parent >> device >> root >> mount)) continue;
            // A subtree/bind mount cannot establish escape from frozen ancestors.
            if (root != "/") continue;
            std::string decoded;
            if (!unescape_mount(mount, decoded)) continue;
            if (group.mount.empty()) group.mount = decoded;
        }
        if (group.mount.empty()) {
            error = "no full-root mount for freezer hierarchy"; return false;
        }
    }
    return true;
}

CgroupIsolationResult isolate_freezer_hierarchies(
    int pid, const CgroupRead &read, const CgroupMove &move) {
    CgroupIsolationResult result;
    if (pid <= 0) { result.error = "invalid self PID"; return result; }
    const std::string proc = "/proc/" + std::to_string(pid);
    std::string membership, mounts;
    if (!read(proc + "/cgroup", membership) || !read(proc + "/mountinfo", mounts)) {
        result.error = "cannot read cgroup membership/mountinfo"; return result;
    }
    std::vector<FreezerHierarchy> groups;
    if (!discover_freezer_hierarchies(membership, mounts, groups, result.error)) return result;
    for (const auto &group : groups) {
        if (group.membership == "/") continue;
        if (!move(group, pid, result.error)) {
            result.error = group.mount + ": " + result.error; return result;
        }
        ++result.moved;
    }
    const size_t expected_hierarchies = groups.size();
    // The write succeeding is not sufficient: re-read kernel membership before
    // opening any NFQUEUE or installing firewall rules. Never unfreeze a group.
    if (!read(proc + "/cgroup", membership) ||
        !discover_freezer_hierarchies(membership, mounts, groups, result.error)) {
        if (result.error.empty()) result.error = "cannot verify cgroup migration";
        return result;
    }
    if (groups.size() != expected_hierarchies) {
        result.error = "freezer hierarchy changed during migration"; return result;
    }
    for (const auto &group : groups) if (group.membership != "/") {
        result.error = "still belongs to a freezable child cgroup"; return result;
    }
    result.ok = true;
    return result;
}

bool frozen_cgroup_process(int pid, const CgroupRead &read) {
    if (pid <= 0) return false;
    const std::string proc = "/proc/" + std::to_string(pid);
    std::string membership, mounts, error;
    std::vector<FreezerHierarchy> groups;
    if (!read(proc + "/cgroup", membership) || !read(proc + "/mountinfo", mounts) ||
        !discover_freezer_hierarchies(membership, mounts, groups, error)) return false;
    for (const auto &group : groups) {
        if (group.membership == "/") continue;
        std::string content;
        const std::string path = group.mount + group.membership;
        if (group.unified) {
            if (!read(path + "/cgroup.events", content)) continue;
            std::istringstream values(content);
            std::string key, value;
            while (values >> key >> value) if (key == "frozen" && value == "1") return true;
        } else {
            if (read(path + "/freezer.state", content) &&
                (content == "FROZEN\n" || content == "FREEZING\n")) return true;
        }
    }
    return false;
}

CgroupIsolationResult isolate_daemon_cgroups() {
#if defined(__linux__)
    return isolate_freezer_hierarchies(static_cast<int>(getpid()), read_bounded, move_self);
#else
    return {true, 0, ""};
#endif
}
bool daemon_cgroup_frozen(int pid) {
    return frozen_cgroup_process(pid, read_bounded);
}
