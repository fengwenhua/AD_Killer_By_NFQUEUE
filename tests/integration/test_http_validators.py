#!/usr/bin/env python3
"""Conditional transfers, bounded headers and finite transient-only retries."""
import ctypes.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
TAG = 'W/"rules-v1"'
MODIFIED = 'Wed, 21 Oct 2015 07:28:00 GMT'


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get('If-None-Match'), self.headers.get('If-Modified-Since')))
        try:
            if self.path == '/redirect':
                self.send_response(302)
                self.send_header('Location', '/etag')
                self.send_header('ETag', '"wrong-redirect-tag"')
                self.end_headers()
                return
            attempts = sum(row[0] == self.path for row in self.server.requests)
            status = {'/404': 404, '/429': 429, '/503': 503, '/304-without-cache': 304}.get(self.path, 200)
            if self.path == '/retry' and attempts == 1:
                status = 503
            if self.path == '/etag' and self.headers.get('If-None-Match') == TAG:
                status = 304
            if self.path == '/modified' and self.headers.get('If-Modified-Since') == MODIFIED:
                status = 304
            self.send_response(status)
            self.send_header('ETag', TAG)
            if self.path == '/duplicate':
                self.send_header('ETag', '"duplicate"')
            self.send_header('Last-Modified', MODIFIED)
            if self.path == '/large-headers':
                self.send_header('X-Padding', 'x' * 18000)
            self.end_headers()
            if status != 304:
                self.wfile.write(b'ads.example.org\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(ThreadingHTTPServer):
    def get_request(self):
        self.accepts += 1
        return super().get_request()


def main():
    curl, library = shutil.which('curl'), ctypes.util.find_library('curl')
    assert curl and library and shutil.which('openssl')
    with tempfile.TemporaryDirectory(prefix='adk-http-validators-') as directory:
        base = Path(directory)
        probes = {}
        for mode in ('libcurl', 'exec'):
            binary = base / mode
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-pthread', '-I' + str(ROOT / 'src'),
                            '-DADK_NFQ_TEST_CURL_LIBRARY="' + (library if mode == 'libcurl' else '/nonexistent/libcurl.so') + '"',
                            '-DADK_NFQ_TEST_CURL_PROGRAM="' + curl + '"', str(ROOT / 'tests/integration/download_response_probe.cpp'),
                            *(str(ROOT / 'src' / name) for name in ('net.cpp', 'net_policy.cpp', 'logging.cpp', 'bounded_log.cpp')),
                            '-ldl', '-lz', '-o', str(binary)], check=True)
            probes[mode] = binary
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', str(base / 'key'), '-out', str(base / 'cert'),
                        '-days', '1', '-subj', '/CN=localhost'], check=True, capture_output=True)
        servers = [Server(('127.0.0.1', 0), Handler) for _ in range(2)]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(base / 'cert', base / 'key')
        servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
        for server in servers:
            server.requests, server.accepts = [], 0
            threading.Thread(target=server.serve_forever, daemon=True).start()
        env = dict(os.environ, NO_PROXY='127.0.0.1,localhost')

        def request(mode, path, validators=('', ''), scope='scoped', success=True, tls=False):
            server = servers[int(tls)]
            target = base / 'rules'
            target.write_bytes(b'old rules\n')
            server.requests.clear()
            result = subprocess.run([str(probes[mode]), f'{"https" if tls else "http"}://127.0.0.1:{server.server_port}' + path,
                                     str(target), *validators, scope], capture_output=True, text=True, env=env, timeout=8)
            assert (result.returncode == 0) == success, (mode, path, result.stdout, result.stderr)
            data = json.loads(result.stdout.splitlines()[-1])
            assert bool(data['error']) != success, (mode, path, data)
            assert not list(base.glob('rules.tmp.*'))
            return data, target.read_bytes(), list(server.requests)

        try:
            for mode in probes:
                for scope in ('direct', 'scoped'):
                    data, body, rows = request(mode, '/etag', scope=scope)
                    assert data['etag'] == TAG and data['last_modified'] == MODIFIED and data['status'] == 200
                    assert body == b'ads.example.org\n' and data['bytes'] == len(body) and len(rows) == 1
                    data, body, rows = request(mode, '/etag', (TAG, ''), scope=scope)
                    assert data['status'] == 304 and data['not_modified'] and data['bytes'] == 0 and body == b'old rules\n'
                    assert rows == [('/etag', TAG, None)]
                    data, body, rows = request(mode, '/modified', ('', MODIFIED), scope=scope)
                    assert data['status'] == 304 and data['not_modified'] and body == b'old rules\n'
                    assert rows == [('/modified', None, MODIFIED)]
                    data, _, _ = request(mode, '/redirect', scope=scope)
                    assert data['url'].endswith('/etag') and data['etag'] == TAG, data
                    data, _, _ = request(mode, '/duplicate', scope=scope)
                    assert not data['etag'], data
                    for bad in ('*', '"x"\r\nInjected: true'):
                        _, body, rows = request(mode, '/etag', (bad, ''), scope=scope, success=False)
                        assert not rows and body == b'old rules\n'
                    for path in ('/304-without-cache', '/large-headers', '/404', '/429'):
                        _, body, rows = request(mode, path, scope=scope, success=False)
                        assert len(rows) == 1 and body == b'old rules\n', (mode, path, rows)
                data, body, rows = request(mode, '/retry')
                assert len(rows) == 2 and body == b'ads.example.org\n', (mode, rows, data)
                data, body, rows = request(mode, '/503', success=False)
                assert len(rows) == 2 and data['status'] == 503 and body == b'old rules\n'
                before = servers[1].accepts
                _, body, _ = request(mode, '/etag', tls=True, success=False)
                assert servers[1].accepts - before == 1 and body == b'old rules\n', 'TLS certificate failure was retried'
            print('HTTP validators: real library/exec ETag and Last-Modified 304, final redirect metadata, bounded headers, no injection, finite 503-only recovery and no TLS retry passed')
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == '__main__':
    main()
