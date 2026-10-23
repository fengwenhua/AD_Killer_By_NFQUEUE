// Observe actual static CLI functions with all effectful dependencies stubbed.
// This deliberately asserts CURRENT defects, not the desired fixed behavior.
#define main static unused_project_main
#include "../../src/main.cpp"
#undef main

#include <cassert>
#include <map>

namespace {
std::map<std::string, std::string> files;
std::string runtime;
bool entered_daemon = false;
bool routes_left = true;
int removal_attempts = 0;
}

// Cleanup/job characterization isolates the already-tested lifecycle protocol.
// Its desired behavior is in tests/test_lifecycle_cli.py, not asserted defective.
namespace lifecycle {
bool begin(const std::string &, Action, Ticket &ticket, std::string &) { ticket = {"isolated", false}; return true; }
bool current(const std::string &, const Ticket &) { return true; }
}

// No real logging, process migration, daemon, or firewall implementation links.
void configure_process_log(const char *, size_t, int) {}
void log_line(const char *, const char *, ...) {}
bool write_file_atomic(const char *path, const std::string &value) {
    files[path] = value;
    return true;
}
bool read_file(const char *path, std::string &value) {
    auto found = files.find(path);
    if (found == files.end()) return false;
    value = found->second;
    return true;
}
std::string read_conf_value(const char *, const char *key) {
    return std::string(key) == "manual_stop" ? "true" : "false";
}
bool write_conf_value(const char *, const char *, const char *) { return true; }
bool write_runtime_state(const char *value) { runtime = value; return true; }
int read_daemon_pid() { return static_cast<int>(getpid()); }
bool is_daemon_pid(int) { return false; }  // Never signal any real process.
bool write_daemon_pid(int) { return true; }
void clear_daemon_pid_if(int) {}
void record_engine_error(const std::string &, const std::string &) {}
CgroupIsolationResult isolate_daemon_cgroups() { return {true, 0, ""}; }
int run_daemon() { entered_daemon = true; return 0; }
bool iptables_any_left() { return routes_left; }
IptablesCleanupResult iptables_remove_all(const IptablesCleanupLimits &) {
    ++removal_attempts; return {FirewallCleanupState::PARTIAL, {"injected routes"}, {"injected cleanup failure"}};
}

int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string operation = argv[1];
    if (operation == "unrelated-pid") {
        const std::string id = "123-456";
        files[update_job_path(id, ".state")] = running_update_job_state(static_cast<int>(getpid()));
        assert(cmd_update_status(id) == 0);
        puts("OBSERVED unrelated-pid: synthetic old job is reported running using audit process PID");
    } else {
        return 2;
    }
}
