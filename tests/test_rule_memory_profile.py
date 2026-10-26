#!/usr/bin/env python3
"""Offline checks for benchmark process ownership and safe input admission."""
import importlib.util
import fcntl
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('profile_rule_memory', ROOT/'integration/profile_rule_memory.py')
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


class ProfileTests(unittest.TestCase):
    def test_kernel_usage_is_reported_after_reaping(self):
        usage = SimpleNamespace(ru_maxrss=1234, ru_utime=.5, ru_stime=.25)
        with tempfile.TemporaryDirectory() as directory, patch.object(profile, 'launch', return_value=123), \
                patch.object(profile.os, 'wait4', return_value=(123, 0, usage)), patch.object(profile.os, 'killpg') as kill:
            result = profile.measure(Path('/test/binary'), [], Path(directory)/'log')
            self.assertEqual(result['peak_kib'], 1234)
            kill.assert_not_called()

    def test_failed_but_reaped_process_is_never_signalled(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory)/'log'; log.write_text('rejected source')
            with patch.object(profile, 'launch', return_value=123), \
                    patch.object(profile.os, 'wait4', return_value=(123, 7 << 8, None)), \
                    patch.object(profile.os, 'killpg') as kill:
                with self.assertRaisesRegex(AssertionError, 'rejected source'):
                    profile.measure(Path('/test/binary'), [], log)
                kill.assert_not_called()

    def test_timeout_only_signals_unreaped_owned_group(self):
        with patch.object(profile, 'launch', return_value=123), \
                patch.object(profile.os, 'wait4', side_effect=[(0, 0, None), (123, 9, None)]) as reap, \
                patch.object(profile.time, 'monotonic', side_effect=[0, 2]), patch.object(profile.os, 'killpg') as kill:
            with self.assertRaisesRegex(AssertionError, 'timeout'):
                profile.measure(Path('/test/binary'), [], Path('/unused/log'), seconds=1)
            kill.assert_called_once_with(123, profile.signal.SIGKILL)
            self.assertEqual(reap.call_args.args, (123, 0))

    def test_generation_cannot_escape_data_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory); (data/'rules.current').write_text('../outside\n')
            with patch.object(profile, 'DATA', data), patch.object(profile.shutil, 'copyfile') as copy:
                with self.assertRaises(AssertionError):
                    profile.snapshot(data/'output')
                copy.assert_not_called()

    def test_existing_module_data_is_not_touched(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)/'data'; data.mkdir()
            marker = data/'preserve'; marker.write_text('unchanged')
            output = Path(directory)/'output'
            argv = ['profile', '--binary', '/unused', '--prime', '/unused', '--snapshot', '/unused', '--output', str(output)]
            with patch.object(profile, 'DATA', data), patch.object(profile.os, 'geteuid', return_value=0), \
                    patch.object(profile.os, 'uname', return_value=SimpleNamespace(sysname='Linux')), patch('sys.argv', argv):
                with self.assertRaises(AssertionError):
                    profile.main()
            self.assertEqual(marker.read_text(), 'unchanged')
            self.assertFalse(output.exists())

    def test_live_helper_lease_preserves_benchmark_data(self):
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory); data = parent/'data'; data.mkdir()
            marker = data/'preserve'; marker.write_text('writer is live')
            with (parent/'.AD_Killer_By_NFQUEUE.operations.lock').open('wb') as lease:
                fcntl.flock(lease, fcntl.LOCK_SH)
                with patch.object(profile, 'DATA', data), self.assertRaises(BlockingIOError):
                    profile.clean_data()
                self.assertTrue(marker.exists())
            with patch.object(profile, 'DATA', data):
                profile.clean_data()
            self.assertFalse(data.exists())

    def test_dangling_data_link_is_not_a_clean_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)/'data'; data.symlink_to(Path(directory)/'absent')
            argv = ['profile', '--binary', '/unused', '--prime', '/unused', '--snapshot', '/unused',
                    '--output', str(Path(directory)/'output')]
            with patch.object(profile, 'DATA', data), patch.object(profile.os, 'geteuid', return_value=0), \
                    patch.object(profile.os, 'uname', return_value=SimpleNamespace(sysname='Linux')), patch('sys.argv', argv):
                with self.assertRaises(AssertionError):
                    profile.main()
            self.assertTrue(data.is_symlink())


if __name__ == '__main__':
    unittest.main()
