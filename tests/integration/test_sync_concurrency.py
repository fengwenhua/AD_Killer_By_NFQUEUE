#!/usr/bin/env python3
"""Real CLI, concurrent HTTP requests, deterministic outputs and atomic failures.

Disposable Linux/root only. Never run against an installed Android module.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import threading
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')


class Server(ThreadingHTTPServer):
    request_queue_size = 64


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        server = self.server
        with server.lock:
            server.active += 1
            server.maximum = max(server.maximum, server.active)
            server.requests.append(self.path)
            if server.active >= server.required:
                server.admitted.set()
        try:
            assert server.admitted.wait(4), 'configured concurrency never admitted its first wave'
            if self.path.startswith('/bad'):
                self.send_response(404)
                self.end_headers()
                return
            time.sleep(.08)
            if self.headers.get('If-None-Match') == '"v1"':
                self.send_response(304)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header('ETag', '"v1"')
            self.end_headers()
            self.wfile.write(('0.0.0.0 ad' + self.path.rsplit('/', 1)[-1] + '.example.org\n').encode())
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with server.lock:
                server.active -= 1


def main():
    binary = sys.argv[1]
    transfer_limit = int(os.environ.get('ADK_TEST_DOWNLOAD_SLOTS', '20'))
    assert os.geteuid() == 0 and not DATA.exists(), 'disposable Linux/root only'
    server = Server(('127.0.0.1', 0), Handler)
    server.lock = threading.Lock()
    server.active = server.maximum = 0
    server.requests = []
    server.required = 1
    server.admitted = threading.Event()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    prefix = f'http://127.0.0.1:{server.server_port}'

    def command(*args, text=None, success=True):
        result = subprocess.run([binary, *args], input=text, text=True, capture_output=True, timeout=20)
        assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
        return result

    def configure(n, paths):
        deadline = time.monotonic() + 5
        while server.active and time.monotonic() < deadline:
            time.sleep(.01)
        assert not server.active
        server.maximum = 0
        server.requests.clear()
        server.required = min(n, transfer_limit, len(paths))
        server.admitted.clear()
        command('--sources-save', text=''.join(prefix + path + '\n' for path in paths) + '# disabled: builtin:hagezi-pro-minus-fake\n')

    def products():
        manifest = (DATA / 'rules.current').read_bytes()
        directory = DATA / 'rulesets' / manifest.decode().strip()
        return manifest, {name: (directory / name).read_bytes() for name in ['hosts_rules.gz', 'adguard_rules.gz']}

    try:
        command('--install')
        assert json.loads(command('--sync-settings').stdout)['concurrency'] == 10
        conf = DATA / 'state.conf'
        original = conf.read_text()
        legacy = ''.join(line for line in original.splitlines(keepends=True) if not line.startswith('sync_concurrency='))
        for suffix in ['', 'sync_concurrency=0\n', 'sync_concurrency=21\n', 'sync_concurrency=invalid\n']:
            conf.write_text(legacy + suffix)
            before = conf.read_bytes()
            assert json.loads(command('--sync-settings').stdout)['concurrency'] == 10
            assert conf.read_bytes() == before, 'reading defaults rewrote old/invalid config'
        conf.write_text(original)
        for invalid in ['0', '21', '-1', '1.1', '10x', '99999999999999999999999']:
            before = (DATA / 'state.conf').read_bytes()
            command('--sync-concurrency', invalid, success=False)
            assert (DATA / 'state.conf').read_bytes() == before
        assert not (DATA / 'daemon.pid').exists()
        paths = [f'/rules/{i}' for i in range(20)]
        baseline = None
        timings = {}
        for count in [10, 20, 2, 1]:
            command('--sync-concurrency', str(count))
            assert json.loads(command('--sync-settings').stdout)['concurrency'] == count
            saved = conf.read_bytes()
            command('--install')
            assert conf.read_bytes() == saved, 'reinstall reset the saved concurrency'
            configure(count, paths)
            started = time.monotonic()
            command('--update')
            timings[count] = round(time.monotonic() - started, 3)
            assert server.maximum == min(count, transfer_limit), (count, transfer_limit, server.maximum)
            assert len(server.requests) == 20
            _, files = products()
            if baseline is None:
                baseline = files
            else:
                assert files == baseline, 'concurrency changed canonical/gzip output'
            report = (DATA / 'source_stats.conf').read_text()
            assert 'source_count=20\n' in report and 'accepted_sources=20\n' in report
            sections = report.split('[source_')[1:]
            assert all(f'url={prefix}{path}\n' in row for path, row in zip(paths, sections)), 'report order changed with completion order'
            assert not (DATA / 'source_staging').exists(), 'download files/leases leaked'
            assert not (DATA / 'daemon.pid').exists(), 'changing concurrency or sync started a daemon'
        before = products()
        command('--sync-concurrency', '10')
        configure(10, ['/bad/0'] + [f'/failure/{i}' for i in range(19)])
        command('--update', success=False)
        assert set(server.requests) <= {'/bad/0', *[f'/failure/{i}' for i in range(9)]}, 'queued sources started after the first failed wave'
        if transfer_limit == 20:
            assert len(server.requests) == 10
        assert products() == before, 'failed concurrent source replaced a complete generation'
        assert not (DATA / 'source_staging').exists()
        assert 'publish_status=incomplete_sources\n' in (DATA / 'source_stats.conf').read_text()
        # A new task owns new deadlines/credits, not the cancelled task's ledger.
        configure(10, paths)
        command('--update')
        assert products()[1] == baseline
        print('local 20-source fixture wall seconds (not phone/network performance):', timings)
        print(f'real configured concurrency 1/2/default10/max20 (backend transfer cap {transfer_limit}), durable settings, 304/gzip parity, stable reports, failure rollback, staging cleanup and next-task recovery passed')
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
