#!/usr/bin/env python3
"""Real idle daemon snapshots and owner cache maintenance; disposable Linux/root."""
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
BINARY = sys.argv[1]


def metrics():
    path = DATA/'metrics.conf'
    if not path.exists():
        return {}
    return {key: int(value) for key, value in
            (line.split('=', 1) for line in path.read_text().splitlines())}


def wait(test, description, seconds=15):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        if test():
            return
        time.sleep(.05)
    raise AssertionError(description)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    DATA.parent.mkdir(parents=True, exist_ok=True)
    daemon = None
    try:
        subprocess.run([BINARY, '--install'], check=True, capture_output=True)
        (DATA/'custom_rules.txt').write_text('ads.example.org\n')
        daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        wait(lambda: metrics().get('updated_at', 0) > 0, 'initial metric snapshot missing')
        initial = (DATA/'metrics.conf').stat()
        # No packets/changes: one-second owner sampling must not rewrite this
        # file every second. The first low-frequency heartbeat is 15 s away.
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            current = (DATA/'metrics.conf').stat()
            assert (current.st_ino, current.st_mtime_ns) == (initial.st_ino, initial.st_mtime_ns), 'idle metric snapshot rewritten'
            assert daemon.poll() is None
            time.sleep(.1)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(('127.0.0.1', 80)); listener.listen(1); listener.settimeout(3)
            with socket.create_connection(listener.getsockname(), timeout=3) as client:
                peer, _ = listener.accept()
                with peer:
                    peer.settimeout(3)
                    client.sendall(b'GET / HTTP/1.1\r\nHost: unfinished')
                    assert peer.recv(4096), 'fail-open partial prefix did not reach server'
                    wait(lambda: metrics().get('reassembly_flows', 0) == 1, 'partial HTTP flow not retained')
                    wait(lambda: metrics().get('reassembly_flows', 1) == 0 and metrics().get('reassembly_expired', 0) >= 1,
                         'idle prefix not reclaimed without another packet', seconds=14)
        print('real daemon: unchanged metric inode for five idle seconds, changed data publication and idle HTTP prefix expiry passed')
    finally:
        if DATA.exists():
            stopped = subprocess.run([BINARY, '--stop'], capture_output=True, text=True, timeout=25)
            assert stopped.returncode == 0, (stopped.stdout, stopped.stderr)
        if daemon:
            assert daemon.wait(timeout=10) == 0
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
