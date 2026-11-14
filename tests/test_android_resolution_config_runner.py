import importlib.util
import json
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('resolution_runner',ROOT/'build/run-resolution-config-device.py')
r=importlib.util.module_from_spec(spec);spec.loader.exec_module(r)
PROOF=json.loads(r.SOURCE.read_text());TEXT=PROOF['host']['stdout']


class Tests(unittest.TestCase):
    def test_actual_host_rows(self):self.assertEqual(len(r.validate(TEXT,TEXT)),2)
    def test_crlf(self):self.assertEqual(len(r.validate(TEXT.replace('\n','\r\n'),TEXT)),2)
    def test_missing_row(self):
        with self.assertRaises(ValueError):r.validate(TEXT.splitlines()[0],TEXT)
    def test_counter_and_scope_drift(self):
        for key,value in [('normalization_cases',0),('game_fps_proven',True),('native_UI_RT_hardware_proven',True)]:
            rows=[json.loads(line)for line in TEXT.splitlines()];rows[1][key]=value
            with self.assertRaises(ValueError):r.validate('\n'.join(map(json.dumps,rows)),TEXT)
    def test_fixed_installed_ownership_pin(self):self.assertEqual(r.APK_SHA,'f3577452e6e10d2d42e7a9b3fb818ac1f64e10a1431b442190dd06fd112a4166')
    def test_all_pins_no_adb(self):
        common,p,pins,adb=r.inputs();self.assertEqual(common.APK_SHA,r.APK_SHA);self.assertEqual(len(p['negative']),12);self.assertGreater(len(pins),25)


if __name__=='__main__':unittest.main()
