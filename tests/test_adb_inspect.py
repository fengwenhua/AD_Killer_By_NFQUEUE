#!/usr/bin/env python3
"""Offline transport/schema/privacy tests; never connects to a phone."""
import base64
from contextlib import redirect_stderr
import copy
import io
import json
from pathlib import Path
import os
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import adb_inspect as tool


BOOT = '12345678-1234-1234-1234-123456789abc'


def ok(body):
    return {'status': 'ok', 'text': body}


def stat(start=100):
    fields = ['S'] + ['0'] * 25
    fields[11], fields[12], fields[17], fields[19] = '10', '2', '3', str(start)
    return '42 (AD Killer) ' + ' '.join(fields)


def sections():
    return {key: ok(value) for key, value in {
        'root_uid': '0', 'module': f'id={tool.MODULE}\nversion=v1.2.6\nversionCode=12601\nreleaseChannel=stable\n',
        'model': 'PKH110', 'sdk': '36', 'kernel': '6.6-test', 'clock_hz': '100', 'boot': BOOT, 'uptime': '100.5 200.5',
        'image': '1:2:300', 'image_after': '1:2:300', 'image_sha': 'a' * 64 + ' binary',
        'pid': '42', 'pid_after': '42', 'pstat': stat(), 'pstat_after': stat(),
        'pstatus': 'VmRSS: 1024 kB\nVmHWM: 2048 kB\nVmData: 3000 kB\n', 'pio': 'read_bytes: 10\nwrite_bytes: 20\n',
        'meminfo': 'MemTotal: 2000000 kB\nMemAvailable: 1000000 kB\nMemFree: 300000 kB',
        'runtime': 'running', 'config': 'scope=0\nenabled=true\nprivate_url=https://secret.example/token\n',
        'queues': '0 987654 0 2 65535 0 0 100 1\n', 'netlink': '0 12 987654 0 0 0 0 0 0 1234\n', 'sockets': 'socket:[1234]\n',
        'metrics': f'metrics_format=2\nowner_boot={BOOT}\nowner_pid=42\nowner_started=100\ndns_packets=100\nsecret_domain=secret.example\n',
        'ipv4': '-N ADK_NFQ_ENTRY\n-A OUTPUT -j ADK_NFQ_ENTRY\n-A ADK_NFQ_ENTRY -m owner --uid-owner 12345 -j ACCEPT\nADK_ROUTE_EXIT:0\n',
        'ipv6': 'ADK_ROUTE_EXIT:0\n', 'disk': 'Filesystem 1K-blocks Used Available Use% Mounted\n/dev/fake 100 40 60 40% /data\n',
        'engine_log': '123:1000', 'dns_log': '456:1000',
        'cgroup': '0::/\n', 'mountinfo': '1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n',
    }.items()}


class FakeProbe:
    def __init__(self, data):
        self.data = data

    def execute(self, script, caps):
        return self.data, ''


class InspectionTests(unittest.TestCase):
    def test_frames_and_failed_reads(self):
        body = b'hello\n@EXIT:0\n'
        frame = b'@ADK:value:ok\n' + base64.b64encode(body) + b'\n@END\n'
        self.assertEqual(tool.parse_frames(frame, {'value': 5}), {'value': ok('hello')})
        self.assertEqual(tool.parse_frames(frame, {'value': 2})['value']['status'], 'oversize')
        failed = b'@ADK:value:ok\n' + base64.b64encode(b'\n@EXIT:1\n') + b'\n@END\n'
        self.assertEqual(tool.parse_frames(failed, {'value': 5})['value']['status'], 'command_failed')
        for malformed in (frame + frame, frame[:-5], b'raw accidental output\n', b'@ADK:value:ok\n!!!!\n@END\n'):
            with self.assertRaises(ValueError):
                tool.parse_frames(malformed, {'value': 5})

    def test_only_explicit_device_and_no_serial_in_remote_script(self):
        captured = []
        def runner(args):
            captured.append(args)
            return {'code': 1, 'error': '', 'data': b''}
        probe = tool.Probe('specific-device;literal', adb='/adb', runner=runner)
        self.assertEqual(probe.execute('printf fixed', {})[1], 'adb_or_root_failed')
        self.assertEqual(captured[0][:4], ['/adb', '-s', 'specific-device;literal', 'shell'])
        self.assertNotIn('specific-device', captured[0][-1])
        for invalid in ('', 'a\nb', 'a b'):
            with self.assertRaises(ValueError):
                tool.Probe(invalid, adb='/adb')
        with patch.object(sys, 'argv', ['adb_inspect.py']), redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
            tool.main()
        self.assertEqual(stopped.exception.code, 2)

    def test_bounded_client_output_and_timeout(self):
        result = tool.run_bounded([sys.executable, '-c', 'import sys; sys.stdout.write("x"*100000)'], limit=100)
        self.assertEqual(result['error'], 'output_limit')
        started = time.monotonic()
        result = tool.run_bounded([sys.executable, '-c', 'import time; time.sleep(20)'], timeout=.1)
        self.assertEqual(result['error'], 'timeout')
        self.assertLess(time.monotonic() - started, 2)

    def test_routing_failure_is_not_empty(self):
        self.assertEqual(tool.route_summary(ok('ADK_ROUTE_EXIT:1\n'))['status'], 'query_failed')
        self.assertEqual(tool.route_summary(ok(''))['status'], 'unverified_query')
        summary = tool.route_summary(sections()['ipv4'])
        self.assertEqual(summary['rule_count'], 1)
        self.assertNotIn('12345', json.dumps(summary))

    def test_netlink_port_is_never_assumed_to_be_pid(self):
        data = sections()
        summary = tool.queue_summary(data, True)
        self.assertTrue(summary['listeners'][0]['owned'])
        data['sockets'] = ok('socket:[9999]\n')
        self.assertIsNone(tool.queue_summary(data, True)['listeners'][0]['owned'])
        data['queues'] = {'status': 'unreadable'}
        self.assertEqual(tool.queue_summary(data, True)['status'], 'unreadable')

    def test_identity_boot_and_private_fields(self):
        data = sections()
        snapshot = tool.inspect(FakeProbe(data))
        self.assertTrue(snapshot['process']['owner_verified'])
        self.assertEqual(snapshot['metrics']['quality'], 'owner_verified_age_unavailable')
        self.assertNotIn(BOOT, json.dumps(snapshot))
        self.assertNotIn('secret.example', json.dumps(snapshot))
        self.assertNotIn('12345', json.dumps(snapshot))
        self.assertEqual(snapshot['disk']['available_kib'], 60)
        data['pstat_after'] = ok(stat(start=101))
        self.assertFalse(tool.inspect(FakeProbe(data))['process']['owner_verified'])
        data = sections(); data['image_after'] = ok('1:3:300')
        self.assertFalse(tool.inspect(FakeProbe(data))['process']['owner_verified'])

    def test_freezer_full_root_mapping_and_unknown(self):
        groups = tool.freezer_paths('0::/uid_0/pid_42\n', '1 0 0:1 / /sys/fs/cgroup rw - cgroup2 cgroup rw\n')
        self.assertEqual(groups, [(True, '/sys/fs/cgroup/uid_0/pid_42/cgroup.events')])
        groups = tool.freezer_paths('5:freezer:/abc\n', '1 0 0:1 / /dev/freezer rw - cgroup freezer rw,freezer\n')
        self.assertEqual(groups, [(False, '/dev/freezer/abc/freezer.state')])
        with self.assertRaises(ValueError):
            tool.freezer_paths('0::/abc\n', '1 0 0:1 /subtree /sys/fs/cgroup rw - cgroup2 cgroup rw\n')
        with self.assertRaises(ValueError):
            tool.freezer_paths('0::/../outside\n', sections()['mountinfo']['text'])
        with self.assertRaises(ValueError):
            tool.freezer_paths('0::/\n0::/\n', sections()['mountinfo']['text'])

    def test_freezer_missing_fields_and_migration_never_look_normal(self):
        data = sections()
        data['cgroup'] = ok('0::/test\n')
        after = copy.deepcopy(data)
        after['group_0'] = ok('frozen 1\n')
        result = tool.freezer_summary(FakeProbe(after), data, tool.process_stat(stat()))
        self.assertEqual(result['state'], 'frozen')
        after['group_0'] = ok('populated 1\n')
        self.assertEqual(tool.freezer_summary(FakeProbe(after), data, tool.process_stat(stat()))['state'], 'unknown')
        after['group_0'] = ok('frozen 0\n')
        after['cgroup'] = ok('0::/migrated\n')
        self.assertEqual(tool.freezer_summary(FakeProbe(after), data, tool.process_stat(stat()))['state'], 'unknown')

    def test_private_durable_report_and_incomplete_capture_exit(self):
        with tempfile.TemporaryDirectory(prefix='adk-adb-inspect-test-') as directory:
            output = Path(directory) / 'report'
            with patch.object(sys, 'argv', ['adb_inspect.py', '--serial', 'private-device-serial', '--output', str(output)]), patch.object(tool, 'Probe', return_value=FakeProbe(sections())), patch('sys.stdout', new=io.StringIO()):
                self.assertEqual(tool.main(), 0)
            contents = ''.join(path.read_text() for path in output.iterdir())
            self.assertNotIn('private-device-serial', contents)
            self.assertNotIn('secret.example', contents)
            self.assertEqual(os.stat(output).st_mode & 0o077, 0)
            self.assertTrue(all(os.stat(path).st_mode & 0o077 == 0 for path in output.iterdir()))
            metadata = json.loads((output / 'run.json').read_text())
            self.assertTrue(metadata['complete'])
            self.assertEqual(metadata['latency'], 'not_probed')
            self.assertIn('not_evaluated', metadata['acceptance'])
            failed = Path(directory) / 'failed'
            with patch.object(sys, 'argv', ['adb_inspect.py', '--serial', 'private-device-serial', '--output', str(failed)]), patch.object(tool, 'Probe', return_value=FakeProbe({})), patch('sys.stdout', new=io.StringIO()):
                self.assertEqual(tool.main(), 1)
            self.assertFalse(json.loads((failed / 'run.json').read_text())['complete'])

    def test_delta_never_crosses_restarts_or_negative_counters(self):
        old = tool.inspect(FakeProbe(sections())); new = copy.deepcopy(old)
        new['device']['uptime_seconds'] += 10
        new['process']['cpu_ticks'] += 50
        new['process']['io']['write_bytes'] += 20
        tool.add_deltas(old, new)
        self.assertEqual(new['delta']['cpu_percent_one_core'], 5)
        self.assertEqual(new['delta']['write_bytes'], 20)
        new['process']['started'] += 1
        tool.add_deltas(old, new)
        self.assertIsNone(new['delta']['cpu_percent_one_core'])
        self.assertEqual(new['delta']['status'], 'unverified_or_new_session')


if __name__ == '__main__':
    unittest.main()
