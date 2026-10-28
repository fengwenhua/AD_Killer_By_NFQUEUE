#!/usr/bin/env python3
"""Fuzz control-plane bounds and reproducible coverage seeds, no compiler needed."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import subprocess
import json
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/fuzz'))
from seeds import TARGETS, seed_inputs
spec = importlib.util.spec_from_file_location('fuzz_runner', ROOT / 'tests/fuzz/run.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class FuzzControl(unittest.TestCase):
    def test_seed_generation_is_stable_and_covers_all_targets(self):
        a = seed_inputs(ROOT / 'tests/fuzz')
        self.assertEqual(a, seed_inputs(ROOT / 'tests/fuzz'))
        self.assertEqual(set(a), set(TARGETS))
        self.assertEqual(len(a), 8)
        for target, values in a.items():
            self.assertIn('empty', values)
            self.assertTrue(all(len(value) <= 131072 for value in values.values()), target)
        self.assertIn('out-of-order-1', a['reassembly'])
        self.assertIn('consume-pipeline', a['dns_reassembly'])
        self.assertIn('inflate_limit', a['rule_stream'])
        self.assertGreater(len(a['module_zip']['long_comment']), 65536)

    def test_numeric_controls_reject_unbounded_or_injected_values(self):
        for raw in ('-1','1;echo bad','1.0','NaN','100000001','１２',' 1'):
            with self.subTest(raw=raw), patch.dict(os.environ, FUZZ_RUNS=raw):
                with self.assertRaises(ValueError):runner.number('FUZZ_RUNS',500,1,100000000)
        with patch.dict(os.environ, FUZZ_SECONDS='600'):
            self.assertEqual(runner.number('FUZZ_SECONDS',0,0,3600),600)

    def test_corpus_quota_and_oversize_are_bounded(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for n in range(8):(root / str(n)).write_bytes(b'a' * (n + 1))
            (root / 'oversized').write_bytes(b'x' * 100)
            with patch.object(runner,'MAX_CORPUS_FILES',3), patch.object(runner,'MAX_CORPUS_BYTES',5):
                paths = runner.corpus_files(root,10)
            self.assertEqual([path.name for path in paths],['0','1'])
            self.assertEqual(sorted(path.name for path in root.iterdir()),['0','1'])

    def test_current_seeds_survive_quota_even_when_cached_cases_are_smaller(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            for name,body in [('cached-a',b'a'),('cached-b',b'b'),('seed-new',b'large-seed')]:
                (root/name).write_bytes(body)
            with patch.object(runner,'MAX_CORPUS_FILES',2),patch.object(runner,'MAX_CORPUS_BYTES',11):
                files=runner.corpus_files(root,100,{'seed-new'})
            self.assertEqual([p.name for p in files],['seed-new','cached-a'])
            self.assertEqual((root/'seed-new').read_bytes(),b'large-seed')
            with patch.object(runner,'MAX_CORPUS_BYTES',1):
                with self.assertRaises(ValueError):runner.corpus_files(root,100,{'seed-new'})

    def test_symlink_hardlink_and_special_entries_refuse_without_following(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp); corpus=root/'corpus';corpus.mkdir();outside=root/'outside';outside.write_bytes(b'keep')
            link=corpus/'entry';link.symlink_to(outside)
            with self.assertRaises(ValueError):runner.corpus_files(corpus,10)
            self.assertEqual(outside.read_bytes(),b'keep')
            link.unlink();os.link(outside,link)
            with self.assertRaises(ValueError):runner.corpus_files(corpus,10)
            link.unlink();link.mkdir()
            with self.assertRaises(ValueError):runner.corpus_files(corpus,10)

    def fake_compiler(self, directory, probe_fails=False):
        path=Path(directory)/'fake-cxx'
        path.write_text('#!'+sys.executable+'\n' + """
import sys
from pathlib import Path
if '--version' in sys.argv:
 print('test compiler fixture');sys.exit(0)
output=Path(sys.argv[sys.argv.index('-o')+1])
code=2 if output.name=='probe' and PROBE_FAILS else 42 if output.name=='fuzz_packet' else 0
output.write_text('#!/bin/sh\\nexit '+str(code)+'\\n')
output.chmod(0o700)
""".replace('PROBE_FAILS',repr(probe_fails)))
        path.chmod(0o700)
        return path

    def test_failed_target_is_reported_and_other_targets_still_execute(self):
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/'results'
            result=subprocess.run([sys.executable,str(ROOT/'tests/fuzz/run.py')],
                env={**os.environ,'FUZZ_CXX':str(self.fake_compiler(temp)),'FUZZ_OUTPUT':str(output),
                     'FUZZ_REQUIRE_LIBFUZZER':'1','FUZZ_SECONDS':'0','FUZZ_RUNS':'1'},
                capture_output=True,text=True,timeout=20)
            self.assertEqual(result.returncode,1,result.stderr)
            report=json.loads((output/'report.json').read_text())
            self.assertFalse(report['complete'])
            self.assertEqual(report['failures'],['packet'])
            self.assertEqual(report['targets']['packet']['exit_code'],42)
            self.assertEqual(len(report['targets']),8)
            self.assertTrue(all(item['status']=='passed' for name,item in report['targets'].items() if name!='packet'))
            self.assertTrue((output/'logs/packet/run.log').exists())

    def test_ci_rejects_missing_libfuzzer_instead_of_substituting_standalone(self):
        with tempfile.TemporaryDirectory() as temp:
            output=Path(temp)/'results'
            result=subprocess.run([sys.executable,str(ROOT/'tests/fuzz/run.py')],
                env={**os.environ,'FUZZ_CXX':str(self.fake_compiler(temp,True)),'FUZZ_OUTPUT':str(output),
                     'FUZZ_REQUIRE_LIBFUZZER':'1','FUZZ_SECONDS':'0','FUZZ_RUNS':'1'},
                capture_output=True,text=True,timeout=20)
            self.assertNotEqual(result.returncode,0)
            report=json.loads((output/'report.json').read_text())
            self.assertFalse(report['complete'])
            self.assertEqual(report['mode'],'standalone')
            self.assertEqual(report['targets'],{})
            self.assertIn('libFuzzer is required',result.stderr)

    def test_report_replacement_keeps_latest_complete_json(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'report.json'
            runner.save_report(path,{'complete':False})
            runner.save_report(path,{'complete':True})
            self.assertEqual(path.read_text(),'{\n  "complete": true\n}\n')
            self.assertFalse(path.with_suffix('.tmp').exists())


if __name__=='__main__':unittest.main()
