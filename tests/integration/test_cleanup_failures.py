#!/usr/bin/env python3
"""Actual kernel cleanup faults and crashed-parent lock leases, disposable Linux."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

import test_engine as engine

ROOT = Path(__file__).resolve().parents[2]
DATA, BINARY = engine.DATA, engine.BINARY


def command(*args, env=None):
    return subprocess.run([BINARY, *args], env=env, capture_output=True, text=True, timeout=30)


def value(result):
    return json.loads(result.stdout.splitlines()[-1])


def status():
    result = command('--status')
    assert result.returncode == 0, result.stderr
    return value(result)


def released():
    queues = Path('/proc/net/netfilter/nfnetlink_queue')
    return not queues.exists() or not {int(row.split()[0]) for row in queues.read_text().splitlines()} & {0, 1}


def lease_finished():
    with (DATA / '.route.lock').open('a') as descriptor:
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    assert released(), 'existing listeners, refusing cleanup test'
    for table in engine.TABLES:
        assert 'ADK_NFQ_' not in engine.firewall(table, '-S'), 'existing module rules, refusing cleanup test'
    DATA.parent.mkdir(parents=True, exist_ok=True)
    daemon = None
    canaries = []
    foreign = ('-p', 'udp', '--dport', '49560', '-j', 'DROP')
    with tempfile.TemporaryDirectory(prefix='adk-kernel-cleanup-') as directory:
        base = Path(directory)
        programs = base / 'bin'; programs.mkdir()
        mode = base / 'fault'; mode.write_text('')
        env = {**os.environ, 'PATH': str(programs) + os.pathsep + os.environ['PATH'],
               'ADK_FIREWALL_FAULT_FILE': str(mode), 'ADK_FIREWALL_REAL_4': shutil.which('iptables'),
               'ADK_FIREWALL_REAL_6': shutil.which('ip6tables')}
        for name in engine.TABLES:
            shutil.copy2(ROOT / 'tests/integration/iptables_fault.sh', programs / name)
            (programs / name).chmod(0o755)
        try:
            installed = command('--install'); assert installed.returncode == 0, installed.stderr
            (DATA / 'custom_rules.txt').write_text('ads.example.com\n')
            for table in engine.TABLES:
                engine.firewall(table, '-A', 'OUTPUT', *foreign); canaries.append(table)
            for fault in ('delete', 'query', 'empty', 'timeout', 'lock'):
                if fault == 'timeout':
                    Path(str(mode) + '.timeout.release').unlink(missing_ok=True)
                for table in engine.TABLES:
                    engine.firewall(table, '-N', 'ADK_NFQ_OUT_A')
                    engine.firewall(table, '-A', 'OUTPUT', '-j', 'ADK_NFQ_OUT_A')
                mode.write_text(fault + '\n')
                descriptor = None
                try:
                    if fault == 'lock':
                        descriptor = (DATA / '.route.lock').open('a')
                        fcntl.flock(descriptor, fcntl.LOCK_EX)
                    started = time.monotonic()
                    failed = command('--stop', env=env); reply = value(failed)
                    assert failed.returncode != 0 and reply['status'].startswith('error_cleanup_'), reply
                    assert reply['cleanup']['routes'] == ('partial' if fault == 'delete' else 'unknown'), reply
                    assert reply['cleanup']['process_gone'] and reply['cleanup']['queues'] == 'released'
                    assert (DATA / 'runtime.state').read_text() == 'error'
                    assert time.monotonic() - started < 20, 'cleanup exceeded its total budget'
                    for table in engine.TABLES:
                        engine.firewall(table, '-C', 'OUTPUT', *foreign)
                    assert 'ADK_NFQ_OUT_A' in engine.firewall('ip6tables', '-S')
                finally:
                    if descriptor:
                        descriptor.close()
                mode.write_text('')
                Path(str(mode) + '.timeout.release').touch()
                engine.wait_for(lease_finished, 'failed command lease released')
                retried = command('--stop'); assert retried.returncode == 0, (retried.stdout, retried.stderr)
                assert value(retried)['cleanup']['routes'] == 'clean'
                assert not (DATA / 'engine_error.conf').read_text()
                for table in engine.TABLES:
                    assert 'ADK_NFQ_' not in engine.firewall(table, '-S')

            # A daemon's own failed shutdown must keep fault/exit status, not
            # merely rely on the stop CLI to notice the residual empty chains.
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'fault-test daemon start')
            mode.write_text('delete\n')
            failed = command('--stop', env=env)
            assert failed.returncode != 0 and value(failed)['cleanup']['routes'] == 'partial'
            daemon.communicate(timeout=20); assert daemon.returncode != 0
            daemon = None
            mode.write_text('')
            assert command('--stop').returncode == 0 and released()

            # A blocked cleanup must not leave unconsumed NFQUEUE listeners.
            # While the real IPv6 tool waits, new unclassified DNS bypasses.
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'timeout-shutdown daemon start')
            Path(str(mode) + '.timeout.release').unlink(missing_ok=True)
            mode.write_text('timeout\n')
            stopping = subprocess.Popen([BINARY, '--stop'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                engine.wait_for(released, 'listeners released before slow cleanup', 2)
                assert stopping.poll() is None, 'slow cleanup was not exercised'
                for family in (engine.socket.AF_INET, engine.socket.AF_INET6):
                    engine.udp_dns(family, 'safe.example.org', True)
                out, err = stopping.communicate(timeout=25)
                assert stopping.returncode != 0, (out, err)
            finally:
                mode.write_text('')
                Path(str(mode) + '.timeout.release').touch()
                if stopping.poll() is None:
                    stopping.communicate(timeout=25)
            daemon.communicate(timeout=10); assert daemon.returncode != 0
            daemon = None
            engine.wait_for(lease_finished, 'timed-out shutdown command lease released')
            retried = command('--stop'); assert retried.returncode == 0, retried.stdout

            # The original O03 fault-state observation becomes a real kernel
            # regression here; mixed-generation consistency is still O03 work.
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'rollback-failure daemon start')
            mode.write_text('rollback\n')
            assert command('--reload').returncode == 0
            daemon.communicate(timeout=20); assert daemon.returncode != 0
            daemon = None
            failure = status()
            assert not failure['running'] and failure['runtime'] == 'error' and failure['last_error_code'] == 'reload_rollback_failed', failure
            assert released()
            mode.write_text('')
            assert command('--stop').returncode == 0

            # Crash while an actual mutation command is waiting. Its inherited
            # route-lock lease must outlive the daemon and block any new bind.
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'lease-test daemon start')
            mode.write_text('hold\n')
            assert command('--reload').returncode == 0
            entered = Path(str(mode) + '.entered')
            release = Path(str(mode) + '.release')
            engine.wait_for(entered.exists, 'real mutation command holds route lease')
            daemon.kill(); daemon.communicate(timeout=5); daemon = None
            with (DATA / '.route.lock').open('a') as descriptor:
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    pass
                else:
                    raise AssertionError('crashed daemon dropped its live command ownership lease')
            failed = command('--stop')
            assert failed.returncode != 0 and value(failed)['cleanup']['routes'] == 'unknown'
            assert command('--start').returncode == 0
            engine.wait_for(lambda: status()['runtime'] == 'error' and not status()['running'] and released(), 'new daemon refuses leased routes')
            assert not status()['running'] and released(), 'new listener bound before the old command finished'
            mode.write_text('')
            release.touch()
            engine.wait_for(lease_finished, 'orphan command completes before recovery')
            recovered = command('--stop'); assert recovered.returncode == 0, recovered.stdout
            assert released()
            for table in engine.TABLES:
                assert 'ADK_NFQ_' not in engine.firewall(table, '-S')
                engine.firewall(table, '-C', 'OUTPUT', *foreign)
            print('real kernel cleanup: IPv6 failures, empty/failed queries, total timeout, locks, daemon fault and SIGKILL command lease passed')
        finally:
            mode.write_text(''); Path(str(mode) + '.release').touch(); Path(str(mode) + '.timeout.release').touch()
            if daemon and daemon.poll() is None:
                daemon.terminate(); daemon.communicate(timeout=25)
            if DATA.exists():
                # Never unlink an active lease inode. On a failed test, preserve
                # evidence rather than letting an orphan affect the next test.
                engine.wait_for(lease_finished, 'cleanup test command lease released', 35)
                final = command('--stop')
                assert final.returncode == 0, (final.stdout, final.stderr)
            for table in canaries:
                engine.firewall(table, '-D', 'OUTPUT', *foreign)
            if DATA.exists():
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
