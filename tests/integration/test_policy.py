"""Real per-UID traffic and rule-mode audit; disposable Linux/root ONLY."""
import gzip
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys

from test_engine import DATA, BINARY, command, dns_query, state, wait_for


def uid_request(family, uid, allowed, tcp=False, host='ads.example.com'):
    address = '127.0.0.1' if family == socket.AF_INET else '::1'
    port = 80 if tcp else 53
    payload = (f'GET / HTTP/1.1\r\nHost: {host}\r\n\r\n'.encode() if tcp else dns_query(host))
    with socket.socket(family, socket.SOCK_STREAM if tcp else socket.SOCK_DGRAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((address, port))
        server.settimeout(0.75)
        if tcp:
            server.listen(1)
        client_code = '''
import os,socket,sys
os.setuid(int(sys.argv[1]))
family,kind=int(sys.argv[2]),int(sys.argv[3])
allowed=sys.argv[4]=='1'
with socket.socket(family,kind) as s:
    s.settimeout(2)
    address=(sys.argv[5],int(sys.argv[6]))
    payload=bytes.fromhex(sys.argv[7])
    if kind==socket.SOCK_STREAM:
        s.connect(address)
        s.sendall(payload)
    else: s.sendto(payload,address)
    try: response=s.recv(4096)
    except ConnectionResetError:
        assert not allowed
    else:
        if allowed: assert response==b'OK',response
        elif kind==socket.SOCK_DGRAM: assert response[:2]==payload[:2] and response[3]&15==3,response
        else: assert response==b'',response
'''
        client = subprocess.Popen([sys.executable, '-c', client_code, str(uid), str(family),
                                   str(socket.SOCK_STREAM if tcp else socket.SOCK_DGRAM),
                                   str(int(allowed)), address, str(port), payload.hex()],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            if tcp:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(.75)
                    try:
                        actual = connection.recv(4096)
                    except (ConnectionResetError, socket.timeout):
                        actual = b''
                    assert bool(actual) == allowed, (uid, family, 'tcp', actual)
                    if allowed:
                        assert actual == payload
                        connection.sendall(b'OK')
            else:
                try:
                    actual, remote = server.recvfrom(4096)
                except socket.timeout:
                    assert not allowed, (uid, family, 'allowed UDP did not arrive')
                else:
                    assert allowed and actual == payload, (uid, family, 'blocked UDP arrived')
                    server.sendto(b'OK', remote)
            stdout, stderr = client.communicate(timeout=3)
            assert client.returncode == 0, (uid, family, tcp, stdout, stderr)
        finally:
            if client.poll() is None:
                client.kill()
                client.communicate()


def stats_match(mode, black):
    path = DATA / 'rule_stats.conf'
    if not path.exists():
        return False
    values = dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)
    return values.get('mode') == str(mode) and values.get('black') == str(black)


def wait_policy(mode, black, description):
    wait_for(lambda: stats_match(mode, black) and json.loads(command(BINARY, '--status').splitlines()[-1])['iptables_active'], description)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'requires clean disposable Linux/root'
    packages = Path('/data/system/packages.list')
    assert not packages.exists(), 'refusing to overwrite an existing Android package database'
    daemon = None
    try:
        DATA.parent.mkdir(parents=True, exist_ok=True)
        command(BINARY, '--install')
        (DATA / 'hosts_rules.gz').write_bytes(gzip.compress(b'# valid empty category\n'))
        (DATA / 'adguard_rules.gz').write_bytes(gzip.compress(b'ads.example.com\n'))
        (DATA / 'custom_rules.txt').write_text('')
        daemon = subprocess.Popen([BINARY, '--daemon', '--foreground'])
        wait_for(lambda: state() == 'running' or daemon.poll() is not None, 'policy engine start')
        assert daemon.poll() is None and state() == 'running'
        wait_policy(2, 1, 'initial AdGuard rule')
        uid_request(socket.AF_INET, 10001, False)
        command(BINARY, '--mode', '1')
        wait_policy(1, 0, 'valid empty Hosts-only snapshot must replace AdGuard')
        for family in (socket.AF_INET, socket.AF_INET6):
            uid_request(family, 10001, True)
        # A corrupt selected category is not an intentional empty rule set.
        (DATA / 'adguard_rules.gz').write_bytes(b'not gzip')
        command(BINARY, '--mode', '2')
        wait_for(lambda: '校验失败' in (DATA / 'log/engine.log').read_text(), 'corrupt reload rejection')
        assert stats_match(1, 0)
        uid_request(socket.AF_INET, 10001, True)
        (DATA / 'adguard_rules.gz').write_bytes(gzip.compress(b'ads.example.com\n'))
        command(BINARY, '--reload')
        wait_policy(2, 1, 'combined mode restores AdGuard')
        uid_request(socket.AF_INET6, 10001, False)

        packages.parent.mkdir(parents=True, exist_ok=True)
        packages.write_text('com.example.first 10001 0 /data/user/0/first default\ncom.example.second 10002 0 /data/user/0/second default\n')
        (DATA / 'app_whitelist.txt').write_text('com.example.first\n')
        (DATA / 'app_intercept.txt').write_text('com.example.second\n')
        for scope in (0, 1, 0):
            command(BINARY, '--scope', str(scope))
            command(BINARY, '--apps-apply')
            wait_for(lambda: json.loads(command(BINARY, '--status').splitlines()[-1])['iptables_active'], 'UID selector applied')
            cache = DATA / ('app_intercept_uids.conf' if scope else 'app_uids.conf')
            assert ('10002' if scope else '10001') in cache.read_text()
            for family in (socket.AF_INET, socket.AF_INET6):
                for tcp in (False, True):
                    uid_request(family, 10001, True, tcp)
                    uid_request(family, 10002, False, tcp)
                    # Neither list includes this UID: global mode intercepts it,
                    # selected-app mode bypasses it. Detect a stale selector.
                    uid_request(family, 10003, scope == 1, tcp)
        command(BINARY, '--scope', '1')
        (DATA / 'app_intercept.txt').write_text('')
        command(BINARY, '--apps-apply')
        wait_for(lambda: json.loads(command(BINARY, '--status').splitlines()[-1])['iptables_active'], 'empty per-app selector')
        uid_request(socket.AF_INET, 10002, True)
        # Explicit custom rules are always active, including Hosts-only mode.
        (DATA / 'custom_rules.txt').write_text('||custom-block.example^\n@@ads.example.com\n')
        command(BINARY, '--scope', '0')
        (DATA / 'app_whitelist.txt').write_text('')
        command(BINARY, '--apps-apply')
        command(BINARY, '--mode', '1')
        wait_policy(1, 1, 'custom rules in Hosts-only mode')
        uid_request(socket.AF_INET, 10002, False, host='custom-block.example')
        command(BINARY, '--mode', '2')
        wait_policy(2, 2, 'custom plus AdGuard')
        uid_request(socket.AF_INET6, 10002, True)
        (DATA / 'rules.current').write_text('../invalid-generation\n')
        command(BINARY, '--reload')
        wait_for(lambda: 'generation 指针' in (DATA / 'log/engine.log').read_text(), 'invalid manifest reload rejected')
        assert stats_match(2, 2)
        uid_request(socket.AF_INET, 10002, False, host='custom-block.example')
        command(BINARY, '--stop')
        assert daemon.wait(timeout=10) == 0
        result = subprocess.run([BINARY, '--daemon', '--foreground'], capture_output=True, text=True, timeout=10)
        assert result.returncode != 0
        status = json.loads(command(BINARY, '--status').splitlines()[-1])
        assert status['last_error_code'] == 'rule_load_failed' and not status['iptables_active']
        print('real dual-stack per-UID allow/intercept, saved-list roundtrip, empty selector, mode 1/2, valid-empty reload, corrupt/manifest preservation, invalid startup and custom exceptions passed')
    except BaseException:
        log = DATA / 'log/engine.log'
        if log.exists():
            print(log.read_text()[-12000:], file=sys.stderr)
        raise
    finally:
        if daemon is not None and daemon.poll() is None:
            daemon.terminate()
            daemon.wait(timeout=10)
        if DATA.exists():
            shutil.rmtree(DATA)
        if packages.exists():
            packages.unlink()


if __name__ == '__main__':
    main()
