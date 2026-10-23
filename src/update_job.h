#pragma once

#include <cstddef>
#include <string>
#include "process_identity.h"

struct UpdateJobState {
    std::string status;
    int pid = -1;
    int exit_code = -1;
    bool legacy = true;
    std::string id, type, target, channel, phase, reason;
    ProcessIdentity owner;
    uint64_t executable_device = 0, executable_inode = 0, created_at = 0, updated_at = 0;
};

enum class UpdateJobLife { ALIVE, INTERRUPTED, UNKNOWN, FINISHED };
struct UpdateJobObservation {
    UpdateJobState state;
    std::string status = "invalid_state", reason;
    UpdateJobLife life = UpdateJobLife::UNKNOWN;
    // -1 = unable to verify; 0 = quiescent; 1 = an owner/descendant holds it.
    int lease = -1;
};

std::string serialize_update_job(const UpdateJobState &state);
bool capture_update_job_owner(UpdateJobState &state, int pid, const std::string &proc = "/proc");
UpdateJobLife update_job_life(const UpdateJobState &state, const std::string &id,
    const std::string &proc = "/proc");
UpdateJobObservation observe_update_job(const std::string &directory, const std::string &id,
    const std::string &proc = "/proc");

bool valid_update_job_id(const std::string &job_id);
bool parse_update_job_state(const std::string &text, UpdateJobState &state);
std::string running_update_job_state(int pid);
std::string finished_update_job_state(int exit_code);

// 只清理已结束/已失联的旧任务,保留最新 keep 个；运行中的任务不删除。
size_t prune_update_jobs(const std::string &directory, size_t keep, const std::string &proc = "/proc");
