#!/usr/bin/env python3
"""Run only as root inside an explicitly disposable Linux network namespace."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


DATA = Path("/data/adb/AD_Killer_By_NFQUEUE")
BINARY = sys.argv[1]
TABLES = ("iptables", "ip6tables")


def command(*args: str) -> str:
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode != 0:
        raise AssertionError(f"command failed ({result.returncode}): {args!r}\n{result.stdout}")
    return result.stdout


def firewall(table: str, *args: str) -> str:
    return command(table, "-w", "5", "-t", "mangle", *args)


def wait_for(test, description: str, timeout: float = 10) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if test():
            return
        time.sleep(0.05)
    raise AssertionError("timeout: " + description)


def state() -> str:
    path = DATA / "runtime.state"
    return path.read_text().strip() if path.exists() else ""


def dns_query(host: str) -> bytes:
    labels = b"".join(bytes([len(label)]) + label.encode() for label in host.split("."))
    return struct.pack("!6H", 1, 0x100, 1, 0, 0, 0) + labels + b"\0\0\1\0\1"


def tls_hello(host: str) -> bytes:
    name = host.encode()
    sni = struct.pack("!H", len(name) + 3) + b"\0" + struct.pack("!H", len(name)) + name
    extension = struct.pack("!HH", 0, len(sni)) + sni
    body = b"\3\3" + bytes(32) + b"\0\0\2\x13\1\1\0" + struct.pack("!H", len(extension)) + extension
    handshake = b"\1" + len(body).to_bytes(3, "big") + body
    return b"\x16\3\3" + struct.pack("!H", len(handshake)) + handshake


def udp_dns(family: int, host: str, allowed: bool) -> None:
    address = "127.0.0.1" if family == socket.AF_INET else "::1"
    payload = dns_query(host)
    with socket.socket(family, socket.SOCK_DGRAM) as receiver, \
            socket.socket(family, socket.SOCK_DGRAM) as sender:
        receiver.bind((address, 53))
        receiver.settimeout(0.5)
        sender.settimeout(0.5)
        try:
            sender.sendto(payload, (address, 53))
        except PermissionError:
            raise AssertionError("DNS was silently dropped instead of returning a response")
        if not allowed:
            start = time.monotonic()
            response, remote = sender.recvfrom(4096)
            assert response[:2] == payload[:2] and response[2] & 0x80
            assert response[3] & 15 == 3, "blocked DNS must receive NXDOMAIN"
            assert remote[1] == 53 and response[12:] == payload[12:]
            assert time.monotonic() - start < 0.5, "DNS rejection waited for a retry timeout"
        try:
            actual = receiver.recv(4096)
        except socket.timeout:
            if allowed:
                raise AssertionError("allowed UDP DNS query did not arrive")
        else:
            assert allowed and actual == payload, "blocked DNS query arrived or payload changed"


def tcp_inspection(family: int, port: int, host: str, allowed: bool) -> None:
    address = "127.0.0.1" if family == socket.AF_INET else "::1"
    payload = (b"GET / HTTP/1.1\r\nHost: " + host.encode() + b"\r\n\r\n") \
        if port == 80 else tls_hello(host) if port == 443 else \
        struct.pack("!H", len(dns_query(host))) + dns_query(host)
    with socket.socket(family, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((address, port))
        listener.listen(1)
        listener.settimeout(2)
        with socket.create_connection((address, port), timeout=2) as client:
            peer, _ = listener.accept()
            with peer:
                peer.settimeout(0.5)
                reset = False
                try:
                    client.sendall(payload)
                except ConnectionResetError:
                    assert not allowed
                    reset = True
                if not allowed:
                    client.settimeout(0.5)
                    if not reset:
                        try:
                            client.recv(1)
                        except ConnectionResetError:
                            reset = True
                    assert reset, "blocked TCP did not promptly reset the client"
                    try:
                        actual = peer.recv(4096)
                    except socket.timeout:
                        actual = b""
                    assert not actual, "blocked TCP payload reached the server"
                    return
                try:
                    actual = b""
                    while len(actual) < len(payload):
                        chunk = peer.recv(len(payload) - len(actual))
                        if not chunk:
                            break
                        actual += chunk
                except socket.timeout:
                    assert not allowed, "allowed TCP payload did not arrive"
                else:
                    assert allowed and actual == payload, "blocked TCP payload arrived or changed"


def quic_rejection(family):
    address = "127.0.0.1" if family == socket.AF_INET else "::1"
    with socket.socket(family, socket.SOCK_DGRAM) as receiver, \
            socket.socket(family, socket.SOCK_DGRAM) as sender:
        receiver.bind((address, 443))
        receiver.settimeout(0.2)
        sender.connect((address, 443))
        sender.settimeout(0.5)
        began = time.monotonic()
        try:
            sender.send(b"ad-killer-by-nfqueue-quic-probe")
            sender.recv(1)
        except ConnectionRefusedError:
            pass
        else:
            raise AssertionError("QUIC did not receive an immediate port-unreachable error")
        assert time.monotonic() - began < 0.5
        try:
            receiver.recv(1024)
        except socket.timeout:
            pass
        else:
            raise AssertionError("rejected QUIC datagram reached its receiver")


def main() -> None:
    assert os.geteuid() == 0, "requires root in a disposable environment"
    assert not DATA.exists(), "refusing to touch an existing module data directory"
    for table in TABLES:
        assert "ADK_NFQ_" not in firewall(table, "-S"), "existing module firewall state"
    queue_file = Path("/proc/net/netfilter/nfnetlink_queue")
    if queue_file.exists():
        queues = {int(line.split()[0]) for line in queue_file.read_text().splitlines()}
        assert not queues.intersection({0, 1}), "production queues already in use"

    daemon = None
    canary = False
    conflict = False
    foreign = ("-p", "udp", "--dport", "49557", "-j", "CONNMARK",
               "--set-xmark", "0x4000000/0x4000000")
    anonymous_drop = ("-p", "udp", "--dport", "443", "-j", "DROP")
    try:
        # Android root managers supply /data/adb; a fresh Linux container does not.
        DATA.parent.mkdir(parents=True, exist_ok=True)
        command(BINARY, "--install")
        status = json.loads(command(BINARY, "--status-fast").splitlines()[-1])
        assert not status["autostart"] and not status["enabled"]
        assert status["block_quic"], "fresh installation must enable QUIC rejection"
        command(BINARY, "--autostart", "on")
        assert not (DATA / "daemon.pid").exists(), "boot setting started an engine"
        command(BINARY, "--autostart", "off")
        before = (DATA / 'state.conf').read_bytes()
        empty = subprocess.run([BINARY, '--start'], capture_output=True, text=True, timeout=10)
        assert empty.returncode != 0 and json.loads(empty.stdout)['status'] == 'error_rules_empty'
        assert '同步开源规则' in json.loads(empty.stdout)['message']
        assert (DATA / 'state.conf').read_bytes() == before, 'empty startup changed boot/enabled policy'
        assert not (DATA / 'daemon.pid').exists()
        direct = subprocess.run([BINARY, '--daemon'], capture_output=True, text=True, timeout=10)
        assert direct.returncode != 0
        assert 'rules_empty' in (DATA / 'engine_error.conf').read_text()
        for table in TABLES:
            assert 'ADK_NFQ_' not in firewall(table, '-S'), 'empty startup attached traffic'
        (DATA / "custom_rules.txt").write_text("ads.example.com\n")
        firewall("iptables", "-A", "OUTPUT", *anonymous_drop)
        canary = True
        daemon = subprocess.Popen([BINARY, "--daemon", "--foreground"])
        wait_for(lambda: state() == "running" or daemon.poll() is not None,
                 "daemon startup")
        assert daemon.poll() is None and state() == "running", "daemon failed startup"
        assert json.loads(command(BINARY, "--status").splitlines()[-1])["iptables_active"]
        fast=json.loads(command(BINARY, '--status-fast').splitlines()[-1])
        assert fast['iptables_active'] and fast['daemon_image_current'] and fast['route_check']=='entry'
        command(BINARY, "--autostart", "on")
        command(BINARY, "--autostart", "off")
        assert daemon.poll() is None and state() == "running", "boot setting stopped engine"

        for family in (socket.AF_INET, socket.AF_INET6):
            udp_dns(family, "safe.example.org", True)
            udp_dns(family, "ads.example.com", False)
            for port in (53, 80, 443):
                tcp_inspection(family, port, "safe.example.org", True)
                tcp_inspection(family, port, "ads.example.com", False)
            # The installation default must attach working IPv4/IPv6 rejection
            # routes without requiring an explicit --quic on first.
            quic_rejection(family)

        command(BINARY, "--quic", "on")
        wait_for(lambda: json.loads(command(BINARY, "--status").splitlines()[-1])["iptables_active"],
                 "QUIC routing enabled")
        for family in (socket.AF_INET, socket.AF_INET6):
            quic_rejection(family)
        command(BINARY, "--quic", "off")
        wait_for(lambda: json.loads(command(BINARY, "--status").splitlines()[-1])["iptables_active"],
                 "QUIC routing disabled")

        command(BINARY, "--scope", "1")
        wait_for(lambda: json.loads(command(BINARY, "--status").splitlines()[-1])
                 ["iptables_active"], "per-app selector generation")
        # Empty per-app selector must bypass even a matching domain.
        udp_dns(socket.AF_INET, "ads.example.com", True)
        command(BINARY, "--scope", "0")
        wait_for(lambda: json.loads(command(BINARY, "--status").splitlines()[-1])
                 ["iptables_active"], "global generation restored")

        # KernelSU's pre-soft-reboot lifecycle hook must release this daemon
        # without treating reboot as a persistent user pause or opting in.
        before_policy=(DATA/'state.conf').read_bytes()
        command(BINARY, '--prepare-reboot')
        assert daemon.wait(timeout=10)==0 and state()=='stopped'
        assert (DATA/'state.conf').read_bytes()==before_policy
        for table in TABLES:
            assert 'ADK_NFQ_' not in firewall(table,'-S')
        assert not json.loads(command(BINARY,'--status-fast').splitlines()[-1])['running']
        daemon=subprocess.Popen([BINARY,'--daemon','--foreground'])
        wait_for(lambda:state()=='running' or daemon.poll() is not None,'startup after lifecycle stop')
        assert daemon.poll() is None
        command(BINARY, "--stop")
        assert daemon.wait(timeout=10) == 0
        assert state() == "stopped"
        for table in TABLES:
            assert "ADK_NFQ_" not in firewall(table, "-S"), "module chains survived stop"
        firewall("iptables", "-C", "OUTPUT", *anonymous_drop)

        # A SIGKILL cannot execute cleanup. New unclassified traffic must still
        # bypass absent listeners, and a restart must replace the stale chains.
        daemon = subprocess.Popen([BINARY, "--daemon", "--foreground"])
        wait_for(lambda: state() == "running" or daemon.poll() is not None,
                 "second daemon startup")
        assert daemon.poll() is None and state() == "running"
        daemon.kill()
        daemon.wait(timeout=10)
        # A diagnostic read may report a dead listener, but never clean up the
        # firewall snapshot as a side effect (also needed for forensic tools).
        before_routes={table:firewall(table,'-S') for table in TABLES}
        assert not json.loads(command(BINARY,'--status').splitlines()[-1])['running']
        assert not json.loads(command(BINARY,'--status-fast').splitlines()[-1])['iptables_active']
        assert before_routes=={table:firewall(table,'-S') for table in TABLES}
        for family in (socket.AF_INET, socket.AF_INET6):
            udp_dns(family, "safe.example.org", True)
            tcp_inspection(family, 80, "safe.example.org", True)
        daemon = subprocess.Popen([BINARY, "--daemon", "--foreground"])
        wait_for(lambda: (state() == "running" and
                         (DATA / "daemon.pid").read_text().strip() == str(daemon.pid))
                 or daemon.poll() is not None,
                 "restart after forced termination")
        assert daemon.poll() is None and state() == "running"
        wait_for(lambda: json.loads(command(BINARY, "--status").splitlines()[-1])
                 ["iptables_active"], "restarted daemon and route identity")
        udp_dns(socket.AF_INET, "ads.example.com", False)
        daemon.send_signal(signal.SIGTERM)
        assert daemon.wait(timeout=10) == 0 and state() == "stopped"
        for table in TABLES:
            assert "ADK_NFQ_" not in firewall(table, "-S")

        # Reproduce an upgrade leaving a deleted old image alive while Android
        # (or another firewall) removes our OUTPUT attachment. Explicit --start
        # must gracefully replace only our daemon, revalidate both stacks and
        # preserve foreign policy. A healthy repeated --start is a no-op.
        with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_old_image_") as directory:
            old_image = Path(directory) / "AD_Killer_By_NFQUEUE"
            shutil.copy2(BINARY, old_image)
            daemon = subprocess.Popen([str(old_image), "--daemon", "--foreground"])
            wait_for(lambda: (state() == "running" and
                             (DATA / "daemon.pid").read_text().strip() == str(daemon.pid))
                     or daemon.poll() is not None, "old image startup")
            assert daemon.poll() is None
            old_image.unlink()
            assert "(deleted)" in os.readlink(f"/proc/{daemon.pid}/exe")
            try:
                # The image changed even though the old listener and routes
                # remain healthy: explicit start must switch to the new image.
                assert not json.loads(command(BINARY,'--status-fast').splitlines()[-1])['daemon_image_current']
                old_pid=daemon.pid
                command(BINARY,'--start')
                wait_for(lambda:state()=='running' and int((DATA/'daemon.pid').read_text())!=old_pid,
                         'image-only upgrade handover',timeout=15)
                assert daemon.wait(timeout=10)==0
                for affected in (("iptables",), ("ip6tables",), TABLES):
                    previous_pid = int((DATA / "daemon.pid").read_text())
                    for table in affected:
                        firewall(table, "-D", "OUTPUT", "-j", "ADK_NFQ_ENTRY")
                    broken = json.loads(command(BINARY, "--status").splitlines()[-1])
                    assert broken["running"] and not broken["iptables_active"]
                    quick=json.loads(command(BINARY,'--status-fast').splitlines()[-1])
                    assert quick['running'] and not quick['iptables_active']
                    reply = json.loads(command(BINARY, "--start").splitlines()[-1])
                    assert reply["status"] == "starting", reply
                    wait_for(lambda: state() == "running" and
                             int((DATA / "daemon.pid").read_text()) != previous_pid and
                             json.loads(command(BINARY, "--status").splitlines()[-1])["iptables_active"],
                             "manual start recovered missing dual-stack routes", timeout=15)
                    current_pid = (DATA / "daemon.pid").read_text().strip()
                    assert Path(f"/proc/{current_pid}/exe").resolve() == Path(BINARY).resolve()
                    status = json.loads(command(BINARY, "--status").splitlines()[-1])
                    assert status["enabled"] and not status["manual_stop"] and not status["autostart"]
                    healthy = json.loads(command(BINARY, "--start").splitlines()[-1])
                    assert healthy["status"] == "running"
                    assert (DATA / "daemon.pid").read_text().strip() == current_pid
                    for family in (socket.AF_INET, socket.AF_INET6):
                        udp_dns(family, "safe.example.org", True)
                        udp_dns(family, "ads.example.com", False)
                    firewall("iptables", "-C", "OUTPUT", *anonymous_drop)
                # A nonresponsive owned daemon must not be force-killed or
                # have a second instance attached over its existing queues.
                current_pid = int((DATA / "daemon.pid").read_text())
                os.kill(current_pid, signal.SIGSTOP)
                try:
                    firewall("iptables", "-D", "OUTPUT", "-j", "ADK_NFQ_ENTRY")
                    before = firewall("iptables", "-S")
                    failed = subprocess.run([BINARY, "--start"], text=True, capture_output=True, timeout=15)
                    assert failed.returncode != 0
                    assert json.loads(failed.stdout.splitlines()[-1])["status"] == "error_recovery_stop_timeout"
                    assert int((DATA / "daemon.pid").read_text()) == current_pid
                    assert Path(f"/proc/{current_pid}/exe").exists(), "recovery killed an unresponsive daemon"
                    assert firewall("iptables", "-S") == before, "timed-out recovery modified routes"
                    assert len(queue_file.read_text().splitlines()) == 2
                    policy=(DATA/'state.conf').read_bytes()
                    lifecycle=subprocess.run([BINARY,'--prepare-reboot'],text=True,capture_output=True,timeout=15)
                    assert lifecycle.returncode!=0
                    assert int((DATA/'daemon.pid').read_text())==current_pid
                    assert Path(f'/proc/{current_pid}/exe').exists(), 'lifecycle cleanup killed an unresponsive instance'
                    assert (DATA/'state.conf').read_bytes()==policy
                    assert firewall('iptables','-S')==before
                finally:
                    os.kill(current_pid, signal.SIGCONT)
                    wait_for(lambda: not Path(f"/proc/{current_pid}/exe").exists(),
                             "pending graceful stop completed")
            finally:
                command(BINARY, "--stop")
                assert daemon.wait(timeout=10) == 0

        firewall("iptables", "-A", "OUTPUT", *foreign)
        conflict = True
        daemon = subprocess.Popen([BINARY, "--daemon", "--foreground"])
        assert daemon.wait(timeout=15) != 0, "external connmark conflict was not rejected"
        assert state() == "error"
        for table in TABLES:
            assert "ADK_NFQ_" not in firewall(table, "-S"), "conflict mounted active routes"
        firewall("iptables", "-C", "OUTPUT", *anonymous_drop)
        print("real engine dual-stack fast DNS NXDOMAIN/TCP reset/QUIC rejection, HTTP/TLS pass, scope reload, CLI stop, "
              "SIGKILL bypass/restart, missing-route/deleted-image recovery, healthy start idempotence, "
              "connmark conflict and foreign-rule preservation tests passed")
    except BaseException:
        log = DATA / "log/engine.log"
        if log.exists():
            print(log.read_text()[-10000:], file=sys.stderr)
        raise
    finally:
        if daemon is not None and daemon.poll() is None:
            daemon.terminate()
            try:
                daemon.wait(timeout=10)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
        if conflict:
            firewall("iptables", "-D", "OUTPUT", *foreign)
        if canary:
            firewall("iptables", "-D", "OUTPUT", *anonymous_drop)
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == "__main__":
    main()
