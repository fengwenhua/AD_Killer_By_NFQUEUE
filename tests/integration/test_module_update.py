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

ROOT = Path(__file__).resolve().parents[2]
DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
MANAGER = Path('/data/adb/ksud')
NAME = 'AD_Killer_By_NFQUEUE-v1.1.zip'
buffer = io.BytesIO()
with zipfile.ZipFile(buffer, 'w') as archive:
    archive.writestr('module.prop', 'id=AD_Killer_By_NFQUEUE\nversion=v1.1\nversionCode=110\n')
PACKAGE = buffer.getvalue()
DIGEST = hashlib.sha256(PACKAGE).hexdigest()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path.startswith('/api') and self.server.api_status != 200:
            self.send_response(self.server.api_status)
            self.end_headers()
            self.wfile.write(b'{"message":"API rate limit exceeded"}')
            return
        if self.path.endswith('update.json'):
            content = json.dumps({'version': 'v1.1'}).encode()
        elif self.path == '/api?per_page=1':
            content = json.dumps([{'tag_name': 'v1.1', 'body': 'Notes from GitHub Release'}]).encode()
        elif self.path.endswith('.sha256'):
            content = (('0'*64 if self.server.corrupt else DIGEST)+'  '+NAME+'\n').encode()
        elif self.path.endswith('.zip'):
            content = PACKAGE
        else:
            content = json.dumps({'tag_name': 'v1.1', 'draft': False, 'prerelease': False}).encode()
        self.send_response(200)
        self.end_headers()
        self.wfile.write(content)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires disposable Linux/root'
    assert not MANAGER.exists() and not Path('/data/adb/apd').exists() and not Path('/data/adb/magisk').exists(), 'refusing existing root manager'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.corrupt = False
    server.api_status = 200
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_port}'
    with tempfile.TemporaryDirectory(prefix='adk_module_update_cli_') as temporary:
        binary = Path(temporary)/'AD_Killer_By_NFQUEUE'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-DMODULE_VERSION="1.0"', '-DMODULE_VERSION_CODE=100',
                        '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"', '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                        f'-DADK_NFQ_TEST_RELEASE_REPO="{base}"', f'-DADK_NFQ_TEST_RELEASE_API="{base}/api"',
                        '-I'+str(ROOT/'src'), *map(str, sorted((ROOT/'src').glob('*.cpp'))), '-ldl', '-lz', '-o', str(binary)], check=True)

        def command(*args, success=True):
            result = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=25)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout

        def job(action, success=True):
            data = json.loads(command(action, '1.1'))
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
            assert json.loads(command('--module-release-notes')) == [{'tag_name': 'v1.1', 'body': 'Notes from GitHub Release'}]
            command('--module-release-notes', 'unexpected', success=False)
            for status in (403, 429):
                server.api_status = status
                for args in (('--module-release-notes',), ('--module-update-check', 'preview')):
                    failure = json.loads(command(*args, success=False))
                    assert failure['http_status'] == status and '匿名请求配额' in failure['message']
                assert 'update' in json.loads(command('--module-update-check', 'stable')), 'API quota should not break static stable metadata'
            server.api_status = 200
            command('--module-update-download', '1.0', success=False)
            command('--module-update-download', '../bad', success=False)
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
            print('real module-update CLI/jobs, download, tamper rejection, fake manager argv and reboot state passed')
        finally:
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
