#!/usr/bin/env python3
"""Independent conntrack-tools oracle; root in a disposable net namespace only."""
import re
from contextlib import contextmanager
import socket
import subprocess
import sys

PROBE = sys.argv[1]
BLOCK, PASS, MASK = [int(value, 0) for value in
                    subprocess.check_output([PROBE, "--mark-specs"], text=True).split()]


def update(version, sport, dport, mark, mask, success=True):
    result = subprocess.run([PROBE, str(version), str(sport), str(dport), hex(mark), hex(mask)],
                            text=True, capture_output=True)
    assert (result.returncode == 0) == success, result.stderr
    return result


@contextmanager
def tracked_loopback(version, address, port):
    # CTNETLINK availability does not activate conntrack hooks. Ubuntu kernels
    # with nf_conntrack.enable_hooks=0 otherwise leave this socket untracked
    # after the preceding engine test removes its last connmark rule.
    table = "iptables" if version == 4 else "ip6tables"
    rule = ["OUTPUT", "-p", "tcp", "-s", address, "-d", address,
            "--dport", str(port), "-m", "connmark", "--mark", "0/0"]
    subprocess.run([table, "-w", "5", "-t", "mangle", "-A", *rule], check=True)
    try:
        yield
    finally:
        subprocess.run([table, "-w", "5", "-t", "mangle", "-D", *rule], check=True)


for family, version, address in ((socket.AF_INET, 4, "127.0.0.1"),
                                 (socket.AF_INET6, 6, "::1")):
    with socket.socket(family, socket.SOCK_STREAM) as listener:
        listener.bind((address, 0))
        listener.listen(1)
        dport = listener.getsockname()[1]
        with tracked_loopback(version, address, dport), \
                socket.create_connection((address, dport), timeout=2) as client:
            peer, _ = listener.accept()
            with peer:
                client.sendall(b"ready")
                assert peer.recv(5) == b"ready"
                sport = client.getsockname()[1]
                query = ["conntrack", "-G", "-p", "tcp",
                         "--orig-src", address, "--orig-dst", address,
                         "--sport", str(sport), "--dport", str(dport)]

                def current():
                    result = subprocess.run(query, text=True, capture_output=True, check=True)
                    found = re.search(r"\bmark=(\d+)", result.stdout)
                    assert found, result.stdout
                    return int(found[1])

                update(version, sport, dport, 0x030ABCDE, 0xFFFFFFFF)
                assert current() == 0x030ABCDE
                for mark in (BLOCK, PASS, 0):
                    update(version, sport, dport, mark, MASK)
                    assert current() == 0x030ABCDE | mark, "non-module mark bits changed"
                before = current()
                update(version, sport, dport, BLOCK | 1, MASK, False)
                update(version, sport, dport, 1, 0, False)
                assert current() == before, "invalid mask/value mutated connection"
    missing = update(version, 0, 0, BLOCK, MASK, False)
    assert "errno=2 " in missing.stderr, missing.stderr

print("independent CTNETLINK IPv4/IPv6 ACK, masked PASS/BLOCK preservation, invalid input and no-create tests passed")
