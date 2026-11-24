#!/usr/bin/env python3
"""Actual CLI/HTTP mirror failover, endpoint validators and shared retry caps.

Disposable Linux root only. All transport endpoints are compile-time loopback
fixtures; production source identities and typed recipe recognition stay real.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
SOURCE = 'https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts'
CDN_ALIAS = 'https://cdn.jsdelivr.net/gh/StevenBlack/hosts@master/hosts'
FASTLY_ALIAS = 'https://fastly.jsdelivr.net/gh/StevenBlack/hosts@master/hosts'
BUILTIN = 'builtin:hagezi-pro-minus-fake'


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        endpoint = self.path.split('/')[1]
        component = 'pro' if self.path.endswith('/wildcard/pro.txt') else 'fake' if self.path.endswith('/wildcard/fake.txt') else ''
        tag = '"' + endpoint + ('-' + component if component else '') + '-v1"'
        mode = self.server.modes.get(endpoint + ':' + component, self.server.modes.get(endpoint, 'ok'))
        self.server.requests.append((endpoint, self.headers.get('If-None-Match')))
        if mode == 'slow':
            time.sleep(2)
        elif mode == 'steady':
            time.sleep(1.2)
        payload = b'<html><body>upstream error</body></html>' if mode == 'html' else b'0.0.0.0 mirror.example.org\n'
        if component and mode != 'html':
            payload = b'*.keep.example.org\n*.fake.example.org\n' if component == 'pro' else b'*.fake.example.org\n'
            if component == 'pro' and self.server.pro_extra:
                payload += b'*.new.example.org\n'
        status = 503 if mode == 'fail' else 304 if mode == 'unsolicited304' or (
            mode == 'ok' and self.headers.get('If-None-Match') == tag) else 200
        try:
            self.send_response(status)
            self.send_header('ETag', tag)
            if status != 304:
                self.send_header('Content-Length', str(len(payload) if status == 200 else 0))
            self.end_headers()
            if status == 200:
                self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass


def fields(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires disposable Linux root with no module data'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.modes, server.requests, server.pro_extra = {}, [], False
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    prefix = f'http://127.0.0.1:{server.server_port}'
    try:
        with tempfile.TemporaryDirectory(prefix='adk-mirrors-') as directory:
            binary = Path(directory) / 'AD_Killer_By_NFQUEUE'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread',
                            '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                            '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"',
                            '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                            f'-DADK_NFQ_TEST_SOURCE_MIRROR_BASE="{prefix}"',
                            '-DADK_NFQ_TEST_SYNC_SOURCE_MS=4000', '-DADK_NFQ_TEST_SYNC_TASK_MS=6000',
                            '-I' + str(ROOT / 'src'), *map(str, sorted((ROOT / 'src').glob('*.cpp'))),
                            '-ldl', '-lz', '-o', str(binary)], check=True)

            def command(*args, text=None, success=True):
                result = subprocess.run([str(binary), *args], input=text, capture_output=True, text=True, timeout=12)
                assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
                return result.stdout

            def save(url=SOURCE):
                command('--sources-save', text=url + '\n# disabled: ' + BUILTIN + '\n')

            def sync(modes, expected, success=True):
                server.modes, server.requests = modes, []
                command('--update', success=success)
                assert server.requests == expected, server.requests
                return fields(DATA / 'source_stats.conf')

            command('--install')
            command('--sync-options', '1', '0', '2')
            save()
            report = sync({'cdn': 'fail'}, [('cdn', None), ('fastly', None)])
            assert report['publish_status'] == 'published' and report['attempts'] == '2'
            assert report['download_url_0'].startswith(prefix + '/fastly/')
            before = (DATA / 'rules.current').read_bytes()

            # Cross-endpoint switches request a complete response without the
            # previous endpoint's ETag. Same-endpoint requests can use a 304.
            report = sync({}, [('cdn', None)])
            assert report['download_url_0'].startswith(prefix + '/cdn/')
            report = sync({}, [('cdn', '"cdn-v1"')])
            assert report['provenance'] == 'revalidated'
            report = sync({'cdn': 'fail'}, [('cdn', '"cdn-v1"'), ('fastly', None)])
            assert report['attempts'] == '2'
            report = sync({'cdn': 'unsolicited304'}, [('cdn', None), ('fastly', '"fastly-v1"')])
            assert report['provenance'] == 'revalidated'
            assert report['download_url_0'].startswith(prefix + '/fastly/')
            assert (DATA / 'rules.current').read_bytes() == before

            # Fresh reuse performs no requests and keeps the actual cache
            # provenance rather than labelling it as a new CDN download.
            command('--sync-options', '1', '30', '2')
            report = sync({'cdn': 'fail', 'fastly': 'fail', 'origin': 'fail'}, [])
            assert report['provenance'] == 'cache_fresh' and report['attempts'] == '0'
            assert report['download_url_0'].startswith(prefix + '/fastly/')

            # No hidden per-mirror retry multiplier: zero retries means one
            # HTTP request; two retries permit exactly three total attempts.
            command('--sync-options', '1', '0', '0')
            report = sync({'cdn': 'fail'}, [('cdn', None)])
            assert report['provenance'] == 'cache_fallback' and report['attempts'] == '1'
            command('--sync-options', '1', '0', '2')
            report = sync({'cdn': 'fail', 'fastly': 'fail', 'origin': 'fail'},
                          [('cdn', None), ('fastly', '"fastly-v1"'), ('origin', None)])
            assert report['provenance'] == 'cache_fallback' and report['attempts'] == '3'
            command('--sync-options', '1', '30', '2')
            report = sync({}, [('cdn', None)])
            assert report['provenance'] == 'downloaded', 'failed-download fallback must be retried on next click'

            command('--sync-options', '1', '0', '2')
            report = sync({'cdn': 'html'}, [('cdn', '"cdn-v1"'), ('fastly', None)])
            assert report['status'] == 'accepted' and report['attempts'] == '2'
            report = sync({'cdn': 'fail', 'fastly': 'fail'},
                          [('cdn', None), ('fastly', '"fastly-v1"'), ('origin', None)])
            assert report['download_url_0'].startswith(prefix + '/origin/')

            # A healthy larger response must not be cut off at one third of
            # the source allowance simply because two retries are configured.
            report = sync({'cdn': 'steady'}, [('cdn', None)])
            assert report['status'] == 'accepted' and report['attempts'] == '1'

            # A stalled primary receives only its share of the existing source
            # deadline, leaving a useful interval for the alternate transport.
            started = time.monotonic()
            report = sync({'cdn': 'slow'}, [('cdn', '"cdn-v1"'), ('fastly', None)])
            assert time.monotonic() - started < 4
            assert report['status'] == 'accepted' and report['attempts'] == '2'

            # Aliases retain names/format/cache identity without rewriting the
            # user's saved URL. An explicitly selected Fastly alias goes first.
            save(FASTLY_ALIAS)
            configured = json.loads(command('--sources-list'))['sources'][0]
            assert configured['url'] == FASTLY_ALIAS and configured['name'] == 'StevenBlack hosts'
            command('--sync-options', '1', '30', '2')
            report = sync({}, [])
            assert report['url'] == FASTLY_ALIAS and report['provenance'] == 'cache_fresh'
            command('--sync-options', '1', '0', '2')
            report = sync({}, [('fastly', '"fastly-v1"')])
            assert report['format'] == 'hosts'
            save(CDN_ALIAS)
            report = sync({}, [('cdn', None)])
            assert report['url'] == CDN_ALIAS

            # With no verified cache, an all-endpoint failure must preserve the
            # whole last generation. This filesystem is disposable test data.
            shutil.rmtree(DATA / 'source_cache')
            snapshot = (DATA / 'rules.current').read_bytes()
            report = sync({'cdn': 'fail', 'fastly': 'fail', 'origin': 'fail'},
                          [('cdn', None), ('fastly', None), ('origin', None)], success=False)
            assert report['publish_status'] == 'incomplete_sources'
            assert (DATA / 'rules.current').read_bytes() == snapshot

            # The locked pair changes transport as one retried source and
            # falls back as a complete group, never new Pro + old Fake.
            command('--sources-save', text=BUILTIN + '\n')
            report = sync({'cdn:fake': 'fail'}, [('cdn', None), ('cdn', None), ('fastly', None), ('fastly', None)])
            assert report['status'] == 'accepted' and report['attempts'] == '2'
            assert report['difference_removed'] == '1'
            assert all(report[f'download_url_{i}'].startswith(prefix + '/fastly/') for i in (0, 1))

            def hosts():
                generation = (DATA / 'rules.current').read_text().strip()
                return gzip.decompress((DATA / 'rulesets' / generation / 'hosts_rules.gz').read_bytes())

            assert b'keep.example.org' in hosts() and b'fake.example.org' not in hosts()
            command('--sync-options', '1', '30', '2')
            report = sync({'cdn': 'fail', 'fastly': 'fail'}, [])
            assert report['provenance'] == 'cache_fresh'
            command('--sync-options', '1', '0', '2')
            server.pro_extra = True
            report = sync({'cdn:fake': 'html', 'fastly': 'fail', 'origin': 'fail'},
                          [('cdn', None), ('cdn', None), ('fastly', '"fastly-pro-v1"'), ('origin', None)])
            assert report['provenance'] == 'cache_fallback'
            assert b'new.example.org' not in hosts(), 'mixed new Pro / cached Fake was published'

            save(prefix + '/custom')
            command('--sync-options', '1', '0', '0')
            report = sync({}, [('custom', None)])
            assert report['download_url_0'] == prefix + '/custom'
            assert not (DATA / 'daemon.pid').exists(), 'sync must not start the engine'

            command('--sync-options', '1', '0', '2')
            unchanged = {str(p.relative_to(DATA)): p.read_bytes() for p in DATA.rglob('*') if p.is_file()}
            server.modes, server.requests = {'cdn': 'fail'}, []
            preview = json.loads(command('--source-preview', SOURCE, '100'))
            assert preview['status'] == 'ok' and server.requests == [('cdn', None), ('fastly', None)]
            assert preview['previews'][0]['url'] == SOURCE
            assert preview['previews'][0]['request_url'].startswith(prefix + '/fastly/')
            assert 'mirror.example.org' in preview['previews'][0]['text']
            assert unchanged == {str(p.relative_to(DATA)): p.read_bytes() for p in DATA.rglob('*') if p.is_file()}, 'preview changed module data'
            command('--sync-options', '1', '0', '0')
            server.requests = []
            command('--source-preview', SOURCE, '100', success=False)
            assert server.requests == [('cdn', None)], 'preview must respect zero retries'
    finally:
        server.shutdown()
        server.server_close()
        shutil.rmtree(DATA, ignore_errors=True)
    print('Real mirror HTTP/CLI: endpoint failover, HTML/slow/unsolicited304, validator isolation, '
          'retry caps, fresh/fallback cache, aliases/custom sources and whole-generation retention passed')


if __name__ == '__main__':
    main()
