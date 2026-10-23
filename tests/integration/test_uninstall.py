#!/usr/bin/env python3
"""Full uninstall only in disposable Linux/root; no connected-device operations."""
import fcntl
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
MODULE = Path('/data/adb/modules/AD_Killer_By_NFQUEUE')
GATE = DATA.parent/'.AD_Killer_By_NFQUEUE.operations.lock'
BINARY = str(MODULE/'AD_Killer_By_NFQUEUE')
MARKER = MODULE/'.uninstalling'
STARTED = threading.Event()
RELEASE = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Length', '1000')
        self.end_headers()
        self.wfile.flush()
        STARTED.set()
        RELEASE.wait(10)


def command(*args, success=True, env=None):
    result = subprocess.run([BINARY, *args], env=env, capture_output=True, text=True, timeout=45)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    if args == ('--install',) or (args == ('--sources-list',) and not result.stdout):
        return {}
    return json.loads(result.stdout)


def routes_clean():
    for tool in ('iptables', 'ip6tables'):
        result = subprocess.run([tool, '-w', '1', '-t', 'mangle', '-S'], capture_output=True, text=True, check=True)
        assert 'ADK_NFQ_' not in result.stdout, result.stdout
    path = Path('/proc/net/netfilter/nfnetlink_queue')
    assert not [row for row in path.read_text().splitlines() if row.split()[0] in ('0', '1')]


def main():
    assert os.geteuid() == 0 and sys.platform == 'linux'
    assert not DATA.exists() and not MODULE.exists(), 'refusing existing module data'
    MODULE.mkdir(parents=True)
    (MODULE/'module.prop').write_text('id=AD_Killer_By_NFQUEUE\nversion=v1.2.6\n')
    shutil.copyfile(sys.argv[1], BINARY)
    Path(BINARY).chmod(0o755)
    daemon = writer = None
    mounted = False
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='adk_uninstall_') as temporary:
        outside = Path(temporary)/'outside'
        outside.mkdir()
        sentinel = outside/'sentinel'
        sentinel.write_text('outside unchanged')

        def install():
            MARKER.unlink(missing_ok=True)  # Test-only simulation of a fresh manager install.
            command('--install')

        def removed():
            assert not DATA.exists() and not GATE.exists()
            assert not (DATA.parent/'.AD_Killer_By_NFQUEUE.install.lock').exists()
            assert MARKER.read_text() == 'ADK_UNINSTALL_1\n'
            assert command('--start', success=False)['status'] == 'error_maintenance'
            command('--sources-list', success=False)
            command('--jobs-list')
            assert not DATA.exists() and not GATE.exists(), 'late call recreated removed data or gate'
            assert sentinel.read_text() == 'outside unchanged'
            routes_clean()

        try:
            install()
            # Legacy running records cannot prove installer completion.
            unknown = DATA/'update_jobs/update-123-456.state'
            unknown.write_text('running:1\n')
            before = (DATA/'state.conf').read_bytes()
            reply = command('--uninstall', success=False)
            assert reply['status'] == 'uninstall_busy' and not MARKER.exists(), reply
            assert (DATA/'state.conf').read_bytes() == before
            unknown.unlink()
            # Even a dead owner and an idle lease do not clear a same-boot
            # installer: managers can close inherited FDs and keep installing.
            boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            fields = {'id': '123-456', 'type': 'install', 'target': '9.9', 'channel': 'stable',
                      'phase': 'installing', 'reason': '', 'status': 'running', 'pid': '999999999',
                      'exit': '-1', 'boot': boot, 'started': '1', 'device': '1', 'inode': '1',
                      'created': '1', 'updated': '1', 'format': '2'}
            unknown.write_text('ADK_JOB_2\n'+''.join(f'{key}={value}\n' for key, value in fields.items()))
            (DATA/'update_jobs/update-123-456.lease').touch()
            reply = command('--uninstall', success=False)
            assert reply['status'] == 'uninstall_busy' and not MARKER.exists(), reply
            assert (DATA/'state.conf').read_bytes() == before
            unknown.unlink()

            # Orphan leases still matter even if there is no state/index.
            (DATA/'update_jobs/update-123-456.lease').touch()
            lease = (DATA/'update_jobs/update-123-456.lease').open('r')
            fcntl.flock(lease, fcntl.LOCK_SH | fcntl.LOCK_NB)
            reply = command('--uninstall', success=False)
            assert 'lock' in reply['message'] and DATA.exists(), reply
            lease.close()
            assert command('--uninstall')['status'] == 'uninstalled'
            removed()
            assert command('--uninstall')['status'] == 'uninstalled', 'repeat must be idempotent'
            removed()

            install()
            # A legacy process with no operation lease can still hold a writable
            # FD. Its argv has no data path, so this exercises fdinfo inspection.
            held = (DATA/'legacy-writer').open('w')
            writer = subprocess.Popen(['sleep', '30'], pass_fds=(held.fileno(),))
            held.close()
            reply = command('--uninstall', success=False)
            assert '写入句柄' in reply['message'] and DATA.exists(), reply
            writer.terminate(); writer.wait(); writer = None
            command('--uninstall'); removed()

            install()
            # Failed IPv6 inventory is unknown, not permission to erase data.
            tools = Path(temporary)/'tools'
            tools.mkdir()
            fake6 = tools/'ip6tables'
            fake6.write_text('#!/bin/sh\nexit 1\n')
            fake6.chmod(0o755)
            reply = command('--uninstall', success=False,
                            env={**os.environ, 'PATH': str(tools)+os.pathsep+os.environ['PATH']})
            assert reply['status'] == 'uninstall_incomplete' and DATA.exists() and MARKER.exists(), reply
            assert (DATA/'log/engine.log').exists()
            command('--uninstall'); removed()

            install()
            # Cleanup succeeding earlier is insufficient: a query that fails
            # only during the post-drain proof must still preserve all data.
            counter = Path(temporary)/'queries'
            fake6.write_text('#!/bin/sh\ncount=0\n'
                             '[ ! -f "$ADK_UNINSTALL_TEST_COUNTER" ] || read -r count < "$ADK_UNINSTALL_TEST_COUNTER"\n'
                             'count=$((count + 1))\nprintf "%s\\n" "$count" > "$ADK_UNINSTALL_TEST_COUNTER"\n'
                             '[ "$count" -lt 2 ] || exit 1\nexec "$ADK_UNINSTALL_TEST_IP6" "$@"\n')
            reply = command('--uninstall', success=False,
                            env={**os.environ, 'PATH': str(tools)+os.pathsep+os.environ['PATH'],
                                 'ADK_UNINSTALL_TEST_COUNTER': str(counter), 'ADK_UNINSTALL_TEST_IP6': shutil.which('ip6tables')})
            assert '排空后' in reply['message'] and DATA.exists(), reply
            command('--uninstall'); removed()

            install()
            # A same-device bind mount must be rejected before data mutation.
            mountpoint = DATA/'mounted'
            mountpoint.mkdir()
            mount = subprocess.run(['mount', '--bind', str(outside), str(mountpoint)], capture_output=True, text=True)
            if mount.returncode == 0:
                mounted = True
                before = (DATA/'state.conf').read_bytes()
                reply = command('--uninstall', success=False)
                assert '挂载' in reply['message'] and not MARKER.exists(), reply
                assert (DATA/'state.conf').read_bytes() == before and sentinel.exists()
                subprocess.run(['umount', str(mountpoint)], check=True)
                mounted = False
            else:
                print('bind-mount case unavailable in this namespace:', mount.stderr.strip())
            mountpoint.rmdir()

            # Exact symlink and hardlink entries disappear, external bytes stay.
            (DATA/'outside-link').symlink_to(outside, target_is_directory=True)
            os.link(sentinel, DATA/'outside-hardlink')
            (DATA/'custom_rules.txt').write_text('ads.example.org\n')
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            for _ in range(200):
                if (DATA/'runtime.state').exists() and (DATA/'runtime.state').read_text().strip() == 'running':
                    break
                assert daemon.poll() is None
                time.sleep(.05)
            else:
                raise AssertionError('daemon did not start')
            command('--source-test', f'http://127.0.0.1:{server.server_port}/idle')
            assert STARTED.wait(5)
            started = time.monotonic()
            reply = command('--uninstall')
            assert reply['status'] == 'uninstalled' and time.monotonic() - started < 15, reply
            assert daemon.wait(timeout=5) == 0
            daemon = None
            RELEASE.set()
            removed()
            time.sleep(.2)
            assert not DATA.exists(), 'cancelled worker recreated removed data'
            install()
            STARTED.clear(); RELEASE.clear()
            (DATA/'update_sources.txt').write_text('# disabled: builtin:hagezi-pro-minus-fake\n'
                                                  f'http://127.0.0.1:{server.server_port}/idle\n')
            command('--update-start')
            assert STARTED.wait(5)
            assert command('--uninstall')['status'] == 'uninstalled'
            RELEASE.set(); removed()
            print('full uninstall: installer/legacy barriers, orphan leases, writable FDs, mount/symlink isolation, '
                  'route-query failure, running daemon + idle sync/source cancellation, exact route/queue release, '
                  'repeat and no late recreation passed')
        finally:
            RELEASE.set(); server.shutdown(); server.server_close()
            if writer and writer.poll() is None:
                writer.terminate(); writer.wait()
            if daemon and daemon.poll() is None:
                daemon.terminate(); daemon.wait(timeout=15)
            if mounted:
                subprocess.run(['umount', str(DATA/'mounted')], check=True)
            if DATA.exists():
                if (DATA/'outside-link').is_symlink():
                    (DATA/'outside-link').unlink()
                shutil.rmtree(DATA)
            shutil.rmtree(MODULE)


if __name__ == '__main__':
    main()
