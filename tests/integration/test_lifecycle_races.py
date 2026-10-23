#!/usr/bin/env python3
"""Controlled production CLI/kernel races in an explicitly disposable Linux."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

import test_engine as engine

ROOT = Path(__file__).resolve().parents[2]
DATA, BINARY = engine.DATA, engine.BINARY


def run(*args, env=None):
    return subprocess.run([BINARY, *args], env=env, text=True, capture_output=True, timeout=20)


def reply(result):
    assert result.returncode == 0, (result.stdout, result.stderr)
    return json.loads(result.stdout.splitlines()[-1])


def status():
    return reply(run('--status'))


def gate(base, name):
    directory = base / name
    directory.mkdir()
    shutil.copy2(ROOT / 'tests/integration/lifecycle_gate.sh', directory / name)
    (directory / name).chmod(0o755)
    return directory, {**os.environ, 'PATH': str(directory) + os.pathsep + os.environ['PATH'],
                       'ADK_LIFECYCLE_GATE': str(directory)}


def clean_network():
    assert not (DATA / 'daemon.pid').exists()
    queues = Path('/proc/net/netfilter/nfnetlink_queue')
    assert not queues.exists() or not {int(row.split()[0]) for row in queues.read_text().splitlines()} & {0, 1}
    for table in engine.TABLES:
        assert 'ADK_NFQ_' not in engine.firewall(table, '-S')


def launcher_exited(marker):
    pid = int(marker.read_text())
    try:
        return not Path(f'/proc/{pid}/cmdline').read_bytes()
    except FileNotFoundError:
        return True


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    DATA.parent.mkdir(parents=True, exist_ok=True)
    children = []
    with tempfile.TemporaryDirectory(prefix='adk-real-lifecycle-') as directory:
        base = Path(directory)
        try:
            installed = run('--install')
            assert installed.returncode == 0, (installed.stdout, installed.stderr)
            (DATA / 'custom_rules.txt').write_text('ads.example.com\n')
            # Interpose only nohup to pause AFTER production --start dispatch,
            # before its background reentry. No CLI/engine/network code patched.
            for boot in (False, True):
                case = base / ('boot' if boot else 'manual'); case.mkdir()
                launch_gate, env = gate(case, 'nohup')
                if boot:
                    reply(run('--autostart', 'on'))
                    # Persisted running policy from a previous boot; there is no
                    # daemon now. Exercise the actual service script as well.
                    conf = DATA / 'state.conf'
                    conf.write_text(conf.read_text().replace('enabled=false', 'enabled=true').replace('manual_stop=true', 'manual_stop=false'))
                    shutil.copy2(ROOT / 'module/service.sh', case / 'service.sh')
                    (case / 'AD_Killer_By_NFQUEUE').symlink_to(BINARY)
                    shutil.copy2(ROOT / 'tests/integration/boot_ready.sh', launch_gate / 'getprop')
                    (launch_gate / 'getprop').chmod(0o755)
                    service = subprocess.run(['sh', str(case / 'service.sh')], env=env, capture_output=True, timeout=20)
                    assert service.returncode == 0, service.stderr
                else:
                    assert reply(run('--start', env=env))['status'] == 'starting'
                engine.wait_for(lambda: (launch_gate / 'entered').exists(), 'dispatched background request')
                clean_network()
                assert reply(run('--stop'))['status'] == 'stopped'
                (launch_gate / 'release').touch()
                # Wait for the exact interposed launcher to exit (zombies have
                # empty cmdline). Never signal an unrelated/reused PID.
                engine.wait_for(lambda: launcher_exited(launch_gate / 'entered'), 'cancelled late launch exits')
                time.sleep(.1)
                clean_network()
                assert not status()['running'] and status()['runtime'] == 'stopped'
                assert 'manual_stop=true' in (DATA / 'state.conf').read_text()

            # Independent windows: a newer start wins while the older dispatch
            # is delayed; releasing the old request must not create an instance.
            windows = base / 'windows'; windows.mkdir()
            launch_gate, env = gate(windows, 'nohup')
            reply(run('--start', env=env))
            engine.wait_for(lambda: (launch_gate / 'entered').exists(), 'first window dispatch')
            reply(run('--start'))
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'second window startup')
            current_pid = status()['daemon_pid']
            (launch_gate / 'release').touch()
            engine.wait_for(lambda: launcher_exited(launch_gate / 'entered'), 'superseded first window exits')
            assert status()['daemon_pid'] == current_pid and status()['iptables_active']
            queues = Path('/proc/net/netfilter/nfnetlink_queue').read_text().splitlines()
            assert len([row for row in queues if int(row.split()[0]) in (0, 1)]) == 2
            reply(run('--stop')); clean_network()

            # Stop an actual NFQUEUE daemon. Pause the stop CLI's post-exit
            # cleanup tool, register a new start, then release cleanup. The new
            # instance must not publish PID/queues/routes before the old cleanup.
            assert reply(run('--start'))['status'] == 'starting'
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'], 'first real daemon')
            old_pid = status()['daemon_pid']
            cleanup_gate, env = gate(base, 'iptables')
            env['ADK_LIFECYCLE_REAL_IPTABLES'] = shutil.which('iptables')
            stop = subprocess.Popen([BINARY, '--stop'], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(stop)
            engine.wait_for(lambda: (cleanup_gate / 'entered').exists(), 'old stop post-exit cleanup barrier')
            assert not (DATA / 'daemon.pid').exists()
            previous_intent = (DATA / 'lifecycle.intent').read_bytes()
            start = subprocess.Popen([BINARY, '--start'], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(start)
            engine.wait_for(lambda: (DATA / 'lifecycle.intent').read_bytes() != previous_intent, 'new start intent registered')
            time.sleep(.1)
            assert start.poll() is None, 'new start did not wait for old cleanup'
            assert not (DATA / 'daemon.pid').exists()
            (cleanup_gate / 'release').touch()
            out, err = stop.communicate(timeout=20); assert stop.returncode == 0, (out, err)
            out, err = start.communicate(timeout=20); assert start.returncode == 0, (out, err)
            engine.wait_for(lambda: status()['running'] and status()['iptables_active'] and status()['daemon_pid'] != old_pid,
                            'new daemon owns real queues/routes after old stop')
            for family in (socket.AF_INET, socket.AF_INET6):
                engine.udp_dns(family, 'ads.example.com', False)
                engine.udp_dns(family, 'safe.example.org', True)
            reply(run('--stop'))
            clean_network()
            print('real CLI/kernel barriers: manual/boot pause cancellation, latest window and old-stop/new-start ownership passed')
        finally:
            for marker in base.glob('**/entered'):
                (marker.parent / 'release').touch()
            run('--stop')
            for child in children:
                if child.poll() is None:
                    child.communicate(timeout=20)
            if DATA.exists():
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
