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
#include <sys/file.h>
#include <unordered_map>
#include <sstream>
#include <ctime>
#include <cstring>
#include "common.h"

namespace {

bool parse_nonnegative_int(const std::string &text, int &value) {
    uint64_t parsed = 0;
    if (!parse_u64(text, parsed) || parsed > INT_MAX) return false;
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
    if (text.size() > 8192) return false;
    if (text.compare(0, 10, "ADK_JOB_2\n") == 0) {
        if (text.size() > 8192 || text.back() != '\n' || text.find('\0') != std::string::npos) return false;
        std::unordered_map<std::string, std::string> fields;
        std::istringstream input(text.substr(10)); std::string line;
        while (std::getline(input, line)) {
            const auto at = line.find('=');
            if (at == std::string::npos || !at || !fields.emplace(line.substr(0, at), line.substr(at + 1)).second) return false;
        }
        if (fields.size() != 16) return false;
        for (const char *key : {"id", "type", "target", "channel", "phase", "reason", "status", "pid", "exit", "boot", "started", "device", "inode", "created", "updated", "format"})
            if (!fields.count(key)) return false;
        state.legacy = false; state.id = fields["id"]; state.type = fields["type"];
        state.target = fields["target"]; state.channel = fields["channel"]; state.phase = fields["phase"];
        state.reason = fields["reason"]; state.status = fields["status"];
        if (!valid_update_job_id(state.id) || fields["format"] != "2" ||
            (state.type != "rules" && state.type != "source" && state.type != "download" && state.type != "install") ||
            (state.channel != "stable" && state.channel != "preview") || state.target.size() > 4096 ||
            state.phase.empty() || state.phase.size() > 64 || state.reason.size() > 512) return false;
        for (const auto &value : {state.target, state.phase, state.reason})
            for (unsigned char ch : value) if (ch < 32 || ch == 127) return false;
        if (!parse_nonnegative_int(fields["pid"], state.pid) || !state.pid ||
            !parse_u64(fields["started"], state.owner.started) || !parse_u64(fields["device"], state.executable_device) ||
            !parse_u64(fields["inode"], state.executable_inode) || !state.executable_inode ||
            !parse_u64(fields["created"], state.created_at) || !state.created_at ||
            !parse_u64(fields["updated"], state.updated_at) || !state.updated_at) return false;
        state.owner.pid = state.pid; state.owner.boot = fields["boot"];
        if (!state.owner.valid()) return false;
        if (state.status == "running") return fields["exit"] == "-1";
        if (state.status != "success" && state.status != "failed") return false;
        return parse_nonnegative_int(fields["exit"], state.exit_code) && (state.status == "success") == (state.exit_code == 0);
    }
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

std::string serialize_update_job(const UpdateJobState &state) {
    const auto text = "ADK_JOB_2\nid=" + state.id + "\ntype=" + state.type + "\ntarget=" + state.target +
        "\nchannel=" + state.channel + "\nphase=" + state.phase + "\nreason=" + state.reason + "\nstatus=" + state.status +
        "\npid=" + std::to_string(state.owner.pid) + "\nexit=" + std::to_string(state.exit_code) + "\nboot=" + state.owner.boot +
        "\nstarted=" + std::to_string(state.owner.started) + "\ndevice=" + std::to_string(state.executable_device) +
        "\ninode=" + std::to_string(state.executable_inode) + "\ncreated=" + std::to_string(state.created_at) +
        "\nupdated=" + std::to_string(state.updated_at) + "\nformat=2\n";
    UpdateJobState checked;
    return parse_update_job_state(text, checked) ? text : "";
}

bool capture_update_job_owner(UpdateJobState &state, int pid, const std::string &proc) {
    state.owner = process_identity(pid, proc); state.pid = pid;
    struct stat executable {};
    const auto path = proc + "/" + std::to_string(pid) + "/exe";
    if (!state.owner.valid() || stat(path.c_str(), &executable) || !S_ISREG(executable.st_mode)) return false;
    state.executable_device = static_cast<uint64_t>(executable.st_dev);
    state.executable_inode = static_cast<uint64_t>(executable.st_ino);
    return state.executable_inode && process_identity(pid, proc) == state.owner;
}

UpdateJobLife update_job_life(const UpdateJobState &state, const std::string &id, const std::string &proc) {
    if (state.status != "running") return UpdateJobLife::FINISHED;
    // Legacy PID-only records can never prove an active worker. No signals
    // are sent, including when the PID points to another process.
    if (state.legacy || state.id != id || !state.owner.valid()) return UpdateJobLife::INTERRUPTED;
    std::string boot;
    if (read_regular_bounded(proc + "/sys/kernel/random/boot_id", boot, 64) != BoundedRead::OK) return UpdateJobLife::UNKNOWN;
    if (!boot.empty() && boot.back() == '\n') boot.pop_back();
    if (!ProcessIdentity{1, boot, 1}.valid()) return UpdateJobLife::UNKNOWN;
    if (boot != state.owner.boot) return UpdateJobLife::INTERRUPTED;
    const auto process = proc + "/" + std::to_string(state.pid);
    struct stat info {};
    if (lstat(process.c_str(), &info)) return errno == ENOENT ? UpdateJobLife::INTERRUPTED : UpdateJobLife::UNKNOWN;
    const auto actual = process_identity(state.pid, proc);
    if (!actual.valid()) return UpdateJobLife::UNKNOWN;
    if (!(actual == state.owner)) return UpdateJobLife::INTERRUPTED;
    if (stat((process + "/exe").c_str(), &info)) return UpdateJobLife::UNKNOWN;
    if (!S_ISREG(info.st_mode) || static_cast<uint64_t>(info.st_dev) != state.executable_device ||
        static_cast<uint64_t>(info.st_ino) != state.executable_inode) return UpdateJobLife::INTERRUPTED;
    return process_identity(state.pid, proc) == state.owner ? UpdateJobLife::ALIVE : UpdateJobLife::UNKNOWN;
}

UpdateJobObservation observe_update_job(const std::string &directory, const std::string &id, const std::string &proc) {
    UpdateJobObservation observed;
    if (!valid_update_job_id(id)) { observed.status = "invalid_job"; return observed; }
    const auto base = directory + "/update-" + id;
    std::string raw;
    const auto read = read_regular_bounded(base + ".state", raw, 8192);
    if (read != BoundedRead::OK) { observed.status = read == BoundedRead::MISSING ? "not_found" : "unverified"; return observed; }
    if (!parse_update_job_state(raw, observed.state) || (!observed.state.legacy && observed.state.id != id)) return observed;
    const int fd = open((base + ".lease").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat info {};
    if (fd >= 0) {
        if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_uid == geteuid() && info.st_nlink == 1) {
            if (!flock(fd, LOCK_EX | LOCK_NB)) observed.lease = 0;
            else if (errno == EWOULDBLOCK || errno == EAGAIN) observed.lease = 1;
        }
        close(fd);
    } else if (errno == ENOENT && observed.state.legacy) observed.lease = 0;
    observed.life = update_job_life(observed.state, id, proc);
    // An installer may intentionally close inherited descriptors. Once it was
    // dispatched, disappearance of its owner is not proof it finished. Only a
    // terminal journal or a verified later boot makes this stage quiescent.
    if (!observed.state.legacy && observed.state.status == "running" && observed.state.phase == "installing" &&
        observed.life != UpdateJobLife::ALIVE) {
        std::string boot;
        const bool readable = read_regular_bounded(proc + "/sys/kernel/random/boot_id", boot, 64) == BoundedRead::OK;
        if (!boot.empty() && boot.back() == '\n') boot.pop_back();
        if (!readable || !ProcessIdentity{1, boot, 1}.valid() || boot == observed.state.owner.boot) {
            observed.status = "unverified"; observed.reason = "installer_completion_unconfirmed"; return observed;
        }
    }
    if (observed.life == UpdateJobLife::FINISHED) {
        if (observed.lease == 0) observed.status = observed.state.status;
        else if (observed.lease == 1) { observed.status = "running"; observed.state.phase = "finishing"; }
        else observed.status = "unverified";
    }
    else if (observed.life == UpdateJobLife::ALIVE && observed.lease == 1) observed.status = "running";
    else if (observed.life == UpdateJobLife::INTERRUPTED && observed.lease == 0) {
        observed.status = "interrupted";
        observed.reason = observed.state.legacy ? "legacy_pid_only_record" : "worker_identity_no_longer_matches";
    }
    else { observed.status = "unverified"; observed.reason = "owner_or_descendant_unverified"; }
    return observed;
}

namespace {
bool valid_log_epoch(const std::string &value) {
    return !value.empty() && value.size() <= 128 && value.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-") == std::string::npos;
}
std::string job_log_base64(const std::string &data) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < data.size(); i += 3) {
        const auto a = static_cast<unsigned char>(data[i]);
        const auto b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const auto c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        out += alphabet[a >> 2]; out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += i + 1 < data.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out += i + 2 < data.size() ? alphabet[c & 63] : '=';
    }
    return out;
}
ssize_t job_log_pread(int fd, void *data, size_t size, off_t offset) {
    ssize_t count;
    do { count = pread(fd, data, size, offset); } while (count < 0 && errno == EINTR);
    return count;
}
}

UpdateJobLogPage read_update_job_log(const std::string &directory, const std::string &id,
                                    const std::string &cursor, size_t limit) {
    UpdateJobLogPage page;
    if (!valid_update_job_id(id)) { page.status = "invalid_job"; return page; }
    std::string requested_epoch; uint64_t offset = 0;
    const bool tail = cursor == "tail";
    if (!cursor.empty() && !tail) {
        const auto colon = cursor.find(':');
        if (cursor.size() > 150 || colon == std::string::npos ||
            !valid_log_epoch(cursor.substr(0, colon)) || !parse_u64(cursor.substr(colon + 1), offset)) {
            page.status = "invalid_cursor"; return page;
        }
        requested_epoch = cursor.substr(0, colon);
    }
    limit = std::min<size_t>(16384, limit);
    if (!limit) { page.status = "invalid_limit"; return page; }
    const auto path = directory + "/update-" + id + ".log";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { page.status = errno == ENOENT ? "not_ready" : "error"; return page; }
    struct stat info {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 ||
        info.st_size < 0 || static_cast<uint64_t>(info.st_size) > UPDATE_LOG_MAX_BYTES) { close(fd); return page; }
    char header[160];
    const auto count = job_log_pread(fd, header, sizeof(header), 0);
    if (count < 0) { close(fd); return page; }
    std::string first(header, static_cast<size_t>(count));
    const std::string prefix = "# ADK_JOB_LOG=";
    std::string epoch = "legacy-" + std::to_string(static_cast<uint64_t>(info.st_dev)) + "-" + std::to_string(static_cast<uint64_t>(info.st_ino));
    uint64_t begin = 0;
    if (first.compare(0, prefix.size(), prefix) == 0) {
        const auto newline = first.find('\n');
        if (newline == std::string::npos || !valid_log_epoch(first.substr(prefix.size(), newline - prefix.size()))) {
            close(fd); page.status = "not_ready"; return page;
        }
        epoch = first.substr(prefix.size(), newline - prefix.size()); begin = newline + 1;
    }
    const uint64_t end = static_cast<uint64_t>(info.st_size);
    if (begin > end) { close(fd); page.status = "not_ready"; return page; }
    if (tail) {
        offset = end > limit && end - limit > begin ? end - limit : begin;
        page.reset = true; page.reset_reason = "tail"; page.omitted_bytes = offset - begin;
    } else if (cursor.empty() || epoch != requested_epoch || offset < begin || offset > end) {
        offset = begin; page.reset = true;
        page.reset_reason = cursor.empty() ? "begin" : epoch != requested_epoch ? "rotated" : "truncated";
    }
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(limit, end - offset));
    std::string data(wanted, '\0'); size_t read = 0;
    while (read < wanted) {
        const auto received = job_log_pread(fd, &data[read], wanted - read, static_cast<off_t>(offset + read));
        if (received <= 0) { close(fd); page.status = "not_ready"; return page; }
        read += static_cast<size_t>(received);
    }
    // An in-place rotation during the read must not advance a mixed cursor.
    if (begin) {
        char again[160];
        const auto got = job_log_pread(fd, again, static_cast<size_t>(begin), 0);
        if (got != static_cast<ssize_t>(begin) || memcmp(again, first.data(), static_cast<size_t>(begin))) {
            close(fd); page.status = "not_ready"; return page;
        }
    }
    if (close(fd)) return page;
    page.status = "ok"; page.data_base64 = job_log_base64(data);
    page.cursor = epoch + ":" + std::to_string(offset + read);
    page.more = offset + read < end;
    return page;
}

std::string running_update_job_state(int pid) {
    return pid > 0 ? "running:" + std::to_string(pid) + "\n" : "";
}

std::string finished_update_job_state(int exit_code) {
    if (exit_code < 0) return "";
    return std::string(exit_code == 0 ? "success:" : "failed:") +
           std::to_string(exit_code) + "\n";
}

size_t prune_update_jobs(const std::string &directory, size_t keep, const std::string &proc) {
    struct FinishedJob {
        std::string id;
        time_t modified = 0;
    };
    std::vector<FinishedJob> finished;
    const int directory_fd = open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    struct stat root {};
    if (directory_fd < 0) return 0;
    if (fstat(directory_fd, &root) || root.st_uid != geteuid() || (root.st_mode & 0022)) { close(directory_fd); return 0; }
    DIR *dir = fdopendir(directory_fd);
    if (!dir) { close(directory_fd); return 0; }
    std::vector<std::string> protected_ids;
    for (const char *family : {"rules", "module"}) {
        std::string id;
        const auto result = read_regular_bounded(directory + "/.active-" + family, id, 128);
        if (result == BoundedRead::MISSING) continue;
        if (!id.empty() && id.back() == '\n') id.pop_back();
        if (result != BoundedRead::OK || !valid_update_job_id(id)) { closedir(dir); return 0; }
        protected_ids.push_back(id);
    }
    size_t entries = 0;
    while (true) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (!entry) { if (errno) { closedir(dir); return 0; } break; }
        if (++entries > 4096) { closedir(dir); return 0; }
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
        if (std::find(protected_ids.begin(), protected_ids.end(), id) != protected_ids.end()) continue;
        std::string state_path = directory + "/" + name;
        const auto observed = observe_update_job(directory, id, proc);
        if (observed.lease != 0 || (observed.status != "success" && observed.status != "failed" && observed.status != "interrupted")) continue;
        struct stat info {};
        if (lstat(state_path.c_str(), &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) continue;
        // Old PID-only records are no longer reported alive. Give an old
        // version's untracked worker a migration grace period before pruning.
        if (observed.state.legacy && observed.state.status == "running" &&
            (time(nullptr) <= info.st_mtime || time(nullptr) - info.st_mtime < 24 * 60 * 60)) continue;
        finished.push_back({id, info.st_mtime});
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
        const auto observed = observe_update_job(directory, finished[i].id, proc);
        if (observed.lease != 0) continue;
        bool safe = true;
        for (const char *suffix : {".state", ".log", ".lease"}) {
            struct stat info {};
            if (lstat((base + suffix).c_str(), &info)) { if (errno != ENOENT) safe = false; }
            else if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1) safe = false;
        }
        if (!safe || unlink((base + ".state").c_str())) continue;
        ++removed; unlink((base + ".log").c_str()); unlink((base + ".lease").c_str());
    }
    return removed;
}
