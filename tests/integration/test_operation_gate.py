#!/usr/bin/env python3
"""Accepted-writer drain and closed admission; disposable Linux/root only."""
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
MODULE = Path('/data/adb/modules/AD_Killer_By_NFQUEUE')
GATE = DATA.parent/'.AD_Killer_By_NFQUEUE.operations.lock'
BINARY = sys.argv[1]
STARTED = threading.Event()
RELEASE = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        STARTED.set(); RELEASE.wait(10)
        self.send_response(200); self.end_headers()
        self.wfile.write(b'0.0.0.0 ads.example.org\n')


def run(*args, success=True, input=None):
    result = subprocess.run([BINARY, *args], text=True, capture_output=True, timeout=5, input=input)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


def main():
    assert sys.platform == 'linux' and os.geteuid() == 0
    assert not DATA.exists() and not MODULE.exists(), 'refusing existing module fixtures'
    DATA.parent.mkdir(parents=True, exist_ok=True)
    MODULE.mkdir(parents=True)
    marker = MODULE/'.uninstalling'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        # Read-only lookups cannot recreate a data directory after removal.
        run('--sources-list', success=False)
        run('--jobs-list'); run('--update-status', '1-2', success=False)
        empty_apply = json.loads(run('--apply-status').stdout)
        assert empty_apply['status'] == 'deferred' and empty_apply['verified'] is False
        run('--status'); run('--status-fast')
        assert not DATA.exists()
        run('--install')
        expected = {name: (DATA/name).read_bytes() for name in ('state.conf', 'custom_rules.txt', 'app_whitelist.txt', 'update_sources.txt')}
        started = json.loads(run('--source-test', f'http://127.0.0.1:{server.server_port}/slow').stdout)
        id = started['job_id']; assert STARTED.wait(5)
        with GATE.open('r+') as lease:
            try:
                fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                pass
            else:
                raise AssertionError('background worker dropped its accepted-writer lease')
        marker.write_text('owned isolation test marker\n')
        for args, data in ((('--install',), None), (('--start',), None), (('--stop',), None),
                           (('--mode', '1'), None), (('--scope', '1'), None), (('--quic', 'off'), None),
                           (('--autostart', 'on'), None), (('--reload',), None), (('--apps-apply',), None),
                           (('--update-start',), None), (('--update',), None), (('--sources-save',), 'https://example.org/a\n'),
                           (('--config-save', 'custom', 'a'*64), 'ads.example.org\n')):
            reply = json.loads(run(*args, success=False, input=data).stdout)
            assert reply['status'] == 'error_maintenance', (args, reply)
        assert {name: (DATA/name).read_bytes() for name in expected} == expected
        assert json.loads(run('--jobs-list').stdout)['jobs'][0]['job_id'] == id
        RELEASE.set()
        for _ in range(100):
            state = json.loads(run('--update-status', id).stdout)
            if state['status'] in ('success', 'failed', 'interrupted'):
                break
            time.sleep(.05)
        assert state['status'] in ('success', 'failed', 'interrupted'), state
        with GATE.open('r+') as lease:
            fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
            GATE.unlink()  # Test-only exact gate, exclusively held and closed to admission.
            run('--mode', '1', success=False)
            assert not GATE.exists(), 'late entry recreated the removed operation lock'
        marker.unlink()
        run('--install')
        assert (DATA/'state.conf').read_bytes() == expected['state.conf']
        print('real operation admission: background lease, marker rejects writes, read-only missing-data queries and no late recreation passed')
    finally:
        RELEASE.set(); server.shutdown(); server.server_close()
        marker.unlink(missing_ok=True)
        if GATE.exists():
            for attempt in range(100):
                try:
                    with GATE.open('r+') as lease:
                        fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if attempt == 99:
                        raise
                    time.sleep(.05)
        if DATA.exists():
            shutil.rmtree(DATA)
        shutil.rmtree(MODULE)


if __name__ == '__main__':
    main()
