#!/usr/bin/env python3
"""Exercise the real sync loop's deadline and complete-source publication gate."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        try:
            if self.path.startswith('/slow'):
                time.sleep(.7)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'0.0.0.0 new.example.org\n' if self.path != '/seed' else b'0.0.0.0 old.example.org\n')
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'disposable Linux root environment required'
    with tempfile.TemporaryDirectory(prefix='adk-sync-budget-') as directory:
        binary = str(Path(directory) / 'AD_Killer_By_NFQUEUE')
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"', '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                        '-DADK_NFQ_TEST_SYNC_TASK_MS=1100', '-DADK_NFQ_TEST_SYNC_SOURCE_MS=900',
                        '-I' + str(ROOT / 'src'), *map(str, sorted((ROOT / 'src').glob('*.cpp'))), '-ldl', '-lz', '-o', binary], check=True)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.requests = []
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        prefix = f'http://127.0.0.1:{server.server_port}'

        def command(*args, text=None, success=True):
            result = subprocess.run([binary, *args], input=text, capture_output=True, text=True, timeout=8)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result

        def sources(*paths):
            command('--sources-save', text=''.join(prefix + path + '\n' for path in paths) + '# disabled: builtin:hagezi-pro-minus-fake\n')

        try:
            command('--install')
            sources('/seed')
            command('--update')
            manifest = (DATA / 'rules.current').read_bytes()
            generation = DATA / 'rulesets' / manifest.decode().strip()
            before = {path.name: path.read_bytes() for path in generation.iterdir() if path.is_file()}
            sources('/slow-a', '/slow-b', '/unreached')
            server.requests.clear()
            started = time.monotonic()
            result = command('--update', success=False)
            elapsed = time.monotonic() - started
            assert elapsed < 2.5 and server.requests == ['/slow-a', '/slow-b'], (elapsed, server.requests, result)
            assert (DATA / 'rules.current').read_bytes() == manifest
            assert {path.name: path.read_bytes() for path in generation.iterdir() if path.is_file()} == before
            assert not list((DATA / 'rulesets').glob('.staging-*'))
            assert not list(DATA.glob('download.part*'))
            report = (DATA / 'source_stats.conf').read_text()
            assert 'publish_status=incomplete_sources\n' in report and 'generation=' not in report
            assert 'accepted_sources=1\n' in report and 'rejected_sources=1\n' in report
            assert not (DATA / 'daemon.pid').exists()
            sources('/seed')
            command('--update')
            assert (DATA / 'rules.current').read_bytes() == manifest, 'next task inherited the previous deadline'
            print('real sync task budget: multiple slow sources, no further requests, no partial publication, old generation and fresh-task recovery passed')
        finally:
            server.shutdown()
            server.server_close()
            thread.join()
            if DATA.exists():
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
