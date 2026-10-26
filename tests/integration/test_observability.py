#!/usr/bin/env python3
"""Real idle daemon snapshots and owner cache maintenance; disposable Linux/root."""
import os
import errno
import json
from pathlib import Path
import shutil
import signal
import socket
import struct
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
            (line.split('=', 1) for line in path.read_text().splitlines()) if key != 'owner_boot'}


def wait(test, description, seconds=15, process=None):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            output, _ = process.communicate(timeout=1)
            detail = output[-4096:].decode('utf-8', errors='replace')
            raise AssertionError(f'{description}: daemon PID {process.pid} exited ({process.returncode}): {detail}')
        if test():
            return
        time.sleep(.05)
    raise AssertionError(description)


def blocked_dns(count):
    host = 'ads.example.org'
    labels = b''.join(bytes([len(part)])+part.encode() for part in host.split('.'))
    for index in range(count):
        request = struct.pack('!6H', index, 0x100, 1, 0, 0, 0)+labels+b'\0\0\1\0\1'
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
            sender.settimeout(.5)
            sender.sendto(request, ('127.0.0.1', 53))
            response = sender.recv(4096)
            assert response[:2] == request[:2] and response[3] & 15 == 3, 'DNS did not receive NXDOMAIN'


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    assert shutil.which('pgrep'), 'requires real pgrep to cover foreground self-discovery'
    assert os.stat('/proc/self/ns/net').st_ino != os.stat(f'/proc/{os.getppid()}/ns/net').st_ino, (
        'requires an isolated network; use tests/integration/run_observability.py')
    DATA.parent.mkdir(parents=True, exist_ok=True)
    daemon = None
    mounted = False
    try:
        subprocess.run([BINARY, '--install'], check=True, capture_output=True)
        (DATA/'custom_rules.txt').write_text('ads.example.org\n')
        daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        wait(lambda: metrics().get('updated_at', 0) > 0, 'initial metric snapshot missing', process=daemon)
        assert metrics()['owner_pid'] == daemon.pid, 'foreground process did not become the metric owner'
        initial = (DATA/'metrics.conf').stat()
        initial_metrics = metrics()
        # No packets/changes: one-second owner sampling must not rewrite this
        # file every second. The first low-frequency heartbeat is 15 s away.
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            current = (DATA/'metrics.conf').stat()
            if (current.st_ino, current.st_mtime_ns) != (initial.st_ino, initial.st_mtime_ns):
                latest = metrics()
                changed = {key: (value, latest.get(key)) for key, value in initial_metrics.items()
                           if latest.get(key) != value}
                raise AssertionError(f'idle metric snapshot rewritten; changed fields: {changed}')
            assert daemon.poll() is None
            time.sleep(.1)
        # A second foreground launcher must still discover the existing owner;
        # excluding its own PID must not bypass the single-daemon protection.
        duplicate = subprocess.run([BINARY, '--daemon', '--foreground'], capture_output=True, timeout=5)
        assert duplicate.returncode == 0 and json.loads(duplicate.stdout.splitlines()[-1])['status'] == 'running', duplicate
        assert int((DATA/'daemon.pid').read_text()) == daemon.pid and daemon.poll() is None
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
        blocked_dns(20)
        wait(lambda: metrics().get('dns_log_written', 0)+metrics().get('dns_log_dropped', 0) == 20,
             'normal DNS logs did not finish')
        normal = metrics()
        assert normal['dns_log_written'] > 0 and normal['dns_log_batches'] < 20
        assert (DATA/'log/dns_query.log').read_text().count('ads.example.org') == normal['dns_log_written']

        # Real ENOSPC, on an owned tmpfs only. Metrics/config stay on the
        # parent filesystem; DNS verdicts must not wait for logging failures.
        subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=64k', 'tmpfs', str(DATA/'log')], check=True, capture_output=True)
        mounted = True
        with (DATA/'log/fill').open('wb', buffering=0) as fill:
            try:
                while True:
                    fill.write(b'x'*4096)
            except OSError as error:
                assert error.errno == errno.ENOSPC
        started = time.monotonic()
        blocked_dns(20)
        assert time.monotonic()-started < 3, 'disk-full DNS logger delayed verdicts'
        wait(lambda: metrics().get('dns_log_written', 0)+metrics().get('dns_log_dropped', 0) == 40,
             'failed log records were not accounted for')
        failed = metrics()
        assert failed['dns_log_write_failures'] > 0 and failed['dns_log_dropped'] > normal['dns_log_dropped']
        assert failed['verdict_failures'] == normal['verdict_failures'] and failed['dns_negative_replies'] >= 40
        subprocess.run(['umount', str(DATA/'log')], check=True, capture_output=True)
        mounted = False
        blocked_dns(20)
        wait(lambda: metrics().get('dns_log_written', 0)+metrics().get('dns_log_dropped', 0) == 60,
             'DNS logging did not recover after storage recovered')
        recovered = metrics()
        assert recovered['dns_log_written'] > normal['dns_log_written'] and recovered['dns_log_pending_bytes'] == 0
        submitted = json.loads(subprocess.run([BINARY, '--dev', 'off'], check=True, capture_output=True).stdout)
        wait(lambda: json.loads(subprocess.run([BINARY, '--apply-status', submitted['revision']],
                                             check=True, capture_output=True).stdout)['status'] == 'applied',
             'log preference was not applied')
        before = metrics()
        blocked_dns(5)
        wait(lambda: metrics().get('dns_negative_replies', 0) >= 65, 'disabled logging affected DNS verdicts')
        after = metrics()
        assert after['dns_log_enqueued'] == before['dns_log_enqueued'] and after['dns_log_dropped'] == before['dns_log_dropped']
        assert after['dns_drop_verdicts'] >= 65 and after['block_exact_hits'] >= 65
        # Freeze only our owned daemon, preserving the exact live identity so
        # fixture edits cannot be immediately replaced by a heartbeat. Restore
        # the file and SIGCONT on every path; no real clock/PID manipulation.
        path = DATA/'metrics.conf'
        saved = path.read_text()
        os.kill(daemon.pid, signal.SIGSTOP)
        try:
            wait(lambda: Path(f'/proc/{daemon.pid}/stat').read_text().rsplit(')', 1)[1].split()[0] == 'T',
                 'test daemon did not stop', seconds=5)
            saved = path.read_text()
            def status():
                return json.loads(subprocess.run([BINARY, '--status-fast'], check=True, capture_output=True).stdout)['metrics']
            current = status()
            assert current['status'] == 'current' and current['owner_verified'] and current['owner_pid'] == daemon.pid
            fields = dict(line.split('=', 1) for line in saved.splitlines())
            def replace(**changes):
                path.write_text(''.join(f'{key}={value}\n' for key, value in {**fields, **changes}.items()))
            replace(owner_started=str(int(fields['owner_started'])+1))
            assert status()['status'] == 'unverified' and not status()['owner_verified']
            replace(owner_boot='ffffffff-ffff-ffff-ffff-ffffffffffff')
            assert not status()['owner_verified']
            replace(observed_monotonic_ms='1')
            stale = status()
            assert stale['status'] == 'stale' and stale['owner_verified'] and stale['age_ms'] > 45000
            replace(dns_packets='-1')
            assert status()['status'] == 'invalid'
            path.unlink()
            path.symlink_to(DATA/'state.conf')
            assert status()['status'] == 'unreadable'
            path.unlink()
            assert status()['status'] == 'missing' and not path.exists(), 'query recreated metrics'
        finally:
            if path.is_symlink():
                path.unlink()
            path.write_text(saved)
            os.kill(daemon.pid, signal.SIGCONT)
        print('real daemon: idle metric suppression/cache expiry, batched DNS logs, real ENOSPC with immediate verdicts, '
              'loss counters, storage recovery, disabled-log semantics and session/age/corrupt record checks passed')
    finally:
        if DATA.exists():
            stopped = subprocess.run([BINARY, '--stop'], capture_output=True, text=True, timeout=25)
            assert stopped.returncode == 0, (stopped.stdout, stopped.stderr)
        if daemon:
            assert daemon.wait(timeout=10) == 0
        if mounted:
            subprocess.run(['umount', str(DATA/'log')], check=True, capture_output=True)
        if (DATA/'metrics.conf').exists():
            final = metrics()
            assert final.get('dns_log_active', 0) == 0 and final.get('dns_log_pending', 0) == 0
            inactive = json.loads(subprocess.run([BINARY, '--status-fast'], check=True, capture_output=True).stdout)['metrics']
            assert inactive['status'] == 'inactive' and not inactive['owner_verified']
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
