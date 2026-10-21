#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    source = (ROOT / "src/engine.cpp").read_text(encoding="utf-8")
    handler = source.split("static void sig_handler(int sig)", 1)[1].split(
        "static bool install_signal_handlers()", 1
    )[0]
    assert "g_signal_stop = 1" in handler
    assert "g_signal_reload = 1" in handler
    assert ".store(" not in handler, "signal handler must only use sig_atomic_t"

    daemon = source.split("int run_daemon()", 1)[1]
    handlers = daemon.index("install_signal_handlers()")
    remove_rules = daemon.index("iptables_remove_all()")
    setup_queue = daemon.index("setup_queue(")
    install_rules = daemon.index("iptables_install(")
    assert handlers < remove_rules < setup_queue < install_rules, (
        "signal handlers must be active before any queue/firewall mutation"
    )
    assert handlers < daemon.index("find_queue_conflict()") < remove_rules, (
        "foreign queue preflight must reject before any firewall cleanup"
    )
    assert 'write_runtime_state(g_fault.load() ? "error" : "stopped")' in daemon
    assert 'record_engine_error("queue_receive_failed"' in source
    assert daemon.count("if (stop_requested())") >= 4, (
        "startup stages must observe SIGTERM and clean up before continuing"
    )

    main_source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
    entry = main_source.split("static int cmd_daemon()", 1)[1].split(
        "static int cmd_stop()", 1
    )[0]
    assert entry.index('write_runtime_state("starting")') < entry.index("write_daemon_pid("), (
        "direct daemon recovery must reset stale running state before publishing its PID"
    )
    assert entry.index("prepare_daemon_cgroups()") < entry.index("write_daemon_pid(")
    assert entry.index("prepare_daemon_cgroups()") < entry.index("run_daemon()")
    start = main_source.split("static int cmd_start()", 1)[1].split(
        "static int cmd_daemon()", 1
    )[0]
    assert start.index("prepare_daemon_cgroups()") < start.index("pid_t pid = fork()")
    assert start.index("prepare_daemon_cgroups()") < start.index("export ADK_NFQ_START_DISPATCHED")
    assert '"cgroup_isolation_failed"' in main_source
    assert "daemon_cgroup_frozen(old)" in start
    recovery = start.split('if (read_runtime_state() == "running")', 1)[1].split(
        "if (!dispatched && old <= 0)", 1
    )[0]
    assert recovery.index("iptables_chain_active(routes)") < recovery.index("kill(old, SIGTERM)")
    assert "is_daemon_pid(old)" in recovery and "STOP_WAIT_MS" in recovery
    assert "SIGKILL" not in recovery and "iptables_remove_all()" not in recovery
    assert '"recovery_stop_timeout"' in recovery
    update = main_source.split("static int cmd_update_start(", 1)[1].split(
        "static int cmd_update_status(", 1
    )[0]
    assert update.index("isolate_daemon_cgroups()") < update.index("cmd_update(test_url)"), (
        "background source jobs must detach from the manager's freezer before downloads"
    )
    for name, end in (("cmd_status", "// --status-fast"), ("cmd_status_fast", "// --version-status")):
        status = main_source.split(f"static int {name}()", 1)[1].split(end, 1)[0]
        assert "bool running = pid > 0 && !frozen" in status
        assert "engine_diagnostics_json(frozen)" in status
    firewall = (ROOT / "src/iptables.cpp").read_text(encoding="utf-8")
    assert "-n -S" not in firewall, "numeric listing flags are invalid with list-rules"

    print("engine lifecycle contract tests passed")


if __name__ == "__main__":
    main()
