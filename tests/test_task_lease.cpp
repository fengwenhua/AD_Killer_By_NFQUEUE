#include "exclusive_file_lock.h"
#include "task_context.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
pid_t owned_child = -1;
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
}
int main(int argc, char **argv) {
    if (argc == 5 && std::string(argv[1]) == "--holder") {
        const int lease = std::atoi(argv[2]), ready = std::atoi(argv[3]), release = std::atoi(argv[4]);
        char value = 0;
        return fcntl(lease, F_GETFD) >= 0 && write(ready, "1", 1) == 1 && read(release, &value, 1) == 1 ? 0 : 2;
    }
    char path[] = "/tmp/adk-task-lease.XXXXXX";
    const int created = mkstemp(path);
    if (created < 0) return 1;
    close(created);
    try {
        int ready[2], release[2]; check(!pipe(ready) && !pipe(release), "pipes");
        {
            ExclusiveFileLock lease(path); check(lease.ok(), "parent lease");
            owned_child = fork(); check(owned_child >= 0, "fork");
            if (!owned_child) {
                close(ready[0]); close(release[1]); task_lease_fd = lease.native_fd();
                if (!inherit_task_lease()) _exit(2);
                const auto fd = std::to_string(task_lease_fd), ready_fd = std::to_string(ready[1]), release_fd = std::to_string(release[0]);
                execl(argv[0], argv[0], "--holder", fd.c_str(), ready_fd.c_str(), release_fd.c_str(), static_cast<char *>(nullptr));
                _exit(127);
            }
            close(ready[1]); close(release[0]); char value = 0;
            check(read(ready[0], &value, 1) == 1 && value == '1', "exec descendant inherited lease");
        }
        { ExclusiveFileLock probe(path); check(!probe.ok(), "owner close cannot release live exec descendant"); }
        check(write(release[1], "1", 1) == 1, "release descendant");
        int status = 0;
        check(waitpid(owned_child, &status, 0) == owned_child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "owned child exited"); owned_child = -1;
        { ExclusiveFileLock probe(path); check(probe.ok(), "last holder releases lease"); }
        close(ready[0]); close(release[1]);
    } catch (const std::exception &error) {
        if (owned_child > 0) { kill(owned_child, SIGKILL); waitpid(owned_child, nullptr, 0); }
        unlink(path); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
    unlink(path);
    std::cout << "task lease survives owner close and descendant exec, releases only at last exit\n";
}
