#include "lifecycle.h"
#include "exclusive_file_lock.h"
#include "logging.h"
#include "sha256.h"
#include <chrono>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace lifecycle {
namespace {
struct Record { Ticket ticket; std::string action; };
bool trusted_root(const std::string &root) {
    struct stat info {};
    return lstat(root.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
        info.st_uid == geteuid() && !(info.st_mode & (S_IWGRP | S_IWOTH));
}
bool bounded_read(const std::string &path, std::string &text, bool missing_ok = false) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return missing_ok && errno == ENOENT;
    struct stat info {};
    bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 && info.st_size <= 16384;
    char buffer[2048]; text.clear();
    while (ok) {
        const auto n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && text.size() + static_cast<size_t>(n) > 16384)) { ok = false; break; }
        if (!n) break;
        text.append(buffer, static_cast<size_t>(n));
    }
    if (close(fd) != 0) ok = false;
    return ok;
}
bool load(const std::string &root, Record &record) {
    std::string text, boot, extra;
    if (!bounded_read(root + "/lifecycle.intent", text)) return false;
    std::istringstream input(text);
    if (!(input >> record.ticket.revision >> record.action >> boot) || (input >> extra)) return false;
    record.ticket.boot = boot == "1";
    return record.ticket.revision.size() == 64 && record.ticket.revision.find_first_not_of("0123456789abcdef") == std::string::npos &&
        (record.action == "start" || record.action == "stop" || record.action == "reboot") && (boot == "0" || boot == "1");
}
bool policy(const std::string &root, bool boot) {
    std::string text;
    if (!bounded_read(root + "/state.conf", text)) return false;
    std::istringstream input(text);
    std::string row, enabled, manual, automatic;
    unsigned enabled_count = 0, manual_count = 0, automatic_count = 0;
    while (std::getline(input, row)) {
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.compare(0, 8, "enabled=") == 0) { enabled = row.substr(8); ++enabled_count; }
        if (row.compare(0, 12, "manual_stop=") == 0) { manual = row.substr(12); ++manual_count; }
        if (row.compare(0, 10, "autostart=") == 0) { automatic = row.substr(10); ++automatic_count; }
    }
    return enabled_count == 1 && (enabled == "true" || enabled == "1") &&
        manual_count <= 1 && (manual.empty() || manual == "false" || manual == "0") &&
        (!boot || (automatic_count == 1 && (automatic == "true" || automatic == "1")));
}
bool matches(const std::string &root, const Ticket &ticket, bool start_only) {
    Record record;
    return !ticket.revision.empty() && load(root, record) && record.ticket.revision == ticket.revision &&
        record.ticket.boot == ticket.boot && (!start_only || record.action == "start");
}
}

bool begin(const std::string &root, Action action, Ticket &ticket, std::string &error) {
    ticket = {}; error.clear();
    if (!trusted_root(root)) { error = "untrusted lifecycle data directory"; return false; }
    ExclusiveFileLock lock((root + "/.intent.lock").c_str(), true, 1000);
    if (!lock.ok()) { error = "lifecycle intent lock unavailable"; return false; }
    if (action == Action::BOOT && !policy(root, true)) { error = "boot policy disabled or paused"; return false; }
    std::string previous;
    if (!bounded_read(root + "/lifecycle.intent", previous, true)) { error = "cannot read lifecycle intent"; return false; }
    const auto time = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string seed = previous + ":" + std::to_string(getpid()) + ":" + std::to_string(time);
    Sha256 hash; hash.update(seed.data(), seed.size());
    ticket = {hash.finish(), action == Action::BOOT};
    const std::string name = action == Action::STOP ? "stop" : action == Action::REBOOT ? "reboot" : "start";
    const auto written = write_file_durable_atomic((root + "/lifecycle.intent").c_str(),
        ticket.revision + " " + name + " " + (ticket.boot ? "1\n" : "0\n"));
    if (!written.committed || !written.durable) { error = "intent persistence not confirmed"; return false; }
    if (action == Action::STOP) {
        const auto state = root + "/state.conf", state_lock = root + "/.state.lock";
        const bool enabled = write_conf_value_with_lock(state.c_str(), "enabled", "false", state_lock.c_str());
        const bool paused = write_conf_value_with_lock(state.c_str(), "manual_stop", "true", state_lock.c_str());
        if (!enabled || !paused) { error = "pause persistence not confirmed"; return false; }
    }
    return true;
}
bool current(const std::string &root, const Ticket &ticket) {
    if (!trusted_root(root)) return false;
    ExclusiveFileLock lock((root + "/.intent.lock").c_str(), true, 1000);
    return lock.ok() && matches(root, ticket, false);
}
bool enable(const std::string &root, const Ticket &ticket, std::string &error) {
    if (!trusted_root(root)) { error = "untrusted lifecycle data directory"; return false; }
    ExclusiveFileLock lock((root + "/.intent.lock").c_str(), true, 1000);
    if (!lock.ok() || !matches(root, ticket, true) || (ticket.boot && !policy(root, true))) {
        error = "start superseded or boot policy changed"; return false;
    }
    if (ticket.boot) return true;
    const auto state = root + "/state.conf", state_lock = root + "/.state.lock";
    const bool enabled = write_conf_value_with_lock(state.c_str(), "enabled", "true", state_lock.c_str());
    const bool resumed = write_conf_value_with_lock(state.c_str(), "manual_stop", "false", state_lock.c_str());
    if (!enabled || !resumed) { error = "start preference persistence not confirmed"; return false; }
    return true;
}
bool publish(const std::string &root, const Ticket &ticket,
             const std::function<bool()> &publish_pid, std::string &error) {
    if (!trusted_root(root)) { error = "untrusted lifecycle data directory"; return false; }
    ExclusiveFileLock lock((root + "/.intent.lock").c_str(), true, 1000);
    if (!lock.ok() || !matches(root, ticket, true) || !policy(root, ticket.boot)) {
        error = "start superseded, paused or no longer authorized"; return false;
    }
    if (!publish_pid()) { error = "PID publication failed"; return false; }
    return true;
}
bool set_autostart(const std::string &root, bool on, std::string &error) {
    if (!trusted_root(root)) { error = "untrusted lifecycle data directory"; return false; }
    ExclusiveFileLock lock((root + "/.intent.lock").c_str(), true, 1000);
    if (!lock.ok() || !write_conf_value_with_lock((root + "/state.conf").c_str(), "autostart", on ? "true" : "false",
                                                (root + "/.state.lock").c_str())) {
        error = "autostart persistence not confirmed"; return false;
    }
    return true;
}
}
