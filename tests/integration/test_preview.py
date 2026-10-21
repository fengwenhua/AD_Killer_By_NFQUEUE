"""Real bounded prefix downloads through both libcurl ABI and exec curl."""
import ctypes.util
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
TEXT = ('中文😀原始来源\n' * 1200).encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        try:
            if self.path in ('/redirect', '/file', '/loop', '/redirect-error'):
                self.send_response(302)
                self.send_header('Location', {'/redirect': '/text', '/file': 'file:///etc/passwd', '/loop': '/loop', '/redirect-error': '/403'}[self.path])
                self.end_headers()
                return
            if self.path in ('/403', '/429'):
                self.send_response(int(self.path[1:]))
                self.end_headers()
                self.wfile.write(b'{"message":"API rate limit exceeded"}')
                return
            if self.path in ('/ignored-range', '/slow-tail'):
                self.send_response(200)  # deliberately ignore Range
                self.send_header('Content-Length', str(10 * 1024 * 1024))
                self.end_headers()
                if self.path == '/slow-tail':
                    self.wfile.write(b'x' * 2064)
                    self.wfile.flush()
                    time.sleep(3)
                for _ in range(2560):
                    self.wfile.write(b'x' * 4096)
                    self.wfile.flush()
                    time.sleep(.001)
                return
            if self.path == '/short':
                self.send_response(200)
                self.send_header('Content-Length', '1000')
                self.end_headers()
                self.wfile.write(b'short')
                return
            if self.path == '/range':
                self.server.ranges.append(self.headers.get('Range'))
            data = {'/text': TEXT, '/range': TEXT, '/gzip': gzip.compress(TEXT), '/empty': b'', '/binary': b'\xff\0', '/html': b'<img src=x onerror=alert(1)>'}.get(self.path)
            self.send_response(200 if data is not None else 404)
            self.end_headers()
            self.wfile.write(data if data is not None else b'not found')
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    curl = shutil.which('curl')
    library = ctypes.util.find_library('curl')
    assert curl and library and shutil.which('openssl')
    with tempfile.TemporaryDirectory(prefix='ad_killer_by_nfqueue_preview_') as directory:
        base = Path(directory)
        probes = []
        for backend in ('libcurl', 'exec'):
            binary = base / backend
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-I' + str(ROOT / 'src'),
                            '-DADK_NFQ_TEST_CURL_LIBRARY="' + (library if backend == 'libcurl' else '/nonexistent/libcurl.so') + '"',
                            '-DADK_NFQ_TEST_CURL_PROGRAM="' + (curl if backend == 'exec' else '/nonexistent/curl') + '"',
                            str(ROOT / 'tests/integration/preview_probe.cpp'),
                            *(str(ROOT / 'src' / name) for name in ('source_preview.cpp', 'net.cpp', 'net_policy.cpp', 'logging.cpp', 'bounded_log.cpp')),
                            '-ldl', '-lz', '-o', str(binary)], check=True)
            probes.append(binary)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(base / 'key'), '-out', str(base / 'cert'), '-days', '1', '-subj', '/CN=localhost'], check=True, capture_output=True)
        servers = [ThreadingHTTPServer(('127.0.0.1', 0), Handler) for _ in range(2)]
        servers[0].ranges = []
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(base / 'cert', base / 'key')
        servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
        for server in servers:
            threading.Thread(target=server.serve_forever, daemon=True).start()
        env = dict(os.environ, NO_PROXY='127.0.0.1,localhost')
        (base / '.curlrc').write_text('insecure\n')
        env['CURL_HOME'] = str(base)
        try:
            for probe in probes:
                for path, expected in (('/text', 200), ('/403', 403), ('/429', 429), ('/404', 404), ('/redirect-error', 403)):
                    result = subprocess.run([str(probe), '--http-status', f'http://127.0.0.1:{servers[0].server_port}{path}'], capture_output=True, text=True, timeout=10, env=env)
                    data = json.loads(result.stdout)
                    assert data['http_status'] == expected, (probe.name, path, data)
                    assert (result.returncode == 0) == (expected == 200), (probe.name, path, data)
                    if expected != 200:
                        assert data['bytes'] == 0, 'HTTP error body was exposed as successful content'
                for path in ('/text', '/gzip', '/redirect', '/empty', '/html', '/ignored-range', '/slow-tail', '/binary', '/short', '/404', '/file', '/loop'):
                    start = time.monotonic()
                    result = subprocess.run([str(probe), f'http://127.0.0.1:{servers[0].server_port}{path}'], capture_output=True, text=True, timeout=10, env=env)
                    data = json.loads(result.stdout)
                    success = path in ('/text', '/gzip', '/redirect', '/empty', '/html', '/ignored-range', '/slow-tail')
                    assert (result.returncode == 0) == success, (probe.name, path, data, result.stderr)
                    if success:
                        assert data['characters'] == len(data['text']) <= 500
                        if path in ('/text', '/gzip', '/redirect'):
                            assert data['text'] == TEXT.decode()[:500] and data['truncated']
                        if path in ('/ignored-range', '/slow-tail'):
                            assert data['text'] == 'x' * 500 and data['truncated']
                            assert time.monotonic() - start < 2, (probe.name, path, 'waited for tail instead of stopping at prefix')
                for count in (100, 1000, 5000):
                    for path in ('/range', '/gzip'):
                        result = subprocess.run([str(probe), f'http://127.0.0.1:{servers[0].server_port}{path}', str(count)], capture_output=True, text=True, timeout=10, env=env)
                        data = json.loads(result.stdout)
                        assert result.returncode == 0 and data['text'] == TEXT.decode()[:count], (probe.name, count, data)
                        assert data['characters'] == count and data['truncated']
                        if path == '/range':
                            assert servers[0].ranges[-1] == f'bytes=0-{count * 4 + 63}'
                for count in ('0', '99', '5001', '9999999'):
                    result = subprocess.run([str(probe), 'http://127.0.0.1:1/invalid', count], capture_output=True, text=True, timeout=2, env=env)
                    assert result.returncode != 0 and '100–5000' in json.loads(result.stdout)['error']
                result = subprocess.run([str(probe), f'https://127.0.0.1:{servers[1].server_port}/text'], capture_output=True, timeout=10, env=env)
                assert result.returncode != 0, 'preview must not bypass TLS verification or read .curlrc'
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()
    print('real libcurl/exec prefix, ignored Range bounds, redirects, errors, UTF-8/gzip, empty content and TLS rejection passed')


if __name__ == '__main__':
    main()
