#include "operation_gate.h"
#include "process_runner.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>

namespace fs = std::filesystem;
namespace {
pid_t owned_child = -1;
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void put(const std::string &path, const std::string &value = "1") { std::ofstream file(path); file << value; check(file.good(), "fixture write"); }
bool exclusive(const std::string &path) {
    const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const bool got = !flock(fd, LOCK_EX | LOCK_NB); close(fd); return got;
}
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char temporary[] = "/tmp/adk-operation-gate.XXXXXX";
    const auto *created = mkdtemp(temporary); if (!created) return 1;
    const std::string root = created, data = root + "/data", marker = root + "/module/.uninstalling";
    const auto lock = root + "/.AD_Killer_By_NFQUEUE.operations.lock", self = operation_executable(argv[0]);
    fs::create_directory(root + "/module");
    try {
        {
            OperationGate writer(data, marker, self); check(writer.ok() && operation_lease_fd >= 3, "accepted writer owns shared operation lease");
            const int outer = operation_lease_fd;
            { OperationGate nested(data, marker, self); check(nested.ok() && operation_lease_fd != outer, "nested writer gate"); }
            check(operation_lease_fd == outer, "nested scope restores the outer inherited lease");
            check(!exclusive(lock), "exclusive deletion cannot overlap accepted writer");
            put(marker);
            OperationGate late(data, marker, self);
            check(!late.ok() && !fs::exists(data), "closed admission rejects late writer before mkdir");
            check(!exclusive(lock), "closing admission doesn't release an already-accepted writer");
        }
        check(exclusive(lock), "accepted writers drain by closing, not explicit unlock");
        fs::remove(lock);
        { OperationGate late(data, marker, self); check(!late.ok() && !fs::exists(lock), "late entry cannot recreate removed lock/data while closed"); }
        fs::remove(marker);
        {
            OperationGate reopened(data, marker, self); check(reopened.ok(), "explicit new installation may reopen admission after marker removal");
        }
        fs::remove(lock);
        put(root + "/foreign", "safe"); check(!symlink((root + "/foreign").c_str(), lock.c_str()), "symlink fixture");
        { OperationGate invalid(data, marker, self); check(!invalid.ok(), "gate never follows a symlink"); }
        fs::remove(lock); check(!link((root + "/foreign").c_str(), lock.c_str()), "hardlink fixture");
        { OperationGate invalid(data, marker, self); check(!invalid.ok(), "gate never accepts a multiply-linked lock inode"); }
        fs::remove(lock);
#ifdef __linux__
        { OperationGate invalid(data, marker, root + "/foreign"); check(!invalid.ok(), "path inode must match the actual running image, not a replacement"); }
#endif

        // A killed CLI/daemon must not release the gate of its live external
        // command. This exercises the actual posix_spawn file actions.
        owned_child = fork(); check(owned_child >= 0, "fork owned supervisor");
        if (!owned_child) {
            OperationGate writer(data, marker, self);
            if (!writer.ok()) _exit(2);
            const auto result = run_process({argv[1], "wait-file", root + "/ready", root + "/release"}, 5000, 1024);
            _exit(result.ok() ? 0 : 3);
        }
        for (unsigned i = 0; i < 200 && !fs::exists(root + "/ready"); ++i) usleep(10000);
        check(fs::exists(root + "/ready"), "external command became ready");
        check(!kill(owned_child, SIGKILL), "kill only the known owned supervisor");
        int status = 0; check(waitpid(owned_child, &status, 0) == owned_child, "reap owned supervisor"); owned_child = -1;
        check(!exclusive(lock), "external command still holds the operation lease after parent SIGKILL");
        put(root + "/release");
        for (unsigned i = 0; i < 200 && !exclusive(lock); ++i) usleep(10000);
        check(exclusive(lock), "last external writer exit releases the operation lease");
    } catch (const std::exception &error) {
        put(root + "/release");
        if (owned_child > 0) { kill(owned_child, SIGKILL); waitpid(owned_child, nullptr, 0); }
        fs::remove_all(root); std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
    fs::remove_all(root);
    std::cout << "operation gate: closed admission, no data recreation, safe inode, accepted writers and inherited spawn lease passed\n";
}
