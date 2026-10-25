#pragma once

#include <fcntl.h>
#include <functional>
#include <string>
#include "operation_gate.h"

// Process-local worker context. The lease is inherited by network/installer
// descendants so a killed owner never permits cleanup under an active writer.
inline int task_lease_fd = -1;
// Once an installer was dispatched, an unknown completion must preserve its
// durable installing barrier, even if the manager closed inherited leases.
inline bool task_installer_completion_unknown = false;
// Preserve paths/leases and refuse further downloads if a writer's exit was
// not confirmed. Shared with paired-source cleanup; never guess quiescence.
inline thread_local bool task_unconfirmed_network_writer = false;
inline std::function<bool(const std::string &)> task_progress_callback;
inline bool task_progress(const std::string &phase) {
    return !task_progress_callback || task_progress_callback(phase);
}
inline bool inherit_task_lease() {
    for (const int fd : {task_lease_fd, operation_lease_fd}) {
        if (fd < 0) continue;
        const int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC)) return false;
    }
    return true;
}
