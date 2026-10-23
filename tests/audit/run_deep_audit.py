#!/usr/bin/env python3
"""Opt-in defect characterization, NOT a fixed-behavior regression or root test.

Compiles unmodified production code with test-only data path rebasing and
network/process stubs. All files live in a TemporaryDirectory. No adb, network,
real iptables, root installation, or live NFQUEUE operations are performed.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / 'tests/audit'


def observe(arguments):
    result = subprocess.run(arguments, text=True, capture_output=True, timeout=10)
    print(result.stdout, end='', flush=True)
    if result.stderr:
        print(result.stderr, end='', file=sys.stderr, flush=True)
    result.check_returncode()
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true', help='also instrument the isolated C++ probes with ASan/UBSan')
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get('CXX', 'c++'))
    with tempfile.TemporaryDirectory(prefix='adk-deep-audit-') as temporary:
        directory = Path(temporary)
        data = directory / 'data'
        data.mkdir()
        assert str(data).startswith(str(directory) + os.sep)
        flags = ['-std=c++17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-function', '-pthread', '-ffunction-sections', '-fdata-sections',
                 '-DADK_AUDIT_DATA_DIR="' + str(data) + '"',
                 '-include', str(AUDIT / 'rebase_paths.h'),
                 '-I' + str(AUDIT / 'include'), '-I' + str(ROOT / 'src')]
        if args.sanitize:
            flags.extend(['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                          '-fno-sanitize-recover=all'])
        linker = ['-Wl,-dead_strip'] if sys.platform == 'darwin' else ['-Wl,--gc-sections']
        cli = directory / 'probe_cli'
        subprocess.run([*compiler, *flags, str(AUDIT / 'probe_cli.cpp'),
                        str(ROOT / 'src/update_job.cpp'), str(ROOT / 'src/process_runner.cpp'), *linker, '-o', str(cli)], check=True, timeout=120)
        observed = ''
        for operation in ('unrelated-pid',):
            output = observe([str(cli), operation])
            observed += output
            if operation == 'unrelated-pid':
                assert '"status": "running"' in output
        engine = directory / 'probe_engine'
        sources = ['rules.cpp', 'rule_stream.cpp', 'rule_bundle.cpp', 'logging.cpp',
                   'bounded_log.cpp', 'tcp_reassembly.cpp', 'dns_tcp_reassembly.cpp', 'packet.cpp',
                   'cgroup_isolation.cpp']
        subprocess.run([*compiler, *flags, str(AUDIT / 'probe_engine.cpp'),
                        *(str(ROOT / 'src' / source) for source in sources),
                        *linker, '-lz', '-o', str(engine)], check=True, timeout=120)
        observed += observe([str(engine)])
        assert sum(line.startswith('OBSERVED ') for line in observed.splitlines()) == 2
    print('Audit characterization completed: 2 remaining observations; fixed faults have normal regressions now.')


if __name__ == '__main__':
    main()
