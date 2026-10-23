#!/usr/bin/env python3
"""Actual C++ cleanup/executor; simulated executables, no root or network."""
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

binary = sys.argv[1]
root = Path(subprocess.check_output([binary, 'root'], text=True).strip())
assert root.name == 'cleanup-data' and not root.exists()
root.mkdir(mode=0o700)
programs = root / 'bin'; programs.mkdir()
simulator = Path(__file__).resolve().parent / 'fixtures/iptables_simulator.py'
for family in ('iptables', 'ip6tables'):
    shutil.copy2(simulator, programs / family)
    (programs / family).chmod(0o755)
state = root / 'state.json'
sentinel = root / 'MUST_NOT_BE_EXECUTED'
foreign = ['-p', 'udp', '--dport', '443', '-j', 'DROP']
literal = f"literal '; $(touch {sentinel}) \"quote\""


def fixture(extra=False):
    data = {family: {'chains': ['ADK_NFQ_ENTRY', 'ADK_NFQ_OUT_A', 'VENDOR'], 'output': [
        ['-j', 'ADK_NFQ_ENTRY'], ['-m', 'comment', '--comment', literal, '-j', 'ADK_NFQ_ENTRY'],
        ['-m', 'comment', '--comment', '', '-j', 'ADK_NFQ_ENTRY'], foreign,
    ]} for family in ('iptables', 'ip6tables')}
    if extra:
        for table in data.values():
            table['chains'].append('ADK_NFQ_UNRECOGNIZED')
    state.write_text(json.dumps(data))


def execute(mode='', operation='cleanup'):
    started = time.monotonic()
    result = subprocess.run([binary, operation], text=True, capture_output=True, timeout=10,
        env={**os.environ, 'PATH': str(programs) + os.pathsep + os.environ['PATH'],
             'ADK_TEST_FIREWALL_STATE': str(state), 'ADK_TEST_FIREWALL_MODE': mode})
    value = json.loads(result.stdout)
    assert (result.returncode == 0) == (value['state'] == 'clean'), (result, value)
    assert not sentinel.exists(), 'untrusted quoted rule/comment was evaluated as shell'
    assert time.monotonic() - started < (10 if operation == 'inspect' else 3)
    return value


try:
    fixture()
    assert execute(operation='inspect')['state'] == 'partial'
    value = execute()
    assert value['state'] == 'clean', value
    for table in json.loads(state.read_text()).values():
        assert table['chains'] == ['VENDOR'] and table['output'] == [foreign]
    assert execute()['state'] == 'clean', 'cleanup must be idempotent'
    fixture()
    value = execute('delete_failure')
    assert value['state'] == 'partial' and any('ip6tables' in item for item in value['remaining'])
    assert any('-X' in item and 'injected delete failure' in item for item in value['errors'])
    assert execute()['state'] == 'clean', 'retry after failure must recover'
    for mode in ('query_failure', 'empty_success', 'timeout', 'overflow'):
        fixture()
        value = execute(mode)
        assert value['state'] == 'unknown' and value['errors'], (mode, value)
    fixture(extra=True)
    value = execute()
    assert value['state'] == 'partial'
    for table in json.loads(state.read_text()).values():
        assert table['chains'] == ['VENDOR', 'ADK_NFQ_UNRECOGNIZED'] and table['output'] == [foreign]
    fixture()
    before = state.read_bytes()
    with (root / '.route.lock').open('w') as descriptor:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        value = execute()
        assert value['state'] == 'unknown' and 'lock' in value['errors'][0]
        assert state.read_bytes() == before
    assert execute()['state'] == 'clean'
    print('actual firewall cleanup: dual-stack partial/error/timeout/limits/lock, exact argv, foreign preservation and retry passed')
finally:
    shutil.rmtree(root)
