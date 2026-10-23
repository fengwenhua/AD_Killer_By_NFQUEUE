// Test-only PID identity adapter: macOS has no /proc. All other configuration,
// atomic file and log functions below are the actual production implementation.
#define is_daemon_pid production_is_daemon_pid
#define read_daemon_pid production_read_daemon_pid
#define write_daemon_pid production_write_daemon_pid
#define clear_daemon_pid_if production_clear_daemon_pid_if
#include "../src/logging.cpp"
