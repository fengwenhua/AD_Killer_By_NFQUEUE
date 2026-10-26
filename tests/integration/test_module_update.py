#!/usr/bin/env python3
"""Disposable Linux/root: real module download, digest, jobs and fake manager CLI."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import zipfile
from urllib.parse import urlparse, parse_qs

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
MANAGER = Path('/data/adb/ksud')
MODULE = Path('/data/adb/modules/AD_Killer_By_NFQUEUE')
NAME = 'AD_Killer_By_NFQUEUE-v1.1.zip'
buffer = io.BytesIO()
with zipfile.ZipFile(buffer, 'w') as archive:
    archive.writestr('module.prop', 'id=AD_Killer_By_NFQUEUE\nversion=v1.1\nversionCode=110\nreleaseChannel=stable\n')
PACKAGE = buffer.getvalue()
DIGEST = hashlib.sha256(PACKAGE).hexdigest()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        base = f'http://127.0.0.1:{self.server.server_port}'
        mode = self.server.mode
        page = int(parse_qs(urlparse(self.path).query).get('page', ['1'])[0])
        status = self.server.api_status if self.path.startswith('/api') else 200
        if mode == 'bad_next' and page == 2: status = 429
        if status != 200:
            self.send_response(status); self.end_headers()
            self.wfile.write(b'{"message":"API rate limit exceeded"}'); return
        if mode == 'slow' and self.path.startswith('/api'): time.sleep(.6)
        def release(version='1.1', preview=False):
            name = f'AD_Killer_By_NFQUEUE-v{version}.zip'
            url = f'{base}/releases/download/v{version}/{name}'
            return {'tag_name': 'v'+version, 'draft': False, 'prerelease': preview,
                    'html_url': f'{base}/releases/tag/v{version}', 'name': 'Release '+version,
                    'body': '大'*160000 if mode == 'long' else 'x'*600000 if mode == 'too_large' else 'Notes from GitHub Release',
                    'published_at': '2026-10-26T00:00:00Z',
                    'assets': [{'name':name, 'size':len(PACKAGE), 'browser_download_url':url},
                               {'name':name+'.sha256', 'size':96, 'browser_download_url':url+'.sha256'}]}
        if self.path.endswith('update.json'):
            content = json.dumps({'module':'AD_Killer_By_NFQUEUE','version':'v1.1','versionCode':110,
                                  'releaseChannel':'stable','sha256':DIGEST,
                                  'zipUrl':base+'/releases/download/v1.1/'+NAME,
                                  'changelog':base+'/releases/download/v1.1/changelog.md'}).encode()
            if self.server.static_bad: content = b'{"version":"wrong"}'
        elif self.path == '/api?per_page=1':
            content = json.dumps([release()], ensure_ascii=False).encode()
        elif self.path.startswith('/api?per_page=5&page='):
            if mode == 'short': entries = []
            elif mode == 'limit' or (mode in ('pages','bad_next','malformed','slow') and page == 1):
                entries = [{**release(), 'assets':[]} for _ in range(5)]
            elif mode == 'pages': entries = [release(),release('1.2',True)]
            else: entries = [release()]
            content = json.dumps(entries,ensure_ascii=False).encode()
            if mode == 'malformed' and page == 2: content = b'[{"tag_name":"v1.1"}'
        elif self.path == '/api/latest': content = json.dumps(release(),ensure_ascii=False).encode()
        elif self.path.endswith('.sha256'):
            content = (('0'*64 if self.server.corrupt else DIGEST)+'  '+NAME+'\n').encode()
        elif self.path.endswith('.zip'): content = PACKAGE
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200); self.end_headers()
        try: self.wfile.write(content)
        except (BrokenPipeError, ConnectionResetError): pass


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires disposable Linux/root'
    assert not MODULE.exists(), 'refusing an existing module fixture'
    assert not MANAGER.exists() and not Path('/data/adb/apd').exists() and not Path('/data/adb/magisk').exists(), 'refusing existing root manager'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.corrupt = False
    server.api_status = 200
    server.mode = "normal"
    server.static_bad = False
    server.requests = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    with tempfile.TemporaryDirectory(prefix='adk_module_update_cli_') as temporary:
        binary = Path(temporary)/'AD_Killer_By_NFQUEUE'
        def build(target, version, code, channel):
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror',
                        f'-DMODULE_VERSION="{version}"', f'-DMODULE_VERSION_CODE={code}', f'-DMODULE_RELEASE_CHANNEL="{channel}"',
                        '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"', '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                        '-DADK_NFQ_TEST_INSTALL_WARNING_MS=100', '-DADK_NFQ_TEST_RELEASE_TIME_MS=1000',
                        f'-DADK_NFQ_TEST_RELEASE_REPO="{base}"', f'-DADK_NFQ_TEST_RELEASE_API="{base}/api"',
                        '-I'+str(ROOT/'src'), *map(str, sorted((ROOT/'src').glob('*.cpp'))), '-ldl', '-lz', '-o', str(target)], check=True)
        build(binary, '1.0', 100, 'stable')

        def command(*args, success=True):
            result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=25)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout

        def job(action, success=True, channel=None):
            data = json.loads(command(action, '1.1', *([channel] if channel else [])))
            assert data['status'] == 'started'
            for _ in range(200):
                state = json.loads(command('--update-status', data['job_id']))
                if state['status'] != 'running':
                    assert state['status'] == ('success' if success else 'failed'), state
                    return
                time.sleep(.025)
            raise AssertionError('module update job timed out')

        try:
            DATA.parent.mkdir(parents=True, exist_ok=True)
            command('--install')
            before = (DATA/'state.conf').read_bytes()
            empty = json.loads(command('--start', success=False))
            assert empty['status'] == 'error_rules_empty' and '同步开源规则' in empty['message']
            assert (DATA/'state.conf').read_bytes() == before and not (DATA/'daemon.pid').exists()
            assert json.loads(command('--module-update-check', 'stable'))['update']['version'] == 'v1.1'
            assert json.loads(command('--module-release-notes'))[0]['body'] == 'Notes from GitHub Release'
            command('--module-release-notes', 'unexpected', success=False)
            for status in (403, 429):
                server.api_status = status
                for args in (('--module-release-notes',), ('--module-update-check', 'preview')):
                    failure = json.loads(command(*args, success=False))
                    assert failure['http_status'] == status and '匿名请求配额' in failure['message']
                assert 'update' in json.loads(command('--module-update-check', 'stable')), 'API quota should not break static stable metadata'
            server.api_status = 200
            server.mode = 'pages'; server.requests.clear()
            versions = json.loads(command('--module-update-check', 'preview'))
            assert [item['tag_name'] for item in versions] == ['v1.1','v1.2']
            assert all('body' not in item for item in versions)
            assert server.requests == ['/api?per_page=5&page=1','/api?per_page=5&page=2']
            server.mode = 'long'
            assert len(command('--module-update-check','preview')) < 2000, 'check must omit long bodies/API metadata'
            notes = json.loads(command('--module-release-notes'))[0]
            assert notes['body_truncated'] and len(notes['body'].encode()) <= 16000 and notes['body'].startswith('大')
            for mode, expected_requests in (('limit',4),('short',1),('malformed',2),('bad_next',2),('too_large',1)):
                server.mode = mode; server.requests.clear()
                failure = json.loads(command('--module-update-check','preview',success=False))
                assert failure['status'] == 'error' and 'update' not in failure, (mode,failure)
                assert len(server.requests) == expected_requests, (mode,server.requests)
                if mode == 'bad_next': assert failure['http_status'] == 429
            server.mode = 'slow'; started = time.monotonic()
            assert json.loads(command('--module-update-check','preview',success=False))['status'] == 'error'
            assert time.monotonic()-started < 3, 'page two must share page one deadline'
            server.mode = 'normal'; server.static_bad = True; server.requests.clear()
            assert json.loads(command('--module-update-check','stable'))[0]['tag_name'] == 'v1.1'
            assert server.requests == ['/releases/latest/download/update.json','/api/latest']
            server.static_bad = False
            command('--module-update-download', '1.0', success=False)
            command('--module-update-download', '../bad', success=False)
            command('--module-update-download', '1.1', 'invalid', success=False)
            # A release flag must never let a stable ZIP masquerade as preview.
            job('--module-update-download', success=False, channel='preview')
            job('--module-update-install', success=False)
            job('--module-update-download')
            state = json.loads(command('--module-update-status'))
            assert state['version'] == '1.1' and not state['rebootRequired']
            assert (DATA/'module_updates/package.zip').read_bytes() == PACKAGE
            assert not MANAGER.exists(), 'download attempted installation'
            server.corrupt = True
            job('--module-update-download', success=False)
            assert json.loads(command('--module-update-status'))['version'] == ''
            server.corrupt = False
            job('--module-update-download')
            (DATA/'module_updates/package.zip').write_bytes(PACKAGE+b'tampered')
            job('--module-update-install', success=False)
            job('--module-update-download')
            MANAGER.write_text('#!/bin/sh\nexit 7\n')
            MANAGER.chmod(0o700)
            job('--module-update-install', success=False)
            assert not json.loads(command('--module-update-status'))['rebootRequired']
            MANAGER.unlink()
            # A slow manager closes its output and exceeds the warning budget.
            # Keep waiting for its normal exit; never kill an installing module.
            MANAGER.write_text('#!/bin/sh\nexec 1>&- 2>&-\nprintf ready > /data/adb/AD_Killer_By_NFQUEUE/fake-manager-ready\n'
                               'while [ ! -f /data/adb/AD_Killer_By_NFQUEUE/fake-manager-release ]; do sleep .02; done\n'
                               'printf completed > /data/adb/AD_Killer_By_NFQUEUE/fake-slow-installed\n')
            MANAGER.chmod(0o700)
            MODULE.mkdir(parents=True)
            (MODULE/'module.prop').write_text('id=AD_Killer_By_NFQUEUE\n')
            active = json.loads(command('--module-update-install', '1.1'))['job_id']
            for _ in range(200):
                if (DATA/'fake-manager-ready').exists():
                    break
                time.sleep(.025)
            else:
                raise AssertionError('slow manager did not start')
            time.sleep(.2)  # Exceed the warning threshold with closed stdout.
            before_uninstall = (DATA/'state.conf').read_bytes()
            blocked = json.loads(command('--uninstall', success=False))
            assert blocked['status'] == 'uninstall_busy' and not (MODULE/'.uninstalling').exists(), blocked
            assert (DATA/'state.conf').read_bytes() == before_uninstall
            assert json.loads(command('--update-status', active))['phase'] == 'installing'
            (DATA/'fake-manager-release').touch()
            for _ in range(200):
                completed = json.loads(command('--update-status', active))
                if completed['status'] == 'success':
                    break
                assert completed['status'] == 'running', completed
                time.sleep(.025)
            else:
                raise AssertionError('slow manager did not complete')
            assert (DATA/'fake-slow-installed').read_text() == 'completed'
            MANAGER.unlink()
            shutil.rmtree(MODULE)
            for path, magisk in ((MANAGER, False), (Path('/data/adb/apd'), False), (Path('/data/adb/magisk/magisk'), True)):
                job('--module-update-download')
                path.parent.mkdir(parents=True, exist_ok=True)
                check = '[ "$1" = --install-module ] && [ "$2" = /data/adb/AD_Killer_By_NFQUEUE/module_updates/package.zip ]' if magisk else '[ "$1" = module ] && [ "$2" = install ] && [ "$3" = /data/adb/AD_Killer_By_NFQUEUE/module_updates/package.zip ]'
                path.write_text('#!/bin/sh\n'+check+' || exit 3\nprintf installed > /data/adb/AD_Killer_By_NFQUEUE/fake-installed\n')
                path.chmod(0o700)
                job('--module-update-install')
                assert (DATA/'fake-installed').read_text() == 'installed'
                assert json.loads(command('--module-update-status'))['rebootRequired']
                path.unlink()
            assert (DATA/'state.conf').read_bytes() == before
            assert not (DATA/'daemon.pid').exists()
            # Same number: only an identified preview build may install stable.
            preview_binary = Path(temporary)/'AD_Killer_By_NFQUEUE-preview'
            build(preview_binary, '1.1', 109, 'preview')
            binary = preview_binary
            assert json.loads(command('--version-status'))['releaseChannel'] == 'preview'
            command('--module-update-download', '1.1', 'preview', success=False)
            command('--module-update-download', '1.0', success=False)
            job('--module-update-download')
            same = json.loads(command('--module-update-status'))
            assert same['version'] == '1.1' and same['channel'] == 'stable' and not same['rebootRequired']
            MANAGER.write_text('#!/bin/sh\nexit 0\n')
            MANAGER.chmod(0o700)
            job('--module-update-install')
            assert json.loads(command('--module-update-status'))['rebootRequired'], 'same-version stable installation must wait for reboot'
            print('real module-update CLI/jobs, channel identity, same-version promotion, tamper rejection, manager argv and reboot state passed')
        finally:
            # Let our own fake installer finish if an assertion fails; it must
            # never be left waiting on a fixture removed by the test teardown.
            if DATA.exists():
                (DATA/'fake-manager-release').touch()
            if MODULE.exists():
                shutil.rmtree(MODULE)
            MANAGER.unlink(missing_ok=True)
            Path('/data/adb/apd').unlink(missing_ok=True)
            Path('/data/adb/magisk/magisk').unlink(missing_ok=True)
            if Path('/data/adb/magisk').exists():
                Path('/data/adb/magisk').rmdir()
            shutil.rmtree(DATA, ignore_errors=True)
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    main()
