#!/usr/bin/env python3
"""Real CLI cache/304/expiry/group-crash recovery in disposable Linux root."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import gzip
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
BUILTIN = 'builtin:hagezi-pro-minus-fake'


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append((self.path, self.headers.get('If-None-Match')))
        try:
            if self.path == '/alias':
                self.send_response(302)
                self.send_header('Location', self.server.alias)
                self.end_headers()
                return
            state = self.server.sources.get(self.path)
            if not state:
                self.send_response(304)
                self.end_headers()
                return
            payload, tag, mode = state
            if self.server.corrupt is not None and self.headers.get('If-None-Match'):
                self.server.corrupt.write_bytes(b'c.example.org\n')
                self.server.corrupt = None
            if mode == 'slow':
                self.server.started.set()
                self.server.release.wait(6)
            status = 503 if mode == 'fail' else 304 if self.headers.get('If-None-Match') == tag else 200
            self.send_response(status)
            self.send_header('ETag', tag)
            self.end_headers()
            if status != 304:
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def fields(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


def tree():
    return {str(path.relative_to(DATA)): path.read_bytes() for path in (DATA / 'source_cache').rglob('*') if path.is_file()}


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'disposable Linux root environment required'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.sources = {
        '/a': (b'a.example.org\n', '"a1"', 'ok'), '/b': (b'b.example.org\n', '"b1"', 'ok'),
        '/pro': (b'*.keep.example.org\n*.old.example.org\n*.fake.example.org\n', '"p1"', 'ok'),
        '/fake': (b'*.fake.example.org\n', '"f1"', 'ok'),
    }
    server.requests, server.alias = [], '/a'
    server.corrupt = None
    server.started, server.release = threading.Event(), threading.Event()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    prefix = f'http://127.0.0.1:{server.server_port}'
    owner = None
    with tempfile.TemporaryDirectory(prefix='adk-cache-live-') as directory:
        binary = Path(directory) / 'AD_Killer_By_NFQUEUE'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"', '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                        f'-DADK_NFQ_TEST_HAGEZI_PRO_URL="{prefix}/pro"', f'-DADK_NFQ_TEST_HAGEZI_FAKE_URL="{prefix}/fake"',
                        '-DADK_NFQ_TEST_SYNC_TASK_MS=5000', '-DADK_NFQ_TEST_SYNC_SOURCE_MS=1500',
                        '-I' + str(ROOT / 'src'), *map(str, sorted((ROOT / 'src').glob('*.cpp'))), '-ldl', '-lz', '-o', str(binary)], check=True)

        def command(*args, text=None, success=True):
            result = subprocess.run([str(binary), *args], input=text, capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout

        def configure(*paths):
            command('--sources-save', text=''.join((BUILTIN if path == BUILTIN else prefix + path) + '\n' for path in paths) +
                    ('' if BUILTIN in paths else '# disabled: ' + BUILTIN + '\n'))

        def rules():
            generation = DATA / 'rulesets' / (DATA / 'rules.current').read_text().strip()
            return {line for name in ('hosts_rules.gz', 'adguard_rules.gz')
                    for line in gzip.decompress((generation / name).read_bytes()).decode().splitlines() if line and not line.startswith('#')}

        def record(path):
            url = (prefix + path).encode().hex()
            return next(item for item in (DATA / 'source_cache').glob('*/metadata') if fields(item).get('url_0') == url)

        def age(metadata, seconds):
            stamp = str(int(time.time()) - seconds)
            metadata.write_text(''.join(line.split('=', 1)[0] + '=' + stamp + '\n' if line.startswith(('verified_at=', 'fetched_')) else line
                                        for line in metadata.read_text().splitlines(keepends=True)))

        def job_wait(job, expected):
            for _ in range(200):
                state = json.loads(command('--update-status', job).splitlines()[-1])
                if state['status'] != 'running':
                    assert state['status'] == expected, state
                    return
                time.sleep(.025)
            raise AssertionError(('job did not finish', job))

        try:
            command('--install')
            command('--sync-concurrency', '1')  # Cache transaction scenarios require deterministic request order.
            configure('/a', '/b')
            command('--update')
            assert rules() == {'a.example.org', 'b.example.org'}
            original_manifest = (DATA / 'rules.current').read_bytes()
            a_meta = record('/a')
            age(a_meta, 10)
            original_cache_generation = a_meta.parent.name
            server.requests.clear()
            command('--update')
            assert server.requests == [('/a', '"a1"'), ('/b', '"b1"')]
            assert (DATA / 'rules.current').read_bytes() == original_manifest
            assert record('/a').parent.name == original_cache_generation
            assert int(fields(record('/a'))['verified_at']) > int(fields(record('/a'))['fetched_0'])
            assert 'revalidated_sources=2\n' in (DATA / 'source_stats.conf').read_text()

            server.sources['/a'] = (b'ignored.example.org\n', '"a1"', 'fail')
            server.sources['/b'] = (b'b2.example.org\n', '"b2"', 'ok')
            a_before = a_meta.read_bytes()
            command('--update')
            assert rules() == {'a.example.org', 'b2.example.org'} and a_meta.read_bytes() == a_before
            assert 'cached_sources=1\n' in (DATA / 'source_stats.conf').read_text()
            server.sources['/a'] = (b'ignored.example.org\n', '"a1"', 'slow')
            server.started.clear(); server.release.clear()
            started = time.monotonic()
            command('--update')
            server.release.set()
            assert server.started.is_set() and time.monotonic() - started < 3
            assert rules() == {'a.example.org', 'b2.example.org'} and a_meta.read_bytes() == a_before
            server.sources['/a'] = (b'ignored.example.org\n', '"a1"', 'fail')
            age(a_meta, 86402)
            before = (DATA / 'rules.current').read_bytes()
            server.requests.clear()
            command('--update', success=False)
            assert (DATA / 'rules.current').read_bytes() == before
            assert [row[0] for row in server.requests] == ['/a', '/a'], 'failed complete set kept requesting later sources'
            assert 'cache_expired' in (DATA / 'source_stats.conf').read_text()

            # A future timestamp cannot authorize cached fallback after rollback.
            age(a_meta, -20)
            command('--update', success=False)
            assert (DATA / 'rules.current').read_bytes() == before
            server.sources['/a'] = (b'a.example.org\n', '"a1"', 'ok')
            server.requests.clear()
            command('--update')
            assert server.requests[0] == ('/a', None)
            a_meta = record('/a')
            raw = a_meta.parent / 'body-0'
            raw.write_bytes(b'x' * raw.stat().st_size)
            server.requests.clear()
            command('--update')
            assert server.requests[0] == ('/a', None) and rules() == {'a.example.org', 'b2.example.org'}
            before = (DATA / 'rules.current').read_bytes()
            server.corrupt = record('/a').parent / 'body-0'
            command('--update', success=False)
            assert (DATA / 'rules.current').read_bytes() == before and rules() == {'a.example.org', 'b2.example.org'}
            assert 'source_digest_changed' in (DATA / 'source_stats.conf').read_text()
            command('--update')  # Persistent damage is repaired by a full fetch.
            assert rules() == {'a.example.org', 'b2.example.org'}

            configure('/alias')
            command('--update')
            assert rules() == {'a.example.org'}
            server.alias = '/b'
            server.sources['/b'] = (b'aliased-b.example.org\n', '"a1"', 'ok')
            server.requests.clear()
            command('--update')
            assert server.requests == [('/alias', '"a1"'), ('/b', '"a1"'), ('/alias', None), ('/b', None)]
            assert rules() == {'aliased-b.example.org'}, '304 reused another effective URL representation'
            configure('/uncached-304')
            before = (DATA / 'rules.current').read_bytes()
            command('--update', success=False)
            assert (DATA / 'rules.current').read_bytes() == before

            configure(BUILTIN)
            command('--update')
            assert rules() == {'keep.example.org', 'old.example.org'}
            group_before = tree()
            server.sources['/pro'] = (b'*.new.example.org\n*.fake.example.org\n', '"p2"', 'ok')
            server.sources['/fake'] = (b'*.fake.example.org\n', '"f1"', 'fail')
            command('--update')
            assert rules() == {'keep.example.org', 'old.example.org'} and tree() == group_before
            server.sources['/fake'] = (b'*.fake.example.org\n', '"f2"', 'ok')
            command('--update')
            assert rules() == {'new.example.org'}
            group_before = tree()
            server.sources['/fake'] = (b'<!doctype html><html>error</html>', '"bad-html"', 'ok')
            command('--update')
            assert rules() == {'new.example.org'} and tree() == group_before
            report_before = (DATA / 'source_stats.conf').read_bytes()
            test = json.loads(command('--source-test', BUILTIN).splitlines()[-1])['job_id']
            job_wait(test, 'failed')
            assert tree() == group_before and (DATA / 'source_stats.conf').read_bytes() == report_before

            server.sources['/fake'] = (b'*.fake.example.org\n', '"f2"', 'slow')
            server.sources['/pro'] = (b'*.never-commit.example.org\n', '"p3"', 'ok')
            server.started.clear(); server.release.clear()
            job = json.loads(command('--update-start').splitlines()[-1])['job_id']
            assert server.started.wait(3)
            state = fields(DATA / 'update_jobs' / f'update-{job}.state')
            owner = int(state['pid'])
            assert Path(f'/proc/{owner}/exe').resolve() == binary
            os.kill(owner, signal.SIGKILL)
            job_wait(job, 'interrupted'); owner = None
            server.release.set()
            assert tree() == group_before and rules() == {'new.example.org'}
            server.sources['/fake'] = (b'*.fake.example.org\n', '"f2"', 'fail')
            command('--update')
            assert tree() == group_before and rules() == {'new.example.org'}
            assert not (DATA / 'source_staging').exists(), 'crashed raw download staging was not reclaimed'
            # Managed GC must reject same-device binds too, not only st_dev
            # changes. These mounts belong exclusively to this fixture.
            configure('/a')
            command('--update')
            external = Path(directory) / 'external-body'
            external.write_bytes(b'outside cache: keep me\n'); external.chmod(0o600)
            mounted = record('/a').parent / 'body-0'
            subprocess.run(['mount', '--bind', str(external), str(mounted)], check=True)
            try:
                command('--update')  # Fresh complete set may publish without cache.
                assert external.read_bytes() == b'outside cache: keep me\n'
            finally:
                subprocess.run(['umount', str(mounted)], check=True)
            foreign = Path(directory) / 'external-stage'
            foreign.mkdir(mode=0o700)
            (foreign / 'source-download-0.part').write_bytes(b'outside stage: keep me\n')
            (DATA / 'source_staging').mkdir(mode=0o700)
            subprocess.run(['mount', '--bind', str(foreign), str(DATA / 'source_staging')], check=True)
            try:
                command('--update', success=False)
                assert (foreign / 'source-download-0.part').read_bytes() == b'outside stage: keep me\n'
            finally:
                subprocess.run(['umount', str(DATA / 'source_staging')], check=True)
            command('--update')
            assert not (DATA / 'daemon.pid').exists()
            print('real source cache: 304 reuse/refresh, TTL/clock rollback, corruption repair, redirect binding, no-cache 304 rejection, whole HaGeZi fallback, source-test isolation and crash recovery passed')
        finally:
            server.release.set()
            if owner is not None:
                try:
                    os.kill(owner, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            server.shutdown(); server.server_close(); thread.join()
            if DATA.exists():
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
