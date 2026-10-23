#!/usr/bin/env python3
"""Disposable root Linux only: freeze a synthetic manager, never a real app/job."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

import test_engine as engine


def main():
    assert sys.platform == "linux" and os.geteuid() == 0
    assert not engine.DATA.exists(), "refusing existing module data"
    mount = None
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        left, _, right = line.partition(" - ")
        if right.split()[0] == "cgroup2" and left.split()[3] == "/":
            mount = Path(left.split()[4])
            break
    assert mount is not None, "real freezer regression requires a full-root cgroup2 mount"
    group = mount / f"AD_Killer_By_NFQUEUE_freezer_test_{os.getpid()}"
    group.mkdir()  # Exclusive: never reuse/remove another process's group.
    manager = launcher = None
    daemon_pid = None

    def enter_test_group():
        (group / "cgroup.procs").write_text(str(os.getpid()))

    def freeze(value):
        (group / "cgroup.freeze").write_text(str(value))
        engine.wait_for(lambda: f"frozen {value}" in (group / "cgroup.events").read_text(),
                        f"synthetic manager frozen={value}", 5)

    def status(command="--status-fast"):
        return json.loads(engine.command(engine.BINARY, command).splitlines()[-1])

    try:
        manager = subprocess.Popen(["sleep", "120"], preexec_fn=enter_test_group)
        engine.DATA.parent.mkdir(parents=True, exist_ok=True)
        engine.command(engine.BINARY, "--install")
        (engine.DATA / "custom_rules.txt").write_text("ads.example.com\n")
        # A private mount namespace gives ONLY this attempted daemon a read-only
        # cgroup mount. A failed migration must not bind queues or install rules.
        before = {table: engine.firewall(table, "-S") for table in engine.TABLES}
        denied = subprocess.run([
            "unshare", "--mount", "--propagation", "private", "sh", "-c",
            'mount --bind "$1" "$1" && mount -o remount,bind,ro "$1" && exec "$2" --daemon --foreground',
            "sh", str(mount), engine.BINARY,
        ], preexec_fn=enter_test_group, capture_output=True, text=True, timeout=10)
        assert denied.returncode != 0
        failed = status()
        assert failed["runtime"] == "error" and failed["last_error_code"] == "cgroup_isolation_failed"
        assert not failed["running"] and not (engine.DATA / "daemon.pid").exists()
        for table in engine.TABLES:
            assert engine.firewall(table, "-S") == before[table], "failed isolation changed firewall"
        for entry in ("--daemon", "--start"):
            launcher = subprocess.Popen([engine.BINARY, entry] + (['--foreground'] if entry == '--daemon' else []), preexec_fn=enter_test_group,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if entry == "--start":
                assert launcher.wait(timeout=10) == 0
            engine.wait_for(lambda: engine.state() == "running", f"{entry} running")
            daemon_pid = int((engine.DATA / "daemon.pid").read_text())
            membership = Path(f"/proc/{daemon_pid}/cgroup").read_text()
            assert "0::/\n" in membership and group.name not in membership, membership
            assert str(manager.pid) in (group / "cgroup.procs").read_text().splitlines()
            freeze(1)
            # Actual production engine must continue processing, not just tick.
            for _ in range(2):
                for family in (socket.AF_INET, socket.AF_INET6):
                    engine.udp_dns(family, "safe.example.org", True)
                    engine.udp_dns(family, "ads.example.com", False)
                    engine.tcp_inspection(family, 443, "safe.example.org", True)
                    engine.tcp_inspection(family, 443, "ads.example.com", False)
            current = status()
            assert current["running"] and not current["frozen"] and current["runtime"] == "running"
            assert status("--status")["iptables_active"]
            assert "frozen 1" in (group / "cgroup.events").read_text(), "engine thawed its caller"
            assert group.name in Path(f"/proc/{manager.pid}/cgroup").read_text(), "manager was migrated"

            # Deliberately regress ONLY our disposable engine to prove that both
            # CLI status paths detect frozen workers instead of trusting PID/state.
            (group / "cgroup.procs").write_text(str(daemon_pid))
            engine.wait_for(lambda: status()["frozen"], "frozen daemon detected", 5)
            for command in ("--status-fast", "--status"):
                current = status(command)
                assert current["frozen"] and not current["running"]
                assert current["runtime"] == "error" and current["last_error_code"] == "daemon_frozen"
                assert current["iptables_active"], "status must not pretend queues/rules vanished"
            retry = subprocess.run([engine.BINARY, "--start"], capture_output=True, text=True, timeout=5)
            assert retry.returncode != 0
            assert json.loads(retry.stdout.splitlines()[-1])["status"] == "error_daemon_frozen"
            freeze(0)
            engine.command(engine.BINARY, "--stop")
            if entry == "--daemon":
                assert launcher.wait(timeout=10) == 0
            daemon_pid = None
            assert engine.state() == "stopped"
        print("real manager freeze: read-only mount rejection before queue/firewall changes, "
              "direct/background daemon isolation, dual-stack DNS/TLS latency, "
              "manager preservation and frozen CLI status passed")
    finally:
        # Thaw only the group exclusively created above, even on assertion failure.
        (group / "cgroup.freeze").write_text("0")
        if engine.DATA.exists():
            subprocess.run([engine.BINARY, "--stop"], capture_output=True, timeout=15)
        if launcher is not None:
            launcher.wait(timeout=10)
        if manager is not None:
            manager.terminate()
            manager.wait(timeout=5)
        group.rmdir()
        if engine.DATA.exists():
            shutil.rmtree(engine.DATA)


if __name__ == "__main__":
    main()
