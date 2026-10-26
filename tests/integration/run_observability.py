#!/usr/bin/env python3
"""Run idle metrics with real ambient DNS traffic in the outer test network."""
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time


def main():
    assert os.geteuid() == 0, 'requires disposable Linux/root'
    for tool in ('unshare', 'ip', 'pgrep'):
        assert shutil.which(tool), f'missing test dependency: {tool}'
    assert not Path('/data/adb/AD_Killer_By_NFQUEUE').exists(), 'requires clean test data'
    queues = Path('/proc/net/netfilter/nfnetlink_queue')
    assert not queues.exists() or not {int(row.split()[0]) for row in queues.read_text().splitlines()} & {0, 1}
    test = Path(__file__).with_name('test_observability.py')
    command = ['unshare', '--net', 'sh', '-eu', '-c',
               'ip link set dev lo up\nexec "$@"', 'adk-observability',
               sys.executable, str(test), sys.argv[1]]
    process = subprocess.Popen(command, start_new_session=True)
    labels = b''.join(bytes([len(part)])+part.encode() for part in 'safe.observability.example'.split('.'))
    request = struct.pack('!6H', 1, 0x100, 1, 0, 0, 0)+labels+b'\0\0\1\0\1'
    sent = 0
    deadline = time.monotonic()+180
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            while process.poll() is None:
                assert time.monotonic() < deadline, 'observability test exceeded total budget'
                sender.sendto(request, ('127.0.0.1', 53))
                sent += 1
                time.sleep(.1)
        assert process.wait() == 0, 'observability child failed under ambient traffic'
        assert sent > 0
        print(f'observability regression passed with {sent} outer-network DNS requests')
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)


if __name__ == '__main__':
    main()
