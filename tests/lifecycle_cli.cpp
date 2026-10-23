// Actual CLI control flow, real locks/config/files. No kernel/network effects.
// Only PID identity, freezer migration and engine/firewall execution are stubs.
#define main static unused_project_main
#include "../src/main.cpp"
#undef main
#include <cassert>

namespace {
volatile sig_atomic_t stopped = 0;
void term(int) { stopped = 1; }
void barrier(const char *name) {
    const auto prefix = DATA_FILE(name);
    if (access((prefix + ".pause").c_str(), F_OK) != 0) return;
    assert(write_file_atomic((prefix + ".entered").c_str(), "entered"));
    for (int i = 0; i < 5000; ++i) {
        if (access((prefix + ".release").c_str(), F_OK) == 0) return;
        usleep(1000);
    }
    _exit(90); // A test failure must not hang a CI worker indefinitely.
}
}
bool is_daemon_pid(int pid) {
    std::string text;
    return pid > 0 && read_file(DAEMON_PID_FILE, text) && text == std::to_string(pid) && kill(pid, 0) == 0;
}
int read_daemon_pid() {
    std::string text; int pid = 0;
    return read_file(DAEMON_PID_FILE, text) && parse_integer_arg(text.c_str(), pid) && is_daemon_pid(pid) ? pid : -1;
}
bool write_daemon_pid(int pid) { return write_file_atomic(DAEMON_PID_FILE, std::to_string(pid)); }
void clear_daemon_pid_if(int pid) {
    std::string text;
    if (read_file(DAEMON_PID_FILE, text) && text == std::to_string(pid)) unlink(DAEMON_PID_FILE);
}
CgroupIsolationResult isolate_daemon_cgroups() { barrier("startup"); return {true, 0, ""}; }
bool iptables_any_left() { return access(DATA_FILE("test.routes").c_str(), F_OK) == 0; }
void iptables_remove_all() { barrier("cleanup"); unlink(DATA_FILE("test.routes").c_str()); }
int run_daemon() {
    signal(SIGTERM, term); signal(SIGINT, term);
    assert(write_file_atomic(DATA_FILE("test.routes").c_str(), std::to_string(getpid())));
    assert(write_file_atomic(DATA_FILE("daemon.started").c_str(), std::to_string(getpid())));
    assert(write_runtime_state("running"));
    while (!stopped) usleep(1000);
    unlink(DATA_FILE("test.routes").c_str());
    write_runtime_state("stopped"); return 0;
}
int main(int argc, char **argv) {
    assert(argc >= 2);
    if (strcmp(argv[1], "root") == 0) {
        assert(DATA_FILE("marker") == std::string(DATA_DIR) + "/marker");
        puts(DATA_DIR); return 0;
    }
    if (strcmp(argv[1], "--daemon") == 0) return cmd_daemon();
    if (strcmp(argv[1], "--stop") == 0) return cmd_stop();
    if (strcmp(argv[1], "--autostart-off") == 0) return cmd_toggle("autostart", false);
    lifecycle::Ticket request; std::string error;
    const auto action = strcmp(argv[1], "begin-boot") == 0 ? lifecycle::Action::BOOT : lifecycle::Action::START;
    if (!lifecycle::begin(DATA_DIR, action, request, error) || !lifecycle::enable(DATA_DIR, request, error)) return 2;
    puts(request.revision.c_str()); return 0;
}
