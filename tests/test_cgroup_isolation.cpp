#include "cgroup_isolation.h"
#include <cassert>
#include <iostream>
#include <map>

static const std::string mounts =
    "131 93 0:77 / /dev/blkio rw - cgroup none rw,blkio\n"
    "133 96 0:78 / /sys/fs/cgroup rw shared:37 - cgroup2 none rw,memory_recursiveprot\n"
    "135 93 0:79 / /dev/cpuctl rw - cgroup none rw,cpu\n"
    "389 93 0:135 / /dev/freezer rw - cgroup none rw,freezer\n";
static const std::string membership =
    "5:freezer:/\n4:memory:/apps/me.weishu.kernelsu\n"
    "3:cpuset:/top-app\n2:cpu:/background\n1:blkio:/background\n"
    "0::/apps/uid_10391/pid_5034\n";

int main() {
    std::vector<FreezerHierarchy> groups;
    std::string error;
    assert(discover_freezer_hierarchies(membership, mounts, groups, error));
    assert(groups.size() == 2 && !groups[0].unified && groups[0].membership == "/");
    assert(groups[1].unified && groups[1].mount == "/sys/fs/cgroup");
    assert(groups[1].membership == "/apps/uid_10391/pid_5034");
    assert(discover_freezer_hierarchies("0::/x\n", "1 2 0:3 / /a\\040b rw - cgroup2 none rw\n", groups, error));
    assert(groups[0].mount == "/a b");
    for (const auto &bad : {"", "bad\n", "0::relative\n", "0::/../root\n", "0::/a//b\n",
                           "0::/a/\n", "0::/a\n0::/b\n", "2:freezer:/x\n3:freezer:/y\n"})
        assert(!discover_freezer_hierarchies(bad, mounts, groups, error));
    assert(!discover_freezer_hierarchies("0::/apps/x\n",
        "1 2 0:3 /apps /bound rw - cgroup2 none rw\n", groups, error));
    assert(!discover_freezer_hierarchies("2:freezer:/x\n",
        "1 2 0:3 / /wrong rw - cgroup none rw,freezer_fake\n", groups, error));
    assert(!discover_freezer_hierarchies("0::/x\n",
        "1 2 0:3 / /bad\\057mount rw - cgroup2 none rw\n", groups, error));

    std::map<std::string, std::string> files = {
        {"/proc/7258/cgroup", membership}, {"/proc/7258/mountinfo", mounts},
        {"/sys/fs/cgroup/apps/uid_10391/pid_5034/cgroup.events", "populated 1\nfrozen 1\n"}
    };
    auto read = [&](const std::string &path, std::string &value) {
        auto found = files.find(path);
        if (found == files.end()) return false;
        value = found->second; return true;
    };
    const auto status = [&] { return inspect_cgroup_process(7258, read).state; };
    assert(status() == FreezerState::FROZEN);
    assert(inspect_cgroup_process(-1, read).state == FreezerState::UNKNOWN);
    files["/sys/fs/cgroup/apps/uid_10391/pid_5034/cgroup.events"] = "populated 1\nfrozen 0\n";
    assert(status() == FreezerState::NORMAL);
    const auto events = "/sys/fs/cgroup/apps/uid_10391/pid_5034/cgroup.events";
    for (const auto &invalid : {"", "populated 1\n", "frozen nope\n", "frozen 0 1\n", "frozen 0\nfrozen 1\n"}) {
        files[events] = invalid;
        const auto failed = inspect_cgroup_process(7258, read);
        assert(failed.state == FreezerState::UNKNOWN && !failed.error.empty());
    }
    files.erase(events);
    assert(status() == FreezerState::UNKNOWN); // missing events is not thawed
    files[events] = "populated 1\nfrozen 0\n";
    for (const auto &denied : {"/proc/7258/cgroup", "/proc/7258/mountinfo", events}) {
        const auto failed = inspect_cgroup_process(7258,
            [&](const std::string &path, std::string &out) { return path != denied && read(path, out); });
        assert(failed.state == FreezerState::UNKNOWN && !failed.error.empty());
    }
    for (const auto &changed : {"/proc/7258/cgroup", "/proc/7258/mountinfo"}) {
        unsigned calls = 0;
        auto result = inspect_cgroup_process(7258, [&](const std::string &path, std::string &out) {
            if (!read(path, out)) return false;
            if (path == changed && ++calls == 2) out += "\n";
            return true;
        });
        assert(result.state == FreezerState::UNKNOWN && result.error.find("changed") != std::string::npos);
    }
    unsigned reads = 0;
    const auto exited = inspect_cgroup_process(7258, [&](const std::string &path, std::string &out) {
        if (path == "/proc/7258/cgroup" && ++reads == 2) return false;
        return read(path, out);
    });
    assert(exited.state == FreezerState::UNKNOWN && exited.error.find("exited") != std::string::npos);
    auto original = files["/proc/7258/mountinfo"];
    files["/proc/7258/mountinfo"] = "1 2 0:3 /apps /bound rw - cgroup2 none rw\n";
    assert(status() == FreezerState::UNKNOWN); // only subtree mounts: not verified
    files["/proc/7258/mountinfo"] = original;
    unsigned moves = 0;
    auto migrate = [&](const FreezerHierarchy &group, int pid, std::string &) {
        assert(pid == 7258 && group.unified && group.mount == "/sys/fs/cgroup");
        assert(group.membership == "/apps/uid_10391/pid_5034");
        ++moves;
        // Only the migrating process changes groups; manager and CPU groups stay.
        files["/proc/7258/cgroup"] = "5:freezer:/\n4:memory:/apps/me.weishu.kernelsu\n"
            "3:cpuset:/top-app\n2:cpu:/background\n1:blkio:/background\n0::/\n";
        return true;
    };
    auto result = isolate_freezer_hierarchies(7258, read, migrate);
    assert(result.ok && result.moved == 1 && moves == 1);
    assert(status() == FreezerState::NORMAL);
    result = isolate_freezer_hierarchies(7258, read, migrate);
    assert(result.ok && result.moved == 0 && moves == 1); // idempotent
    files["/proc/7258/cgroup"] = membership;
    result = isolate_freezer_hierarchies(7258, read,
        [](const FreezerHierarchy &, int, std::string &why) { why = "Permission denied"; return false; });
    assert(!result.ok && result.error.find("Permission denied") != std::string::npos);
    result = isolate_freezer_hierarchies(7258, read,
        [](const FreezerHierarchy &, int, std::string &) { return true; });
    assert(!result.ok && result.error.find("still belongs") != std::string::npos);
    result = isolate_freezer_hierarchies(7258,
        [](const std::string &, std::string &) { return false; }, migrate);
    assert(!result.ok && moves == 1);
    assert(!isolate_freezer_hierarchies(0, read, migrate).ok);
    result = isolate_freezer_hierarchies(7258, read,
        [&](const FreezerHierarchy &, int, std::string &) {
            files["/proc/7258/cgroup"] = "2:cpu:/background\n";
            return true;
        });
    assert(!result.ok && result.error.find("hierarchy changed") != std::string::npos);

    files["/proc/7258/cgroup"] = "2:cpu:/background\n5:freezer:/apps/manager\n";
    files["/dev/freezer/apps/manager/freezer.state"] = "FROZEN\n";
    assert(status() == FreezerState::FROZEN);
    files["/dev/freezer/apps/manager/freezer.state"] = "FREEZING\n";
    assert(status() == FreezerState::FROZEN);
    files["/dev/freezer/apps/manager/freezer.state"] = "invalid\n";
    assert(status() == FreezerState::UNKNOWN);
    files["/dev/freezer/apps/manager/freezer.state"] = "THAWED\n";
    assert(status() == FreezerState::NORMAL);
    result = isolate_freezer_hierarchies(7258, read,
        [&](const FreezerHierarchy &group, int pid, std::string &) {
            assert(pid == 7258 && !group.unified && group.mount == "/dev/freezer");
            files["/proc/7258/cgroup"] = "2:cpu:/background\n5:freezer:/\n";
            return true;
        });
    assert(result.ok && result.moved == 1);
    assert(status() == FreezerState::NORMAL);
    std::cout << "cgroup v2/v1 freezer discovery, self-only migration, verification, failure and frozen status tests passed\n";
}
