#!/usr/bin/env python3
"""Prevent missing, skipped or failed parallel tests from producing a green gate."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ci_policy import HOST_SHARDS, REQUIRED_JOBS, verify_host_results


class HostGate(unittest.TestCase):
    def successful(self):
        return {name: {'result': 'success', 'outputs': {}} for name in HOST_SHARDS}

    def test_complete_success(self):
        verify_host_results(self.successful())
        self.assertTrue(HOST_SHARDS <= REQUIRED_JOBS)

    def test_each_missing_shard_is_rejected(self):
        for name in HOST_SHARDS:
            with self.subTest(name=name):
                value = self.successful(); del value[name]
                with self.assertRaises(ValueError): verify_host_results(value)

    def test_each_non_successful_result_is_rejected(self):
        for name in HOST_SHARDS:
            for state in ('failure', 'cancelled', 'skipped', 'in_progress', '', None):
                with self.subTest(name=name, state=state):
                    value = self.successful(); value[name]['result'] = state
                    with self.assertRaises(ValueError): verify_host_results(value)

    def test_unknown_or_malformed_inventory_is_rejected(self):
        value = self.successful(); value['extra'] = {'result': 'success'}
        for item in (value, {}, None, [], 'success'):
            with self.assertRaises(ValueError): verify_host_results(item)
        for item in (None, {}, 'success', {'conclusion': 'success'}):
            value = self.successful(); value['host-unit'] = item
            with self.assertRaises(ValueError): verify_host_results(value)

    def test_actual_cli_exit_status(self):
        for body, good in ((json.dumps(self.successful()), True), ('{}', False), ('', False), ('{', False)):
            with self.subTest(body=body):
                run = subprocess.run([sys.executable, str(ROOT / 'scripts/ci_policy.py')],
                    env={**os.environ, 'CI_NEEDS_JSON': body}, capture_output=True, text=True, timeout=10)
                self.assertEqual(run.returncode == 0, good, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
