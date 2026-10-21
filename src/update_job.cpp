#include "update_job.h"

#include <cerrno>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <signal.h>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

namespace {

bool parse_nonnegative_int(const std::string &text, int &value) {
    if (text.empty()) return false;
    errno = 0;
    char *end = nullptr;
    long parsed = strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        parsed < 0 || parsed > INT_MAX)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

}  // namespace

bool valid_update_job_id(const std::string &job_id) {
    if (job_id.empty() || job_id.size() > 96) return false;
    for (char ch : job_id) {
        if ((ch < '0' || ch > '9') && ch != '-') return false;
    }
    return job_id.front() != '-' && job_id.back() != '-';
}

bool parse_update_job_state(const std::string &text, UpdateJobState &state) {
    state = UpdateJobState{};
    std::string value = text;
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
        value.pop_back();
    size_t colon = value.find(':');
    if (colon == std::string::npos) return false;
    std::string kind = value.substr(0, colon);
    std::string number = value.substr(colon + 1);
    if (kind == "running") {
        if (!parse_nonnegative_int(number, state.pid) || state.pid <= 0) return false;
        state.status = "running";
        return true;
    }
    if (kind == "success" || kind == "failed") {
        if (!parse_nonnegative_int(number, state.exit_code)) return false;
        state.status = kind;
        return (kind == "success") == (state.exit_code == 0);
    }
    return false;
}

std::string running_update_job_state(int pid) {
    return pid > 0 ? "running:" + std::to_string(pid) + "\n" : "";
}

std::string finished_update_job_state(int exit_code) {
    if (exit_code < 0) return "";
    return std::string(exit_code == 0 ? "success:" : "failed:") +
           std::to_string(exit_code) + "\n";
}

size_t prune_update_jobs(const std::string &directory, size_t keep) {
    struct FinishedJob {
        std::string id;
        time_t modified = 0;
    };
    std::vector<FinishedJob> finished;
    DIR *dir = opendir(directory.c_str());
    if (!dir) return 0;
    while (dirent *entry = readdir(dir)) {
        std::string name = entry->d_name;
        const std::string prefix = "update-";
        const std::string suffix = ".state";
        if (name.size() <= prefix.size() + suffix.size() ||
            name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        std::string id = name.substr(prefix.size(),
                                     name.size() - prefix.size() - suffix.size());
        if (!valid_update_job_id(id)) continue;
        std::string state_path = directory + "/" + name;
        std::ifstream input(state_path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
        UpdateJobState state;
        if (!parse_update_job_state(raw, state)) continue;
        bool done = state.status == "success" || state.status == "failed";
        if (state.status == "running") {
            errno = 0;
            done = kill(state.pid, 0) != 0 && errno == ESRCH;
        }
        if (!done) continue;
        struct stat info;
        time_t modified = lstat(state_path.c_str(), &info) == 0 ? info.st_mtime : 0;
        finished.push_back({id, modified});
    }
    closedir(dir);
    std::sort(finished.begin(), finished.end(), [](const FinishedJob &left,
                                                   const FinishedJob &right) {
        if (left.modified != right.modified) return left.modified > right.modified;
        return left.id > right.id;
    });
    size_t removed = 0;
    for (size_t i = keep; i < finished.size(); i++) {
        std::string base = directory + "/update-" + finished[i].id;
        if (unlink((base + ".state").c_str()) == 0) removed++;
        unlink((base + ".log").c_str());
    }
    return removed;
}
