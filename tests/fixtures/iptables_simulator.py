#!/usr/bin/env python3
"""Only used through an isolated test PATH; never invokes a real firewall."""
import json
import os
from pathlib import Path
import shlex
import sys
import time

path = Path(os.environ['ADK_TEST_FIREWALL_STATE'])
mode = os.environ.get('ADK_TEST_FIREWALL_MODE', '')
family = Path(sys.argv[0]).name
assert family in ('iptables', 'ip6tables')
arguments = sys.argv[1:]
data = json.loads(path.read_text())
table = data[family]

if '-S' in arguments:
    if family == 'ip6tables':
        if mode == 'query_failure':
            print('-P OUTPUT ACCEPT')  # A parseable prefix is not a successful read.
            print('permission denied', file=sys.stderr)
            sys.exit(1)
        if mode == 'empty_success':
            sys.exit(0)
        if mode == 'timeout':
            time.sleep(5)
    if family == 'iptables' and mode == 'overflow':
        sys.stdout.write('x' * (3 * 1024 * 1024))
        sys.exit(0)
    print('-P OUTPUT ACCEPT')
    for chain in table['chains']:
        print(shlex.join(['-N', chain]))
    for rule in table['output']:
        print(shlex.join(['-A', 'OUTPUT', *rule]))
    sys.exit(0)

if '-D' in arguments:
    at = arguments.index('-D')
    assert arguments[at + 1] == 'OUTPUT'
    rule = arguments[at + 2:]
    if rule not in table['output']:
        sys.exit(1)
    table['output'].remove(rule)
elif '-F' in arguments:
    assert arguments[-1].startswith('ADK_NFQ_')
elif '-X' in arguments:
    if family == 'ip6tables' and mode == 'delete_failure':
        print('injected delete failure', file=sys.stderr)
        sys.exit(1)
    chain = arguments[-1]
    if chain not in table['chains']:
        sys.exit(1)
    table['chains'].remove(chain)
else:
    raise AssertionError(arguments)

temporary = path.with_suffix('.tmp')
temporary.write_text(json.dumps(data))
temporary.replace(path)
