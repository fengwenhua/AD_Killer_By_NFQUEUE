"""Real CLI, revision ACK and dual-stack traffic; synthetic Android catalog.

Disposable Linux/root only. Uses the production 30 s polling interval.
"""
import gzip
import json
import os
import shutil
import socket
import subprocess
import sys
import time

from package_manager import PackageManager
from test_engine import BINARY, DATA, command, state, wait_for, reload_command
from test_policy import uid_request, wait_policy


def phase():
    return json.loads(command(BINARY, '--apply-status'))


def wait_revision(previous, status='applied'):
    def changed():
        current = phase()
        return current['status'] == status and current['desired_revision'] != previous
    wait_for(changed, 'automatic application identity ' + status, timeout=42)
    return phase()


def save_allow(text):
    revision = json.loads(command(BINARY, '--config-read', 'allow'))['revision']
    result = subprocess.run([BINARY, '--config-save', 'allow', revision], input=text,
                            capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, (result.stdout, result.stderr)
    return json.loads(result.stdout)['revision']


def both(uid, allowed):
    for family in (socket.AF_INET, socket.AF_INET6):
        uid_request(family, uid, allowed)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    packages = PackageManager()
    daemon = saving = None
    try:
        command(BINARY, '--install')
        (DATA / 'hosts_rules.gz').write_bytes(gzip.compress(b'# valid empty category\n'))
        (DATA / 'adguard_rules.gz').write_bytes(gzip.compress(b'ads.example.com\n'))
        (DATA / 'app_whitelist.txt').write_text('com.example.app\n')
        # A valid-looking cache must not authorize a stale/reused UID at start.
        cache = DATA / 'app_uids.conf'
        revision = json.loads(command(BINARY, '--config-read', 'allow'))['revision']
        cache.write_text('# packages_revision=' + revision + '\n10009 com.example.app\n')
        packages.set('package:com.example.app uid:10001,1010001\npackage:com.example.shared uid:10001\n')
        daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'])
        wait_for(lambda: state() == 'running' or daemon.poll() is not None, 'application lifecycle daemon')
        assert daemon.poll() is None and state() == 'running'
        wait_policy(2, 1, 'fresh application identity at startup')
        both(10001, True)
        both(10009, False)
        both(1010001, False)  # legacy package-only selection is primary-user only
        saved = json.loads(reload_command('--apps-apply'))
        assert saved['shared_package_count'] == 1 and saved['uninstalled_count'] == 0

        # Installing an unrelated app must not write/reload an unchanged policy.
        unchanged = phase()['desired_revision']
        before_queries = packages.count()
        before_journal = (DATA / 'apply.state').stat()
        packages.set('package:com.example.app uid:10001,1010001\npackage:com.example.shared uid:10001\n'
                     'package:com.example.unrelated uid:10005\n')
        wait_for(lambda: packages.count() > before_queries, 'production periodic catalog check', timeout=36)
        time.sleep(.5)
        assert phase()['desired_revision'] == unchanged
        after = (DATA / 'apply.state').stat()
        assert (after.st_ino, after.st_mtime_ns) == (before_journal.st_ino, before_journal.st_mtime_ns)

        # Reinstall and reuse: only the selected package's new UID is bypassed.
        packages.set('package:com.example.app uid:10002,1010001\npackage:com.example.reused uid:10001\n')
        current = wait_revision(unchanged)
        assert current['verified']
        both(10001, False)
        both(10002, True)
        pid = json.loads(command(BINARY, '--status'))['daemon_pid']
        command(BINARY, '--start')  # stale hint must not restart a healthy owner
        assert json.loads(command(BINARY, '--status'))['daemon_pid'] == pid

        # Owner uninstall with clone still present must not resolve the clone
        # into its appId, or continue bypassing an unrelated reused owner UID.
        packages.set('package:com.example.app uid:1010001\npackage:com.example.reused uid:10002\n')
        current = wait_revision(current['desired_revision'])
        both(10002, False)
        assert (DATA / 'app_whitelist.txt').read_text() == 'com.example.app\n'
        saved = json.loads(reload_command('--apps-apply'))
        assert saved['uninstalled_count'] == 1 and saved['shared_package_count'] == 0

        packages.set('package:com.example.app uid:10003\n')
        reload_command('--reload')
        both(10003, True)
        current = phase()
        old_cache = cache.read_bytes()
        # A partial/malformed query is unknown, never an empty app selection.
        packages.set('package:com.example.app uid:10003\nunexpected partial error\n')
        failed = wait_revision(current['desired_revision'], 'failed')
        assert failed['verified'] and failed['applied_revision'] == current['applied_revision']
        both(10003, True)
        result = subprocess.run([BINARY, '--apps-apply'], capture_output=True, text=True, timeout=6)
        assert result.returncode != 0 and json.loads(result.stdout)['status'] == 'error_uid_resolution'
        assert cache.read_bytes() == old_cache
        # Recovery with the same UID must also clear the failed receipt.
        packages.set('package:com.example.app uid:10003\n')
        assert wait_revision(failed['desired_revision'])['verified']

        # A slow Binder call cannot lock out a later save; the old save must
        # refuse to overwrite the newer selection/cache after the query ends.
        stalled = packages.root / 'stall'
        stalled.touch()
        queries = packages.count()
        saving = subprocess.Popen([BINARY, '--apps-apply'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        wait_for(lambda: packages.count() > queries, 'stalled save query')
        save_allow('com.example.newer\n')
        stalled.unlink()
        stdout, stderr = saving.communicate(timeout=6)
        assert saving.returncode != 0 and json.loads(stdout)['status'] == 'superseded', (stdout, stderr)
        assert cache.read_bytes() == old_cache
        packages.set('package:com.example.newer uid:10004\n')
        reload_command('--apps-apply')
        both(10004, True)

        # Cancel an owner query on pause; no orphan Binder child/group or
        # unconsumed queue may prolong the normal daemon stop budget.
        stalled.touch()
        queries = packages.count()
        wait_for(lambda: packages.count() > queries, 'stalled owner query', timeout=36)
        started = time.monotonic()
        command(BINARY, '--stop')
        assert daemon.wait(timeout=6) == 0 and time.monotonic() - started < 3
        print('application lifecycle: fresh startup, shared/clone boundaries, periodic reinstall/reuse/uninstall, '
              'unknown preservation/recovery, unchanged-catalog idle, save supersession and query cancellation passed')
    except BaseException:
        log = DATA / 'log/engine.log'
        if log.exists(): print(log.read_text()[-16000:], file=sys.stderr)
        raise
    finally:
        if saving is not None and saving.poll() is None:
            saving.terminate(); saving.communicate(timeout=6)
        if daemon is not None and daemon.poll() is None:
            daemon.terminate(); daemon.wait(timeout=10)
        packages.close()
        if DATA.exists(): shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
