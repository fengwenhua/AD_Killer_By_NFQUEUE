#!/usr/bin/env python3
"""Execute the actual Make/wrapper interfaces with isolated group fixtures."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Groups(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='adk suite ')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'tests').mkdir()
        shutil.copy(ROOT / 'Makefile', self.root)
        shutil.copy(ROOT / 'tests/run_host_tests.sh', self.root / 'tests')
        self.trace = self.root / 'trace'
        for name in ('native', 'frontend', 'contract'):
            (self.root / f'tests/run_{name}_tests.sh').write_text(
                '#!/bin/sh\nset -eu\n' + f'printf "%s\\n" "{name}:${{SANITIZE:-0}}:${{STRICT:-0}}:${{CHECK_RULE_ARTIFACTS:-0}}" >> "$TRACE"\n'
                + f'if [ "${{FAIL_GROUP:-}}" = {name} ]; then exit 23; fi\n')

    def run_group(self, target, **extra):
        result = subprocess.run(['make', '-s', target], cwd=self.root,
            env={**os.environ, 'TRACE': str(self.trace), 'SANITIZE': '0', 'STRICT': '0', 'CHECK_RULE_ARTIFACTS': '0', **extra},
            capture_output=True, text=True, timeout=10)
        lines = self.trace.read_text().splitlines() if self.trace.exists() else []
        return result, lines

    def test_complete_local_entry_runs_each_group_once(self):
        result, lines = self.run_group('test')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, ['native:0:0:0', 'frontend:0:0:0', 'contract:0:0:0'])

    def test_ci_entries_are_independent(self):
        for target, expected in [('test-native','native:0:0:0'), ('test-native-sanitize','native:1:1:0'),
                                  ('test-frontend','frontend:0:0:0'), ('test-contracts','contract:0:0:0')]:
            with self.subTest(target=target):
                self.trace.unlink(missing_ok=True)
                result, lines = self.run_group(target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(lines, [expected])

    def test_complete_entry_propagates_failure_and_stops(self):
        for group, expected in [('native', ['native']), ('frontend', ['native','frontend']), ('contract', ['native','frontend','contract'])]:
            with self.subTest(group=group):
                self.trace.unlink(missing_ok=True)
                result, lines = self.run_group('test', FAIL_GROUP=group)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual([line.split(':')[0] for line in lines], expected)

    def test_existing_full_sanitizer_and_rule_entries_are_preserved(self):
        result, lines = self.run_group('test-sanitize')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, ['native:1:1:0','frontend:1:1:0','contract:1:1:0'])
        self.trace.unlink()
        result, lines = self.run_group('test-rules')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(lines, ['native:0:0:1','frontend:0:0:1','contract:0:0:1'])


if __name__ == '__main__':
    unittest.main()
