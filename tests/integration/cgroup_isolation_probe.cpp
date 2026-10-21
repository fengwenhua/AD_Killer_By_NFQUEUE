// Device-safe probe: only a forked child joins a newly/exclusively created test
// cgroup. Never migrates another process, freezes a group or touches the engine.
#include "cgroup_isolation.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static std::string text(const char *path) {
    std::ifstream input(path);
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}
int main() {
    std::vector<FreezerHierarchy> groups;
    std::string error, root;
    if (geteuid() != 0 || !discover_freezer_hierarchies(text("/proc/self/cgroup"),
        text("/proc/self/mountinfo"), groups, error)) return 1;
    for (const auto &group : groups) if (group.unified) root = group.mount;
    if (root.empty()) { std::fprintf(stderr, "probe requires cgroup2\n"); return 1; }
    const std::string directory = root + "/AD_Killer_By_NFQUEUE_probe_" + std::to_string(getpid());
    if (mkdir(directory.c_str(), 0700) != 0) { perror("exclusive test cgroup"); return 1; }
    const pid_t child = fork();
    if (child == 0) {
        {
            std::ofstream procs(directory + "/cgroup.procs");
            procs << getpid() << '\n';
            procs.close();
            if (!procs) _exit(2);
        }
        auto result = isolate_daemon_cgroups();
        if (!result.ok || result.moved != 1) {
            std::fprintf(stderr, "isolation failed: %s (moved=%u)\n", result.error.c_str(), result.moved);
            _exit(3);
        }
        auto again = isolate_daemon_cgroups();
        _exit(again.ok && again.moved == 0 ? 0 : 4);
    }
    int status = 0;
    bool ok = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (rmdir(directory.c_str()) != 0) { perror("test cgroup cleanup"); return 1; }
    if (!ok) return 1;
    std::puts("real cgroup child self-migration, read-back verification and idempotence passed; test group removed");
    return 0;
}
