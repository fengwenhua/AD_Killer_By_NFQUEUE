#pragma once

#include <fcntl.h>
#include <functional>
#include <string>
#include "operation_gate.h"

// Process-local worker context. The lease is inherited by network/installer
// descendants so a killed owner never permits cleanup under an active writer.
inline int task_lease_fd = -1;
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
