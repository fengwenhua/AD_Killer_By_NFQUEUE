#pragma once

#include <cstddef>
#include <string>

struct UpdateJobState {
    std::string status;
    int pid = -1;
    int exit_code = -1;
};

bool valid_update_job_id(const std::string &job_id);
bool parse_update_job_state(const std::string &text, UpdateJobState &state);
std::string running_update_job_state(int pid);
std::string finished_update_job_state(int exit_code);

// 只清理已结束/已失联的旧任务,保留最新 keep 个；运行中的任务不删除。
size_t prune_update_jobs(const std::string &directory, size_t keep);
