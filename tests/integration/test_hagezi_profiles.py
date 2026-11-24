#!/usr/bin/env python3
"""Full Pro default, legacy upgrade/switches and isolated caches on real CLI/HTTP."""
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
from job_status import read_job_state

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
FULL = 'builtin:hagezi-pro'
LEGACY = 'builtin:hagezi-pro-minus-fake'
PRO = b'*.keep.example.org\n*.fake.example.org\n*.other.example.org\n'
FAKE = b'*.fake.example.org\n'


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        payload = self.server.pro if self.path == '/pro' else self.server.fake if self.path == '/fake' else b'ordinary.example.org\n'
        self.send_response(200 if payload is not None else 404)
        self.end_headers()
        self.wfile.write(payload if payload is not None else b'missing')


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'disposable Linux root only'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.pro, server.fake = [], PRO, FAKE
    threading.Thread(target=server.serve_forever, daemon=True).start()
    prefix = f'http://127.0.0.1:{server.server_port}'
    try:
        with tempfile.TemporaryDirectory(prefix='adk-hagezi-profiles-') as temp:
            binary = Path(temp) / 'AD_Killer_By_NFQUEUE'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                            '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"', '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                            f'-DADK_NFQ_TEST_HAGEZI_PRO_URL="{prefix}/pro"', f'-DADK_NFQ_TEST_HAGEZI_FAKE_URL="{prefix}/fake"',
                            '-I' + str(ROOT / 'src'), *map(str, sorted((ROOT / 'src').glob('*.cpp'))), '-ldl', '-lz', '-o', str(binary)], check=True)

            def command(*args, text=None, success=True):
                r = subprocess.run([str(binary), *args], input=text, text=True, capture_output=True, timeout=12)
                assert (r.returncode == 0) == success, (args, r.stdout, r.stderr)
                return r.stdout

            def builtin():
                rows = json.loads(command('--sources-list'))['sources']
                return next(s for s in rows if s['locked'])

            def hosts():
                generation = (DATA / 'rules.current').read_text().strip()
                text = gzip.decompress((DATA / 'rulesets' / generation / 'hosts_rules.gz').read_bytes()).decode()
                return {line for line in text.splitlines() if line and not line.startswith('#')}

            command('--install')
            assert builtin()['url'] == FULL and builtin()['enabled']
            assert builtin()['dependencies'] == [prefix + '/pro']
            assert {p['url'] for p in builtin()['profiles']} == {FULL, LEGACY}
            command('--sync-options', '1', '30', '0')
            for enabled in (False, True):
                source_text = ('' if enabled else '# disabled: ') + LEGACY + '\n'
                command('--sources-save', text=source_text)
                config = DATA / 'state.conf'
                config.write_text(config.read_text().replace('source_defaults_version=3\n', 'source_defaults_version=2\n'))
                saved = (DATA / 'update_sources.txt').read_bytes()
                before = config.read_text().replace('source_defaults_version=2\n', 'source_defaults_version=3\n')
                command('--install')
                assert (DATA / 'update_sources.txt').read_bytes() == saved
                assert config.read_text() == before
                assert builtin()['url'] == LEGACY and builtin()['enabled'] is enabled
            assert not server.requests, 'migration/configuration downloaded or started a sync'

            command('--sources-save', text='# disabled: ' + FULL + '\n')
            command('--sources-save', text=prefix + '/ordinary\n')
            assert builtin()['url'] == FULL and not builtin()['enabled'], 'omission changed selected profile or enablement'
            command('--sources-save', text=FULL + '\n')
            command('--update')
            assert server.requests == ['/pro'] and hosts() == {'keep.example.org', 'fake.example.org', 'other.example.org'}
            report = (DATA / 'source_stats.conf').read_text()
            assert 'format=hagezi-pro\n' in report and 'difference_removed=' not in report
            server.requests.clear()
            command('--update')
            assert not server.requests, 'fresh full-Pro cache downloaded again'

            command('--sources-save', text=LEGACY + '\n')
            server.fake = None
            snapshot = (DATA / 'rules.current').read_bytes()
            command('--update', success=False)
            assert server.requests == ['/pro', '/fake'], 'full Pro cache was incorrectly admitted as a legacy pair'
            assert (DATA / 'rules.current').read_bytes() == snapshot
            server.fake = FAKE
            server.requests.clear()
            command('--update')
            assert server.requests == ['/pro', '/fake'] and hosts() == {'keep.example.org', 'other.example.org'}

            command('--sources-save', text=FULL + '\n')
            server.requests.clear()
            command('--update')
            assert not server.requests and 'fake.example.org' in hosts(), 'switch reused the legacy difference as full Pro'
            saved = (DATA / 'update_sources.txt').read_bytes()
            command('--sources-save', text=FULL + '\n' + LEGACY + '\n', success=False)
            assert (DATA / 'update_sources.txt').read_bytes() == saved

            # Expiring only full Pro must not admit the still-valid legacy
            # cache, even when that older profile was used successfully.
            full_meta = next(p for p in (DATA / 'source_cache').rglob('metadata') if '\ncount=1\n' in p.read_text())
            old = str(int(time.time()) - 86410)
            full_meta.write_text(''.join(line.split('=', 1)[0] + '=' + old + '\n' if line.startswith(('verified_at=', 'fetched_')) else line
                                         for line in full_meta.read_text().splitlines(keepends=True)))
            server.pro = None
            server.requests.clear()
            snapshot = (DATA / 'rules.current').read_bytes()
            command('--update', success=False)
            assert server.requests == ['/pro'] and (DATA / 'rules.current').read_bytes() == snapshot
            assert 'cache_expired' in (DATA / 'source_stats.conf').read_text()

            server.pro = PRO
            command('--sources-save', text='# disabled: ' + FULL + '\n')
            tracked = ['state.conf', 'update_sources.txt', 'rules.current', 'source_stats.conf', 'available_rule_stats.conf']
            protected = {name: (DATA / name).read_bytes() for name in tracked}
            cache_before = {str(p.relative_to(DATA)): p.read_bytes() for p in (DATA / 'source_cache').rglob('*') if p.is_file()}
            server.requests.clear()
            started = json.loads(command('--source-test', FULL))
            for _ in range(200):
                state = read_job_state(binary, started['job_id'])
                if state['status'] not in ('running', 'unverified'):
                    break
                time.sleep(.025)
            assert state['status'] == 'success' and server.requests == ['/pro']
            assert {name: (DATA / name).read_bytes() for name in tracked} == protected
            assert cache_before == {str(p.relative_to(DATA)): p.read_bytes() for p in (DATA / 'source_cache').rglob('*') if p.is_file()}
            server.requests.clear()
            preview = json.loads(command('--source-preview', FULL))
            assert server.requests == ['/pro'] and len(preview['previews']) == 1
            assert preview['previews'][0]['text'] == PRO.decode()
            assert {name: (DATA / name).read_bytes() for name in tracked} == protected
            assert cache_before == {str(p.relative_to(DATA)): p.read_bytes() for p in (DATA / 'source_cache').rglob('*') if p.is_file()}
            assert not (DATA / 'daemon.pid').exists()
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(DATA, ignore_errors=True)
    print('Full-Pro real CLI/HTTP: fresh default, legacy upgrade/enablement, explicit switches, Fake inclusion, '
          'single request, isolated/expired caches, old-generation protection and read-only disabled tests/previews passed')


if __name__ == '__main__':
    main()
