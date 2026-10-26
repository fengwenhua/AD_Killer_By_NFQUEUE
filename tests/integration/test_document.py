"""Complete bounded in-memory metadata through supervised libcurl and exec."""
import ctypes.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args): pass

    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get('Range')))
        try:
            if self.path == '/slow': time.sleep(.4)
            if self.path == '/fast': time.sleep(.5)
            status = 206 if self.path == '/partial' else 403 if self.path == '/denied' else 200
            if self.path == '/redirect':
                self.send_response(302); self.send_header('Location', '/eight'); self.end_headers(); return
            self.send_response(status)
            if self.path == '/incomplete': self.send_header('Content-Length','100')
            self.end_headers()
            if self.path == '/incomplete': self.wfile.write(b'[]'); return
            if self.path == '/tail':
                self.wfile.write(b'partial'); self.wfile.flush(); time.sleep(.4)
            self.wfile.write(b'x' * (100000 if self.path == '/large' else 257 if self.path == '/overflow' else 256 if self.path == '/exact' else 8))
        except (BrokenPipeError, ConnectionResetError): pass


def main():
    curl, library = shutil.which('curl'), ctypes.util.find_library('curl')
    assert curl and library
    with tempfile.TemporaryDirectory(prefix='adk-document-') as directory:
        base = Path(directory); compiler = os.environ.get('CXX', 'c++')
        hanging = base / 'hanging.so'
        subprocess.run([compiler, '-std=c++17', '-shared', '-fPIC', str(ROOT/'tests/integration/hanging_curl.cpp'), '-o', str(hanging)], check=True)
        probes = {}
        for mode, lib in (('libcurl', library), ('exec', '/nonexistent/libcurl.so'), ('hanging', str(hanging))):
            probe = base / mode
            subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-I'+str(ROOT/'src'),
                            '-DADK_NFQ_TEST_CURL_LIBRARY="'+lib+'"', '-DADK_NFQ_TEST_CURL_PROGRAM="'+curl+'"',
                            str(ROOT/'tests/integration/document_probe.cpp'),
                            *(str(ROOT/'src'/name) for name in ('net.cpp','net_policy.cpp','logging.cpp','bounded_log.cpp')),
                            '-ldl', '-lz', '-o', str(probe)], check=True)
            probes[mode] = probe
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler); server.requests = []
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f'http://127.0.0.1:{server.server_port}'
        env = dict(os.environ, NO_PROXY='127.0.0.1,localhost')
        def run(mode, paths, milliseconds=2000, budget=512*1024, cap=512*1024):
            server.requests.clear(); started = time.monotonic()
            result = subprocess.run([str(probes[mode]), str(milliseconds), str(budget), str(cap), *(url+p for p in paths)],
                                    capture_output=True, text=True, timeout=5, env=env)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout), time.monotonic()-started
        try:
            for mode in ('libcurl','exec'):
                for path, size, cap in (('/large',100000,512*1024),('/exact',256,256),('/redirect',8,256)):
                    values, _ = run(mode, [path], cap=cap)
                    assert values[0]['bytes'] == size and not values[0]['error'], (mode, path, values)
                    assert all(header is None for _, header in server.requests), 'complete JSON must not request Range'
                for path, status in (('/overflow',200),('/partial',206),('/denied',403),('/incomplete',200)):
                    values, _ = run(mode, [path], cap=256)
                    assert values[0]['error'] and values[0]['bytes'] == 0 and values[0]['http_status'] == status, (mode,path,values)
                for path in ('/slow','/tail'):
                    values, elapsed = run(mode, [path], milliseconds=100)
                    assert values[0]['error'] and values[0]['bytes'] == 0 and elapsed < 1, (mode,path,values,elapsed)
                values, elapsed = run(mode, ['/fast','/fast','/fast'], milliseconds=750)
                assert len(values) == 2 and not values[0]['error'] and values[1]['error'] and elapsed < 2.5, (mode,values,elapsed)
                values, _ = run(mode, ['/eight','/eight','/eight'], budget=12, cap=8)
                assert len(values) == 2 and values[0]['bytes'] == 8 and values[1]['error'] and values[1]['remaining'] == 0, (mode,values)
            pid_file = base / 'hanging.pid'; env['ADK_TEST_HANG_PID'] = str(pid_file)
            values, elapsed = run('hanging', ['/eight'], milliseconds=500)
            assert values[0]['error'] and elapsed < 2 and not server.requests, (values,elapsed,server.requests)
            assert pid_file.exists(), (values,elapsed)
            try: os.kill(int(pid_file.read_text()), 0)
            except ProcessLookupError: pass
            else: raise AssertionError('library initialization child survived its parent deadline')
        finally:
            server.shutdown(); server.server_close()
    print('complete documents: libcurl/exec, >32KiB, exact/overflow/206, no Range, shared time/bytes and hanging-init reaping passed')


if __name__ == '__main__': main()
