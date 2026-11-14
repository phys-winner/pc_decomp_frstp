"""Offline runner guards and durable failure evidence; no subprocess executes."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('avl_runner',ROOT/'build/run-region-avl-device.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_offline_preparation_never_adb(self):
        with patch.object(runner.subprocess,'run',side_effect=AssertionError('Unexpected ADB')):
            proof,rows,pins,adb=runner.inputs()
        self.assertEqual(len(rows),7);self.assertEqual(proof['baseline_sha256'],'a8381b4ad5d729e24e891cd31fd01827590e39563c1f0b350d02c537bc3498c0')
        self.assertGreater(len(pins),25)

    def test_timeout_preserves_partial_output(self):
        with tempfile.TemporaryDirectory() as d:
            calls=runner.Calls('never-adb',Path(d))
            error=subprocess.TimeoutExpired('fake',30,output=b'partial\x00',stderr=b'failure')
            with patch.object(runner.subprocess,'run',side_effect=error),self.assertRaises(subprocess.TimeoutExpired):calls.call(['shell','fake'])
            row=json.loads((Path(d)/'001-call.json').read_text())
            self.assertIsNone(row['exit_code']);self.assertEqual((Path(d)/'001-stdout.bin').read_bytes(),b'partial\x00')
            self.assertTrue(row['exception'])

    def test_wrong_model_and_live_own_app_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            calls=runner.Calls('never-adb',Path(d))
            with patch.object(calls,'call',side_effect=[(runner.SERIAL+'\n',''),('wrong\n','')]),self.assertRaisesRegex(ValueError,'model'):calls.identity()
            with patch.object(calls,'call',return_value=('31714\n','')),self.assertRaisesRegex(ValueError,'stopped'):calls.stopped()

    def test_wrong_apk_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            calls=runner.Calls('never-adb',Path(d))
            with patch.object(calls,'call',return_value=('0'*64+' /data/app/other/base.apk\n','')),self.assertRaisesRegex(ValueError,'SHA'):calls.hash_file('/data/app/other/base.apk',runner.APK_SHA)

    def test_failed_case_receipt_is_unique_and_durable(self):
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d);proof={'host':{}};final=folder/'never-success.json'
            with patch.object(runner,'FOLDER',folder),patch.object(runner,'FINAL',final),patch.object(runner.Calls,'identity',side_effect=ValueError('model mismatch')):
                with self.assertRaisesRegex(ValueError,'model mismatch'):runner.execute(proof,{}, {},'never-adb')
                with self.assertRaisesRegex(ValueError,'model mismatch'):runner.execute(proof,{}, {},'never-adb')
            reports=list((folder/'ha-runs').glob('*/result.json'));self.assertEqual(len(reports),2)
            for p in reports:self.assertFalse(json.loads(p.read_text())['success'])
            self.assertFalse(final.exists())

    def test_proof_and_benchmark_reject_wrong_answers(self):
        proof=json.loads(runner.SOURCE.read_text());expected=proof['host']['candidate']['semantic']
        actual=dict(expected);actual['structural_mutations']+=1
        oracle={'success':True,'independent_array_byte_model':True,'index_invariants_checked':True,
                'legacy_queries':expected['legacy_queries'],'extra_queries':expected['extra_queries']}
        with self.assertRaisesRegex(ValueError,'differs'):runner.validate_proof(json.dumps(oracle)+'\n'+json.dumps(actual),expected)
        benchmark=proof['host_model_benchmark'];bad=json.loads(json.dumps(benchmark));bad['scenarios'][1]['digest']='wrong'
        with self.assertRaisesRegex(ValueError,'digest'):runner.validate_benchmark(json.dumps(bad),benchmark)


if __name__=='__main__':unittest.main()
