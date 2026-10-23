#!/usr/bin/env python3
"""Execute shipped uninstall hook with fake tools; no root/device/data writes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    for status in (0, 1, 42, 126, 127, None):
        with tempfile.TemporaryDirectory(prefix='adk uninstall hook ') as temporary:
            base = Path(temporary)
            script = base/'uninstall.sh'
            shutil.copyfile(ROOT/'module/uninstall.sh', script)
            calls = base/'calls'
            env = {**os.environ, 'ADK_HOOK_TEST_CALLS': str(calls), 'PATH': str(base)+os.pathsep+os.environ['PATH']}
            if status is not None:
                binary = base/'AD_Killer_By_NFQUEUE'
                binary.write_text(f'#!/bin/sh\necho "$*" >> "$ADK_HOOK_TEST_CALLS"\nexit {status}\n')
                binary.chmod(0o755)
            timeout = base/'timeout'
            timeout.write_text('#!/bin/sh\necho "$*" >> "$ADK_HOOK_TEST_CALLS"\nexit 1\n')
            timeout.chmod(0o755)
            result = subprocess.run(['sh', str(script)], env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == (status if status in (0, 1, 42) else 1), result
            commands = calls.read_text().splitlines() if calls.exists() else []
            expected = ['--uninstall'] if status is not None else []
            if status in (126, 127, None):
                expected += ['3 iptables -w 1 -t mangle -S', '3 ip6tables -w 1 -t mangle -S']
            assert commands == expected, (status, commands)
            if status != 0:
                assert '数据' in result.stderr
    print('real uninstall hook: native success/busy/error propagation and bounded read-only missing-binary fallback passed')


if __name__ == '__main__':
    main()
