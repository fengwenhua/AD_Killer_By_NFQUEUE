#include "process_runner.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

ProcessResult run_process(const std::vector<std::string> &arguments, unsigned timeout_ms, size_t output_limit, int inherited_lock) {
    ProcessResult result;
    size_t bytes = 0;
    if (arguments.empty() || arguments.size() > 128 || !timeout_ms) { result.error = "invalid command or expired deadline"; return result; }
    for (const auto &arg : arguments) {
        if (arg.find('\0') != std::string::npos || arg.size() > 65536 || bytes + arg.size() > 128 * 1024) {
            result.error = "invalid or oversized command arguments"; return result;
        }
        bytes += arg.size();
    }
    int channels[6] = {-1, -1, -1, -1, -1, -1};
    const auto close_channels = [&] { for (int &fd : channels) if (fd >= 0) { close(fd); fd = -1; } };
    for (int i = 0; i < 4; i += 2) {
        if (pipe(channels + i) != 0) { result.error = strerror(errno); close_channels(); return result; }
        for (int j = i; j < i + 2; ++j) {
            if (channels[j] <= STDERR_FILENO) {
                const int moved = fcntl(channels[j], F_DUPFD_CLOEXEC, 3);
                close(channels[j]); channels[j] = moved;
            }
            if (channels[j] < 0 || fcntl(channels[j], F_SETFD, FD_CLOEXEC) != 0 ||
                (j % 2 == 0 && fcntl(channels[j], F_SETFL, O_NONBLOCK) != 0)) {
                result.error = strerror(errno); close_channels(); return result;
            }
        }
    }
    if (inherited_lock >= 0) {
        channels[4] = fcntl(inherited_lock, F_DUPFD_CLOEXEC, 3);
        if (channels[4] >= 0) channels[5] = fcntl(inherited_lock, F_DUPFD_CLOEXEC, 3);
        if (channels[4] < 0 || channels[5] < 0) {
            result.error = "cannot lease command ownership lock"; close_channels(); return result;
        }
    }
    std::vector<char *> argv;
    for (const auto &arg : arguments) argv.push_back(const_cast<char *>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int code = posix_spawn_file_actions_init(&actions);
    if (code) { result.error = strerror(code); close_channels(); return result; }
    posix_spawnattr_t attributes;
    code = posix_spawnattr_init(&attributes);
    if (code) { posix_spawn_file_actions_destroy(&actions); result.error = strerror(code); close_channels(); return result; }
    const auto action = [&](int value) { if (!code) code = value; };
    action(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    action(posix_spawn_file_actions_adddup2(&actions, channels[1], STDOUT_FILENO));
    action(posix_spawn_file_actions_adddup2(&actions, channels[3], STDERR_FILENO));
    // A distinct dup2 target clears CLOEXEC. The child retains this flock lease
    // across exec even if its daemon is SIGKILLed before it can cancel/reap it.
    if (channels[4] >= 0) action(posix_spawn_file_actions_adddup2(&actions, channels[4], channels[5]));
    for (int i = 0; i < 5; ++i) if (channels[i] >= 0) action(posix_spawn_file_actions_addclose(&actions, channels[i]));
    action(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    action(posix_spawnattr_setpgroup(&attributes, 0));
    pid_t child = -1;
    if (!code) code = posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions); posix_spawnattr_destroy(&attributes);
    if (code) { result.error = strerror(code); close_channels(); return result; }
    close(channels[1]); channels[1] = -1;
    close(channels[3]); channels[3] = -1;
    for (int i : {4, 5}) if (channels[i] >= 0) { close(channels[i]); channels[i] = -1; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool ended = false, owned = true;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true; result.error = "command deadline exceeded"; break;
        }
        for (int index : {0, 2}) {
            if (channels[index] < 0) continue;
            char buffer[8192];
            while (true) {
                const auto n = read(channels[index], buffer, sizeof(buffer));
                if (n > 0) {
                    if (static_cast<size_t>(n) > output_limit - result.output.size() - result.diagnostic.size()) {
                        result.overflow = true; result.error = "command output limit exceeded"; break;
                    }
                    (index == 0 ? result.output : result.diagnostic).append(buffer, static_cast<size_t>(n));
                } else {
                    if (!n) { close(channels[index]); channels[index] = -1; }
                    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) result.error = strerror(errno);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
            }
            if (!result.error.empty()) break;
        }
        if (!result.error.empty()) break;
        siginfo_t info {};
        // Do not reap the leader while a descendant can still hold the pipe.
        // Keeping the PID reserved prevents killing a reused process group.
        if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) != 0) {
            if (errno == EINTR) continue;
            owned = errno != ECHILD; result.error = "cannot observe command exit"; break;
        }
        ended = info.si_pid == child;
        if (ended && channels[0] < 0 && channels[2] < 0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true; result.error = "command deadline exceeded"; break;
        }
        pollfd descriptors[2] = {{channels[0], POLLIN | POLLHUP, 0}, {channels[2], POLLIN | POLLHUP, 0}};
        if (poll(descriptors, 2, 10) < 0 && errno != EINTR) { result.error = strerror(errno); break; }
    }
    if (owned) {
        // The unreaped leader still reserves this PID/group ID. Also contain
        // background descendants that closed their pipes before the leader.
        kill(-child, SIGKILL); kill(child, SIGKILL);
    }
    close_channels();
    const auto reap_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    int status = 0;
    result.reaped = false;
    while (owned) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { result.reaped = true; break; }
        if (waited < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= reap_deadline) break;
        usleep(1000);
    }
    if (result.reaped) result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    else if (result.error.empty()) result.error = "command termination not confirmed";
    // Signal 0 only after reaping: never send another terminating signal once
    // the PID could be reused. Even an unreaped grandchild remains unknown.
    errno = 0;
    result.group_gone = result.reaped && kill(-child, 0) < 0 && errno == ESRCH;
    if (!result.group_gone && result.error.empty()) result.error = "command group release not confirmed";
    return result;
}
