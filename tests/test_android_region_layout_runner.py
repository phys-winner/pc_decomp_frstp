import importlib.util
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('layout_runner',ROOT/'build/run-region-layout-device.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        proof=json.loads((ROOT/'build/android-region-layout-candidate/result.json').read_text())
        cls.expected=proof['results']['candidate']['host'][0]['runs'][0]['stdout']
        cls.rows=runner.parsed(cls.expected)
        cls.bench=proof['results']['candidate']['host'][1]['runs'][0]['stdout']

    def changed(self,mutate):
        rows=runner.copied(self.rows);mutate(rows)
        return '\n'.join(json.dumps(r)for r in rows)+'\n'

    def test_frozen_host(self):runner.validate_positive(self.expected,self.expected,'proof')
    def test_crlf(self):runner.validate_positive(self.expected.replace('\n','\r\n'),self.expected,'proof')
    def test_benchmark(self):runner.validate_positive(self.bench,self.bench,'bench')
    def test_oracle_count(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[0].update(legacy_queries=1)),self.expected,'proof')
    def test_parallel_count(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[-1].update(parallel_checks=0)),self.expected,'proof')
    def test_owner_offset(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[-1].update(owner_offset=0)),self.expected,'proof')
    def test_fake_gain_claim(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[-1].update(game_fps_proven=True)),self.expected,'proof')
    def test_missing_row(self):
        with self.assertRaises(ValueError):runner.validate_positive('\n'.join(self.expected.splitlines()[1:]),self.expected,'proof')
    def test_zero_duration(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[-1]['whole_add_validate_remove'][0].update(ns=0)),self.expected,'proof')
    def test_wrong_digest(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[-1]['whole_add_validate_remove'][0].update(checksum=0)),self.expected,'proof')
    def test_tls_fastpath_must_run(self):
        with self.assertRaises(ValueError):runner.validate_positive(self.changed(lambda r:r[1].update(fast_hits=0)),self.expected,'proof')


if __name__=='__main__':unittest.main()
