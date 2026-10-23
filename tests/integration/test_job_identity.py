#!/usr/bin/env python3
"""Real CLI worker identities/recovery/dedup. Disposable Linux/root only."""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
BINARY = Path(sys.argv[1]).resolve()
STARTED = threading.Event()
RELEASE = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/slow':
            STARTED.set()
            RELEASE.wait(15)
        self.send_response(200)
        self.end_headers()
        try:
            self.wfile.write(b'0.0.0.0 ads.example.org\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


def command(*args, success=True):
    result = subprocess.run([str(BINARY), *args], text=True, capture_output=True, timeout=10)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    if args == ('--install',):
        return {}  # Installation emits diagnostic text, not a JSON response.
    return json.loads(result.stdout.splitlines()[-1])


def wait(id, status):
    for _ in range(100):
        result = subprocess.run([str(BINARY), '--update-status', id], text=True, capture_output=True, timeout=5)
        state = json.loads(result.stdout)
        if state['status'] == status:
            return state
        time.sleep(.05)
    raise AssertionError((id, status, state))


def main():
    assert sys.platform == 'linux' and os.geteuid() == 0 and not DATA.exists()
    DATA.parent.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    slow = f'http://127.0.0.1:{server.server_port}/slow'
    fast = f'http://127.0.0.1:{server.server_port}/fast'
    owned_pid = None
    try:
        command('--install')
        assert command('--jobs-list') == {'status': 'ok', 'jobs': []}
        with ThreadPoolExecutor(max_workers=8) as pool:
            replies = list(pool.map(lambda _: command('--source-test', slow), range(8)))
        ids = {reply['job_id'] for reply in replies}
        assert len(ids) == 1 and all(reply['status'] == 'started' for reply in replies)
        id = ids.pop()
        assert STARTED.wait(5)
        assert len(list((DATA/'update_jobs').glob('*.state'))) == 1
        assert command('--update-start', success=False)['status'] == 'busy'
        assert command('--source-test', fast, success=False)['job_id'] == id
        active = command('--jobs-list')['jobs']
        assert len(active) == 1 and active[0]['job_id'] == id
        assert active[0]['type'] == 'source' and active[0]['target'] == slow
        assert active[0]['phase'] == 'downloading' and active[0]['lease_active'] is True
        state_path = DATA/'update_jobs'/f'update-{id}.state'
        journal = state_path.read_text()
        assert journal.startswith('ADK_JOB_2\n') and 'boot=' in journal and 'started=' in journal
        assert command('--update-status', id)['status'] == 'running'
        RELEASE.set()
        complete = wait(id, 'success')
        assert complete['phase'] == 'finished' and complete['reason'] == 'completed'
        assert complete['updated_at'] >= complete['started_at'] > 0

        # Synthetic PID reuse/reboot/renamed job records never signal a process
        # or become running merely because that PID currently exists.
        fields = dict(line.split('=', 1) for line in journal.splitlines()[1:])
        fake = '123-456'
        legacy = DATA/'update_jobs'/f'update-{fake}.state'
        legacy.write_text(f'running:{os.getpid()}\n')
        assert command('--update-status', fake)['status'] == 'interrupted'
        for changes in ({'boot': 'ffffffff-ffff-ffff-ffff-ffffffffffff'},
                        {'started': str(int(fields['started'])+1)}, {'pid': str(os.getpid())}):
            copy = {**fields, 'id': fake, **changes}
            legacy.write_text('ADK_JOB_2\n'+''.join(f'{k}={v}\n' for k, v in copy.items()))
            (DATA/'update_jobs'/f'update-{fake}.lease').touch()
            before = legacy.read_bytes()
            assert command('--update-status', fake)['status'] == 'interrupted'
            assert legacy.read_bytes() == before
            os.kill(os.getpid(), 0)
        legacy.write_text(journal)
        assert command('--update-status', fake, success=False)['status'] == 'invalid_state'

        # An owned real worker dies mid-fetch. Recovery discovers an interrupted
        # record, not an unrelated PID, and a new explicit request can proceed.
        STARTED.clear(); RELEASE.clear()
        crashed = command('--source-test', slow)['job_id']
        assert STARTED.wait(5)
        crashed_path = DATA/'update_jobs'/f'update-{crashed}.state'
        fields = dict(line.split('=', 1) for line in crashed_path.read_text().splitlines()[1:])
        owned_pid = int(fields['pid'])
        assert Path(f'/proc/{owned_pid}/exe').resolve() == BINARY
        os.kill(owned_pid, signal.SIGKILL)
        wait(crashed, 'interrupted'); owned_pid = None; RELEASE.set()
        recovered = command('--jobs-list')['jobs'][0]
        assert recovered['job_id'] == crashed and recovered['status'] == 'interrupted'
        resumed = command('--source-test', fast)['job_id']
        assert resumed != crashed
        wait(resumed, 'success')
        index = DATA/'update_jobs/.active-rules'
        saved_index = index.read_bytes(); index.write_text('../bad\n')
        try:
            assert command('--jobs-list', success=False)['status'] == 'unverified'
            assert command('--source-test', fast, success=False)['status'] == 'unverified'
        finally:
            index.write_bytes(saved_index)
        print('real task identities: concurrent dedup, backend discovery, durable stages, PID/boot/image rejection, owner crash and corrupt index passed')
    finally:
        RELEASE.set()
        if owned_pid is not None:
            try:
                os.kill(owned_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        server.shutdown(); server.server_close()
        # Only delete this isolated fixture after all task leases are released.
        if DATA.exists():
            import fcntl
            for path in (DATA/'update_jobs').glob('*.lease'):
                for attempt in range(100):
                    try:
                        with path.open() as file:
                            fcntl.flock(file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        break
                    except BlockingIOError:
                        if attempt == 99:
                            raise
                        time.sleep(.05)
            shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
