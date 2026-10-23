#!/usr/bin/env python3
"""Actual CLI/daemon durable ACK, supersession and fault recovery, isolated root."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

import test_engine as engine
from test_cleanup_failures import command, released, lease_finished

DATA, BINARY = engine.DATA, engine.BINARY
ROOT = Path(__file__).resolve().parents[2]
QUEUE_ONLY = sys.argv[2]


def reply(*args, success=True):
    result = command(*args)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return json.loads(result.stdout)


def phase(revision=None):
    return reply('--apply-status', *([revision] if revision else []))


def wait_applied(revision):
    engine.wait_for(lambda: phase(revision)['status'] == 'applied', 'verified apply ACK ' + revision[:8])
    value = phase(revision)
    assert value['verified'] and value['applied_revision'] == value['desired_revision'] == revision, value
    return value


def save_config(target, text):
    before = reply('--config-read', target)
    result = subprocess.run([BINARY, '--config-save', target, before['revision']], input=text,
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    return json.loads(result.stdout)['revision']


def save_custom(text):
    return save_config('custom', text)


def main():
    assert os.geteuid() == 0 and not DATA.exists() and released()
    for table in engine.TABLES:
        assert 'ADK_NFQ_' not in engine.firewall(table, '-S')
    DATA.parent.mkdir(parents=True, exist_ok=True)
    daemon = None
    with tempfile.TemporaryDirectory(prefix='adk-apply-protocol-') as directory:
        base = Path(directory); programs = base / 'bin'; programs.mkdir()
        mode = base / 'fault'; mode.write_text('')
        entered = Path(str(mode) + '.entered'); release = Path(str(mode) + '.release')
        env = {**os.environ, 'PATH': str(programs) + os.pathsep + os.environ['PATH'],
               'ADK_FIREWALL_FAULT_FILE': str(mode), 'ADK_FIREWALL_REAL_4': shutil.which('iptables'),
               'ADK_FIREWALL_REAL_6': shutil.which('ip6tables')}
        for name in engine.TABLES:
            shutil.copy2(ROOT / 'tests/integration/iptables_fault.sh', programs / name)
            (programs / name).chmod(0o755)
        try:
            assert command('--install').returncode == 0
            first_hash = save_custom('old.example.com\n')
            saved = reply('--reload', 'custom', first_hash)
            assert saved['status'] == 'deferred' and phase(saved['revision'])['status'] == 'deferred'
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: engine.state() == 'running' or daemon.poll() is not None, 'ACK daemon start')
            assert daemon.poll() is None
            initial = phase(); assert initial['status'] == 'applied' and initial['verified']
            assert not Path('/data/system/packages.list').exists()
            previous_allow = reply('--config-read', 'allow')['revision']
            cache = DATA / 'app_uids.conf'
            old_cache = cache.read_bytes() if cache.exists() else None
            save_config('allow', 'com.example.unresolvable\n')
            assert reply('--apps-apply', 'allow', previous_allow, success=False)['status'] == 'superseded'
            assert reply('--apps-apply', success=False)['status'] == 'error_uid_resolution'
            assert (cache.read_bytes() if cache.exists() else None) == old_cache
            assert phase()['status'] == 'saved' and reply('--status')['iptables_active']
            empty_allow = save_config('allow', '')
            wait_applied(reply('--apps-apply', 'allow', empty_allow)['revision'])
            initial = phase()
            second_hash = save_custom('new.example.com\nsecond.example.com\n')
            current = phase()
            assert current['status'] == 'saved' and current['applied_revision'] == initial['applied_revision']
            state = reply('--status')
            assert state['iptables_active'] and state['rules_black'] == 1 and state['apply']['rules_black'] == 1
            assert reply('--reload', 'custom', first_hash, success=False)['status'] == 'superseded'
            queued = reply('--reload', 'custom', second_hash)
            assert queued['status'] == 'queued'
            wait_applied(queued['revision'])
            for family in (socket.AF_INET, socket.AF_INET6):
                engine.udp_dns(family, 'new.example.com', False)
                engine.udp_dns(family, 'old.example.com', True)

            # Desired scope differs from old effective routes after rollback.
            # --status must verify the latter, not falsely report lost routing.
            mode.write_text('switch\n')
            failed = reply('--scope', '1')
            engine.wait_for(lambda: phase(failed['revision'])['status'] == 'failed', 'failed apply result')
            state = reply('--status')
            assert state['scope'] == 1 and state['iptables_active'] and state['apply']['scope'] == 0, state
            assert state['apply']['verified'] and state['apply']['error']
            mode.write_text('')
            wait_applied(reply('--reload')['revision'])
            assert phase()['scope'] == 1
            engine.udp_dns(socket.AF_INET6, 'new.example.com', True)
            wait_applied(reply('--scope', '0')['revision'])

            # A later save while an actual tool is blocked must remain pending
            # after the older operation commits; querying its token is superseded.
            mode.write_text('hold\n')
            old = reply('--reload')
            engine.wait_for(entered.exists, 'held apply mutation')
            latest = reply('--mode', '1')
            assert phase(old['revision'])['status'] == 'superseded'
            mode.write_text(''); release.touch()
            current = wait_applied(latest['revision']); assert current['mode'] == 1

            # Durable queue recovery does not depend on signal delivery or the
            # WebUI staying open after command dispatch.
            (DATA / 'custom_rules.txt').write_text('recovered.example.com\n')
            queued_only = subprocess.run([QUEUE_ONLY, str(DATA)], capture_output=True, text=True, timeout=10)
            assert queued_only.returncode == 0, queued_only.stderr
            wait_applied(queued_only.stdout.strip())
            engine.udp_dns(socket.AF_INET6, 'recovered.example.com', False)
            # A file statistic cannot override verified in-memory rule counts.
            (DATA / 'rule_stats.conf').write_text('black=999\nwhite=999\n')
            assert reply('--status')['rules_black'] == reply('--status-fast')['rules_black'] == 1

            # Corruption/permission uncertainty is not green, and must prevent
            # changing routing inputs before a durable dirty marker exists.
            journal = DATA / 'apply.state'; good = journal.read_bytes()
            state_before = (DATA / 'state.conf').read_bytes()
            routes_before = {table: engine.firewall(table, '-S') for table in engine.TABLES}
            journal.write_text('corrupt receipt\n')
            assert command('--quic', 'off').returncode != 0
            assert (DATA / 'state.conf').read_bytes() == state_before
            assert not reply('--status')['apply']['verified']
            assert routes_before == {table: engine.firewall(table, '-S') for table in engine.TABLES}
            journal.write_bytes(good)
            error_file = DATA / 'engine_error.conf'; previous_error = error_file.read_bytes()
            error_file.write_text('code=apply_ack_failed\nmessage=unconfirmed directory sync\n')
            assert not reply('--status')['apply']['verified']
            assert command('--apply-status').returncode != 0
            error_file.write_bytes(previous_error)
            old_revision = phase()['applied_revision']
            assert command('--stop').returncode == 0
            daemon.communicate(timeout=10); assert daemon.returncode == 0; daemon = None
            assert not phase()['verified']
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: engine.state() == 'running' or daemon.poll() is not None, 'new identity ACK')
            assert daemon.poll() is None and phase()['applied_revision'] != old_revision

            # Lose the ACK lock only AFTER the candidate was captured. The
            # actual cutover may succeed, but no success ACK may be fabricated.
            entered.unlink(); release.unlink()
            mode.write_text('hold\n')
            unconfirmed = reply('--reload')
            engine.wait_for(entered.exists, 'ACK fault after candidate capture')
            with (DATA / '.apply.lock').open('a') as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                mode.write_text(''); release.touch()
                daemon.communicate(timeout=20)
                assert daemon.returncode != 0
            daemon = None
            failed_state = reply('--status')
            assert failed_state['last_error_code'] == 'apply_ack_failed' and not failed_state['running'], failed_state
            assert phase(unconfirmed['revision'])['status'] != 'applied' and released()
            print('real ACK protocol: deferred/saved/applied/failed, old actual routing, CAS, supersession, lost signal, counts, corruption, identity restart and ACK failure exit passed')
        finally:
            mode.write_text(''); release.touch()
            if daemon and daemon.poll() is None:
                daemon.terminate(); daemon.communicate(timeout=25)
            if DATA.exists():
                engine.wait_for(lease_finished, 'apply test command lease released', 35)
                final = command('--stop'); assert final.returncode == 0, (final.stdout, final.stderr)
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
