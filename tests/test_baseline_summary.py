#!/usr/bin/env python3
"""Offline evidence review: counters, sessions, gaps and unsafe report inputs."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import summarize_device_baseline as summary


def sample(uptime=100, ticks=10, pid=12, started=50, boot='a'*12, image='b'*16):
    return {'schema':1,'status':'observed','module':{'id':summary.MODULE,'version':'v1.2.8','versionCode':'12801','releaseChannel':'stable','image_epoch':image},
        'device':{'model':'PKH110','boot_epoch':boot,'uptime_seconds':uptime,'clock_hz':100},
        'process':{'owner_verified':True,'pid':pid,'started':started,'cpu_ticks':ticks,'memory_kib':{'VmRSS':8192,'VmHWM':9216}},
        'freezer':{'state':'normal'},'routing':{'ipv4':{'status':'ok'},'ipv6':{'status':'ok'}},
        'sampling_overhead_ms':1,
        'metrics':{'status':'ok','quality':'owner_verified_age_unavailable','counters':{'dns_packets':ticks,'queue_errors':0,'reassembly_flows':ticks}},
        'queues':{'status':'ok','listeners':[{'queue':q,'owned':True,'queued':0,'kernel_dropped':0,'user_dropped':0} for q in (0,1)]}}


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)

    def report(self, rows, name='report', **updates):
        path=self.root/name;path.mkdir()
        meta={'schema':1,'interval_seconds':60,'duration_requested_seconds':60,'elapsed_seconds':60,'complete':True,'samples':len(rows),'failed_samples':sum(r['status']!='observed' for r in rows)}
        meta.update(updates)
        (path/'run.json').write_text(json.dumps(meta)+'\n')
        (path/'samples.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
        return path

    def test_same_session_full_deltas_and_cpu(self):
        path=self.report([sample(),sample(160,70)])
        result=summary.summarize(path)
        self.assertTrue(result['collection_complete'])
        self.assertEqual(result['verified_process_samples'],2)
        self.assertEqual(result['verified_interval_seconds'],60)
        self.assertEqual(result['cpu_percent_one_core_average'],1)
        self.assertEqual(result['rss_mib_range'],[8,8])
        self.assertEqual(result['counter_deltas_in_verified_intervals']['dns_packets'],60)
        self.assertEqual(result['issues'],{})
        self.assertNotIn('healthy',result)
        self.assertEqual(result['acceptance'],'not_evaluated_requires_device_state_matrix')

    def test_pid_boot_image_and_version_changes_do_not_bridge(self):
        for key,value in [('pid',13),('started',51),('boot','c'*12),('image','d'*16),('version','v1.2.9'),('channel','preview')]:
            with self.subTest(key=key):
                second=sample(160,5,**({key:value} if key not in ('version','channel') else {}))
                if key=='version':second['module']['version']=value
                if key=='channel':second['module']['releaseChannel']=value
                path=self.report([sample(),second],name=key)
                result=summary.summarize(path)
                self.assertEqual(result['verified_sessions'],2)
                self.assertEqual(result['verified_interval_seconds'],0)
                self.assertIsNone(result['cpu_percent_one_core_average'])
                self.assertEqual(result['counter_deltas_in_verified_intervals'],{})

    def test_dropped_sample_and_large_gap_are_not_continuous_acceptance(self):
        failed={'schema':1,'status':'unknown','reason':'private error must not be exported'}
        r=summary.summarize(self.report([sample(),failed,sample(220,100)],complete=False))
        self.assertFalse(r['collection_complete'])
        self.assertEqual(r['verified_interval_seconds'],0)
        self.assertIn('process_unverified_samples',r['issues'])
        r=summary.summarize(self.report([sample(),sample(86500,200)],name='gap',duration_requested_seconds=86400))
        self.assertEqual(r['issues']['sampling_gap_intervals'],1)
        self.assertEqual(r['verified_interval_seconds'],0)
        self.assertIn('not_evaluated',r['acceptance'])

    def test_declared_complete_does_not_hide_unfinished_requested_duration(self):
        path=self.report([sample(),sample(160,70)],duration_requested_seconds=86400)
        r=summary.summarize(path)
        self.assertFalse(r['collection_complete'])
        self.assertIn('requested_duration_not_completed',r['issues'])
        self.assertIn('not_evaluated',r['acceptance'])

    def test_clock_rollback_and_bad_hz_do_not_forge_cpu(self):
        rows=[sample(),sample(99,70)]
        r=summary.summarize(self.report(rows))
        self.assertIn('uptime_not_increasing_intervals',r['issues'])
        rows=[sample(),sample(160,70)];rows[1]['device']['clock_hz']=False
        r=summary.summarize(self.report(rows,name='hz'))
        self.assertIsNone(r['cpu_percent_one_core_average'])
        self.assertIn('cpu_interval_unknown',r['issues'])

    def test_gauges_can_fall_while_real_counter_reset_is_reported(self):
        rows=[sample(),sample(160,70)]
        rows[1]['metrics']['counters'].update(reassembly_flows=0,dns_packets=1)
        r=summary.summarize(self.report(rows))
        self.assertEqual(r['issues']['metric_counter_decrease_intervals'],1)
        self.assertNotIn('dns_packets',r['counter_deltas_in_verified_intervals'])
        self.assertNotIn('reassembly_flows',r['counter_deltas_in_verified_intervals'])
        self.assertEqual(r['gauge_observed_ranges']['reassembly_flows'],(0,10))

    def test_unknown_components_are_not_zeroes_or_full_routing_checks(self):
        row=sample();row['queues']={'status':'command_failed'};row['freezer']={'state':'unknown'}
        row['routing']['ipv6']={'status':'query_failed'};row['metrics']['quality']='unverified'
        r=summary.summarize(self.report([row]))
        self.assertEqual(r['issues']['queue_unverified_samples'],2)
        self.assertIsNone(r['queues']['0']['max_queued'])
        self.assertEqual(r['routing_query_readable_samples'],0)
        self.assertEqual(r['metrics_owner_samples'],0)
        self.assertIn('freezer_unknown_samples',r['issues'])

    def test_observed_errors_and_queue_backlog_remain_visible(self):
        row=sample();row['metrics']['counters']['verdict_failures']=3
        row['queues']['listeners'][0].update(queued=2,kernel_dropped=1)
        r=summary.summarize(self.report([row]))
        self.assertEqual(r['counter_observed_maxima']['verdict_failures'],3)
        self.assertEqual(r['queues']['0']['max_queued'],2)
        self.assertEqual(r['issues']['queue_drop_counter_observations'],1)
        self.assertIn('operational_error_counter_observations',r['issues'])

    def test_metadata_mismatch_and_file_replacement_mark_incomplete(self):
        path=self.report([sample()],samples=True)
        r=summary.summarize(path)
        self.assertFalse(r['collection_complete'])
        self.assertIn('collection_metadata_mismatch',r['issues'])
        path=self.report([sample()],name='changed')
        real=summary.signature
        with patch.object(summary,'signature',side_effect=lambda p:(0,0,0,0) if p.name=='samples.jsonl' else real(p)):
            r=summary.summarize(path)
        self.assertFalse(r['collection_complete'])
        self.assertIn('input_changed_during_read',r['issues'])

    def test_unfinished_duplicate_nonfinite_and_invalid_schema_fail(self):
        for n,body in enumerate([b'{"schema":1,"schema":1}\n',b'{"schema":1,"x":NaN}\n',b'{"schema":true}\n',b'{"schema":1}',b'{}\n']):
            with self.subTest(body=body):
                path=self.report([],name=str(n));(path/'samples.jsonl').write_bytes(body)
                with self.assertRaises(ValueError):summary.summarize(path)
        self.assertIsNone(summary.finite(10**400))
        self.assertIsNone(summary.finite(float('inf')))

    def test_file_line_and_count_bounds(self):
        path=self.report([sample(),sample(160,70)])
        with patch.object(summary,'MAX_SAMPLES',1):
            with self.assertRaises(ValueError):summary.summarize(path)
        with patch.object(summary,'MAX_BYTES',10):
            with self.assertRaises(ValueError):summary.summarize(path)
        (path/'samples.jsonl').write_bytes(b'x'*(summary.MAX_LINE+1)+b'\n')
        with self.assertRaises(ValueError):summary.summarize(path)

    def test_symlink_hardlink_and_fifo_are_never_read(self):
        path=self.report([sample()]);original=path/'samples.jsonl';saved=self.root/'outside';original.rename(saved)
        original.symlink_to(saved)
        with self.assertRaises(OSError):summary.summarize(path)
        original.unlink();os.link(saved,original)
        with self.assertRaises(ValueError):summary.summarize(path)
        original.unlink();os.mkfifo(original)
        with self.assertRaises(ValueError):summary.summarize(path)
        self.assertTrue(saved.read_text().startswith('{'))

    def test_cli_comparison_is_private_and_never_overwrites(self):
        row=sample();row['private_secret']='NEVER_EXPORT_ME';row['module']['unknown']='NEVER_EXPORT_ME'
        path=self.report([row]);baseline=self.report([sample()],name='before')
        output=self.root/'summary.json'
        args=[sys.executable,str(ROOT/'scripts/summarize_device_baseline.py'),str(path),'--compare',str(baseline),'--output',str(output)]
        result=subprocess.run(args,env={**os.environ,'PATH':''},capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr)
        text=output.read_text();self.assertNotIn('NEVER_EXPORT_ME',text)
        self.assertNotIn(str(self.root),text)
        self.assertEqual(json.loads(text)['kind'],'uncontrolled_observation_comparison')
        self.assertEqual(output.stat().st_mode & 0o777,0o600)
        result=subprocess.run(args,env={**os.environ,'PATH':''},capture_output=True,text=True,timeout=5)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(output.read_text(),text)
        self.assertEqual(list(self.root.glob('tmp*')),[])


if __name__=='__main__':unittest.main()
