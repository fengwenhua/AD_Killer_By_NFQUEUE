#!/usr/bin/env python3
"""Real foreign NFQUEUE owner: reject before mutations; never stop the owner."""
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys

DATA = Path("/data/adb/AD_Killer_By_NFQUEUE")
BINARY, HOLDER = sys.argv[1:3]


def run(*args):
    return subprocess.run(args, text=True, capture_output=True, timeout=15)


def main():
    assert os.geteuid() == 0 and not DATA.exists()
    queue_file = Path("/proc/net/netfilter/nfnetlink_queue")
    if queue_file.exists():
        assert not {int(row.split()[0]) for row in queue_file.read_text().splitlines()} & {0, 1}
    DATA.parent.mkdir(parents=True, exist_ok=True)
    assert run(BINARY, "--install").returncode == 0
    holder, marker = None, False
    try:
        holder = subprocess.Popen([HOLDER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        with selectors.DefaultSelector() as selector:
            selector.register(holder.stdout, selectors.EVENT_READ)
            assert selector.select(5), "queue holder did not become ready"
            assert holder.stdout.readline().strip() == "READY", "cannot bind real test queues"
        # A stale owned chain would be removed by startup cleanup; conflict
        # preflight must return before reaching that cleanup at all.
        assert run("iptables", "-t", "mangle", "-N", "ADK_NFQ_OUT").returncode == 0
        marker = True
        routes = run("iptables", "-t", "mangle", "-S").stdout
        before = (DATA / "state.conf").read_bytes()
        result = run(BINARY, "--start")
        assert result.returncode != 0
        response = json.loads(result.stdout.splitlines()[-1])
        assert response["status"] == "error_queue_conflict" and "占用" in response["message"]
        assert holder.poll() is None, "foreign owner killed"
        assert (DATA / "state.conf").read_bytes() == before, "failed preflight changed pause policy"
        assert not (DATA / "daemon.pid").exists()
        assert run("iptables", "-t", "mangle", "-S").stdout == routes, "preflight mutated firewall"
        state = json.loads(run(BINARY, "--status-fast").stdout)
        assert state["runtime"] == "error" and state["last_error_code"] == "queue_conflict"
        assert "占用" in state["last_error"] and not state["autostart"]
        # Direct daemon entry must enforce the same protection, not just WebUI.
        assert run(BINARY, "--daemon").returncode != 0
        assert holder.poll() is None and not (DATA / "daemon.pid").exists()
        assert run("iptables", "-t", "mangle", "-S").stdout == routes
        ports = {int(row.split()[1]) for row in queue_file.read_text().splitlines()
                 if int(row.split()[0]) in (0, 1)}
        assert len(ports) == 2, "foreign queue binding lost"
        print("foreign queue preflight, direct daemon rejection, precise error and policy/firewall preservation passed")
    finally:
        if holder:
            holder.communicate("\n", timeout=5)
        if marker:
            run("iptables", "-t", "mangle", "-F", "ADK_NFQ_OUT")
            run("iptables", "-t", "mangle", "-X", "ADK_NFQ_OUT")
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == "__main__":
    main()
