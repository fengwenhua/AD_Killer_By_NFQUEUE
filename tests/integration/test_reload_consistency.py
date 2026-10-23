#!/usr/bin/env python3
"""Real dual-stack cutover/rollback and existing-flow boundary; disposable root."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile

import test_engine as engine
from test_cleanup_failures import command, released, lease_finished

DATA, BINARY = engine.DATA, engine.BINARY
ROOT = Path(__file__).resolve().parents[2]


def main():
    assert os.geteuid() == 0 and not DATA.exists() and released()
    for table in engine.TABLES:
        assert 'ADK_NFQ_' not in engine.firewall(table, '-S')
    DATA.parent.mkdir(parents=True, exist_ok=True)
    daemon = None
    with tempfile.TemporaryDirectory(prefix='adk-reload-consistency-') as directory:
        base = Path(directory)
        programs = base / 'bin'; programs.mkdir()
        mode = base / 'fault'; mode.write_text('')
        release = Path(str(mode) + '.release')
        env = {**os.environ, 'PATH': str(programs) + os.pathsep + os.environ['PATH'],
               'ADK_FIREWALL_FAULT_FILE': str(mode), 'ADK_FIREWALL_REAL_4': shutil.which('iptables'),
               'ADK_FIREWALL_REAL_6': shutil.which('ip6tables')}
        for name in engine.TABLES:
            shutil.copy2(ROOT / 'tests/integration/iptables_fault.sh', programs / name)
            (programs / name).chmod(0o755)
        try:
            assert command('--install').returncode == 0
            custom = DATA / 'custom_rules.txt'
            custom.write_text('ads.example.com\n')
            daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'], env=env,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            log = DATA / 'log/engine.log'
            engine.wait_for(lambda: engine.state() == 'running' or daemon.poll() is not None, 'reload daemon start')
            assert daemon.poll() is None
            original = (DATA / 'rule_stats.conf').read_bytes()
            custom.write_text('new.example.com\nother.example.com\n')
            mode.write_text('switch\n')
            assert command('--reload').returncode == 0
            engine.wait_for(lambda: '已保留旧规则和路由' in log.read_text(), 'IPv6 switch failure with successful rollback')
            engine.wait_for(lease_finished, 'rollback lease completed')
            assert daemon.poll() is None and (DATA / 'rule_stats.conf').read_bytes() == original
            for family in (socket.AF_INET, socket.AF_INET6):
                engine.udp_dns(family, 'ads.example.com', False)
                engine.udp_dns(family, 'new.example.com', True)

            # Establish a PASS-marked flow before changing the policy. We do
            # not flush global conntrack or pretend a past TLS handshake can
            # be reconstructed; existing classified flows keep their verdict.
            mode.write_text('')
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(('127.0.0.1', 80)); listener.listen(1)
                with socket.create_connection(('127.0.0.1', 80), timeout=2) as client:
                    peer, _ = listener.accept()
                    with peer:
                        peer.settimeout(2)
                        payload = b'GET / HTTP/1.1\r\nHost: new.example.com\r\n\r\n'
                        client.sendall(payload); assert peer.recv(4096) == payload
                        mode.write_text('hold\n')
                        assert command('--reload').returncode == 0
                        engine.wait_for(Path(str(mode) + '.entered').exists, 'held actual firewall transaction')
                        # Consumers remain live during slow installation. Even
                        # old-policy blocks bypass, without a permanent mark.
                        for family in (socket.AF_INET, socket.AF_INET6):
                            engine.udp_dns(family, 'ads.example.com', True)
                        mode.write_text(''); release.touch()
                        engine.wait_for(lambda: '热重载已完成' in log.read_text(), 'policy and both receive queues committed')
                        assert json.loads(command('--status').stdout)['iptables_active']
                        client.sendall(payload); assert peer.recv(4096) == payload
                        for family in (socket.AF_INET, socket.AF_INET6):
                            engine.udp_dns(family, 'ads.example.com', True)
                            engine.udp_dns(family, 'new.example.com', False)
            for family in (socket.AF_INET, socket.AF_INET6):
                engine.tcp_inspection(family, 80, 'new.example.com', False)
            # Several notifications may coalesce; the final saved candidate
            # must eventually win and both queue loops must resume verdicts.
            custom.write_text('final.example.com\n')
            for _ in range(5):
                assert command('--reload').returncode == 0
            engine.wait_for(lambda: 'black=1\n' in (DATA / 'rule_stats.conf').read_text(), 'last coalesced reload')
            for family in (socket.AF_INET, socket.AF_INET6):
                engine.udp_dns(family, 'final.example.com', False)
                engine.udp_dns(family, 'new.example.com', True)
            assert command('--stop').returncode == 0
            daemon.communicate(timeout=10); assert daemon.returncode == 0; daemon = None
            print('real reload: IPv6 rollback retains old snapshot/counts, transition bypass, queue drain, new policy, retained PASS flow and repeated signals passed')
        finally:
            mode.write_text(''); release.touch()
            if daemon and daemon.poll() is None:
                daemon.terminate(); daemon.communicate(timeout=25)
            if DATA.exists():
                engine.wait_for(lease_finished, 'reload command lease released', 35)
                result = command('--stop'); assert result.returncode == 0, (result.stdout, result.stderr)
                shutil.rmtree(DATA)


if __name__ == '__main__':
    main()
