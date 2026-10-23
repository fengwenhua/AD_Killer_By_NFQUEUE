#pragma once

#include <cerrno>
#include <chrono>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

struct OwnedChildExit {
    bool reaped = false, group_gone = false;
    int exit_code = -1;
};

// Only a direct, still-unreaped child is eligible. The caller must use
// waitid(WNOWAIT), never pass a PID recovered from a file or process listing.
// Keeping the leader reserved prevents terminating an unrelated reused group.
inline OwnedChildExit finish_owned_child(pid_t child, bool owned, bool group_owned = true,
                                         unsigned budget_ms = 200) {
    OwnedChildExit result;
    if (owned) {
        if (group_owned) kill(-child, SIGKILL);
        kill(child, SIGKILL);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    int status = 0;
    while (owned) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) { result.reaped = true; break; }
        if (waited < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        usleep(1000);
    }
    if (result.reaped) result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    // No terminating signals after reaping. Signal 0 can conservatively report
    // unknown if a new group or an unreaped descendant still has this number.
    errno = 0;
    result.group_gone = group_owned && result.reaped && kill(-child, 0) < 0 && errno == ESRCH;
    return result;
}
