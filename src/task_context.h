#pragma once

#include <fcntl.h>
#include <functional>
#include <string>

// Process-local worker context. The lease is inherited by network/installer
// descendants so a killed owner never permits cleanup under an active writer.
inline int task_lease_fd = -1;
inline std::function<bool(const std::string &)> task_progress_callback;
inline bool task_progress(const std::string &phase) {
    return !task_progress_callback || task_progress_callback(phase);
}
inline bool inherit_task_lease() {
    if (task_lease_fd < 0) return true;
    const int flags = fcntl(task_lease_fd, F_GETFD);
    return flags >= 0 && fcntl(task_lease_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0;
}
