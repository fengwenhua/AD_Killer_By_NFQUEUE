#!/usr/bin/env python3
"""Real parent-enforced budgets across slow sources and unavailable libraries."""
import ctypes.util
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        try:
            if self.path.startswith('/slow'):
                time.sleep(.55)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'x' * (60 if self.path.startswith('/body') else 16))
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    curl, library = shutil.which('curl'), ctypes.util.find_library('curl')
    assert curl and library
    with tempfile.TemporaryDirectory(prefix='adk-download-budget-') as directory:
        base = Path(directory)
        compiler = os.environ.get('CXX', 'c++')
        hanging = base / 'hanging.so'
        subprocess.run([compiler, '-std=c++17', '-shared', '-fPIC', str(ROOT / 'tests/integration/hanging_curl.cpp'), '-o', str(hanging)], check=True)
        probes = {}
        for mode, lib in (('libcurl', library), ('exec', '/nonexistent/libcurl.so'), ('hanging', str(hanging))):
            binary = base / mode
            subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-I' + str(ROOT / 'src'),
                            '-DADK_NFQ_TEST_CURL_LIBRARY="' + lib + '"', '-DADK_NFQ_TEST_CURL_PROGRAM="' + curl + '"',
                            str(ROOT / 'tests/integration/download_budget_probe.cpp'),
                            *(str(ROOT / 'src' / name) for name in ('net.cpp', 'net_policy.cpp', 'logging.cpp', 'bounded_log.cpp')),
                            '-lz', '-ldl', '-o', str(binary)], check=True)
            probes[mode] = binary
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        prefix = f'http://127.0.0.1:{server.server_port}'
        env = dict(os.environ, NO_PROXY='127.0.0.1,localhost')

        def run(mode, task, source, body, paths):
            server.requests.clear()
            for index in range(len(paths)):
                (base / str(index)).write_bytes(b'old rules\n')
            started = time.monotonic()
            result = subprocess.run([str(probes[mode]), str(task), str(source), str(body), str(base),
                                     *(prefix + path for path in paths)], env=env, capture_output=True, text=True, timeout=6)
            assert result.returncode == 0, (mode, result.stdout, result.stderr)
            return [list(map(int, line.split())) for line in result.stdout.splitlines()], time.monotonic() - started

        try:
            for mode in ('libcurl', 'exec'):
                rows, elapsed = run(mode, 850, 700, 1024, ['/slow-a', '/slow-b', '/unreached'])
                assert [row[1] for row in rows] == [1, 0, 0], (mode, rows)
                assert elapsed < 2 and server.requests == ['/slow-a', '/slow-b'], (mode, elapsed, server.requests)
                assert (base / '0').read_bytes() == b'x' * 16
                assert (base / '1').read_bytes() == (base / '2').read_bytes() == b'old rules\n'
                rows, elapsed = run(mode, 3000, 250, 256 * 1024 * 1024, ['/slow-single', '/ok'])
                assert [row[1] for row in rows] == [0, 1] and elapsed < 1.5, (mode, rows, elapsed)
                rows, _ = run(mode, 3000, 1000, 100, ['/body-a', '/body-b', '/unreached'])
                assert [row[1] for row in rows] == [1, 0, 0] and rows[-1][2] == 0, (mode, rows)
                assert server.requests == ['/body-a', '/body-b'], (mode, server.requests)
                assert (base / '1').read_bytes() == b'old rules\n'
                assert not list(base.glob('*.tmp.*')), 'confirmed downloads leaked temporary files'
            env['ADK_TEST_HANG_PID'] = str(base / 'hanging.pid')
            rows, elapsed = run('hanging', 200, 1000, 1024, ['/not-requested', '/still-not-requested'])
            assert [row[1] for row in rows] == [0, 0] and elapsed < 1.5 and not server.requests, (rows, elapsed)
            assert (base / '0').read_bytes() == b'old rules\n'
            assert not list(base.glob('*.tmp.*'))
            if sys.platform == 'linux':
                # Crash the owner only after its helper has inherited the
                # lease. Linux parent-death cancellation must release it.
                lease = base / 'lease'
                pid_file = base / 'hanging.pid'
                pid_file.unlink(missing_ok=True)
                env['ADK_TEST_DOWNLOAD_LEASE'] = str(lease)
                child = subprocess.Popen([str(probes['hanging']), '5000', '4000', '1024', str(base), prefix + '/never', prefix + '/never-2'],
                                         env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    deadline = time.monotonic() + 2
                    while not pid_file.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    assert pid_file.exists(), 'helper did not start'
                    with lease.open('r+') as handle:
                        try:
                            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        except BlockingIOError:
                            pass
                        else:
                            raise AssertionError('accepted download lease disappeared')
                        child.kill()
                        child.wait(timeout=2)
                        while time.monotonic() < deadline:
                            try:
                                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                break
                            except BlockingIOError:
                                time.sleep(.01)
                        else:
                            raise AssertionError('orphan library writer kept running after its owner died')
                finally:
                    if child.poll() is None:
                        child.kill()
                        child.wait()
            print('download budgets: slow multi-source totals, independent source deadline, cumulative body cap, hung library initialization, old files and crash leases passed')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == '__main__':
    main()
