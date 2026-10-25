#pragma once
#include <cstdlib>
#include <string>
#if defined(__linux__)
#include <fstream>
#include <sstream>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

// Reject file/directory bind mounts as well as different-device mounts before
// managed storage GC. Unknown inventory is not permission to recurse/delete.
inline bool no_storage_mounts(const std::string &path) {
    char *resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return false;
    const std::string root(resolved); free(resolved);
    const auto inside = [&](const std::string &point) { return point == root || point.compare(0, root.size() + 1, root + "/") == 0; };
#if defined(__linux__)
    std::ifstream input("/proc/self/mountinfo");
    if (!input) return false;
    std::string line;
    size_t total = 0;
    while (std::getline(input, line)) {
        if (line.size() > 65536 || (total += line.size()) > 4u * 1024u * 1024u) return false;
        std::istringstream row(line);
        std::string id, parent, device, source, encoded, point;
        if (!(row >> id >> parent >> device >> source >> encoded)) return false;
        for (size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '\\') point += encoded[i];
            else {
                if (i + 3 >= encoded.size() || encoded.substr(i + 1, 3).find_first_not_of("01234567") != std::string::npos) return false;
                const int ch = (encoded[i + 1] - '0') * 64 + (encoded[i + 2] - '0') * 8 + encoded[i + 3] - '0';
                if (!ch || ch > 255) return false;
                point += static_cast<char>(ch); i += 3;
            }
        }
        if (inside(point)) return false;
    }
    return input.eof();
#elif defined(__APPLE__)
    struct statfs *mounts = nullptr;
    const auto count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) return false;
    for (int i = 0; i < count; ++i) if (inside(mounts[i].f_mntonname)) return false;
    return true;
#else
    (void)inside;
    return false;
#endif
}
