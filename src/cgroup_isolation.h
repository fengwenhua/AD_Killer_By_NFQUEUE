#pragma once
#include <functional>
#include <string>
#include <vector>

// Only unified v2 and the legacy freezer hierarchy are changed. CPU, cpuset,
// memory and I/O policies remain owned by Android/the root manager.
struct FreezerHierarchy {
    bool unified = false;
    std::string membership;
    std::string mount;
};
struct CgroupIsolationResult {
    bool ok = false;
    unsigned moved = 0;
    std::string error;
};
using CgroupRead = std::function<bool(const std::string &, std::string &)>;
using CgroupMove = std::function<bool(const FreezerHierarchy &, int, std::string &)>;

// Pure discovery and injectable I/O support fixtures without touching host groups.
bool discover_freezer_hierarchies(const std::string &membership,
                                 const std::string &mountinfo,
                                 std::vector<FreezerHierarchy> &groups,
                                 std::string &error);
CgroupIsolationResult isolate_freezer_hierarchies(
    int pid, const CgroupRead &read, const CgroupMove &move);
bool frozen_cgroup_process(int pid, const CgroupRead &read);

// Production entry points never accept a target PID for migration: only self.
CgroupIsolationResult isolate_daemon_cgroups();
bool daemon_cgroup_frozen(int pid);
