"""Small exact inverse tests; full staged gate runs the immutable real corpus."""
import importlib.util
import json
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'tools'))
PATH=ROOT/'build/android-resolution-config-integration/files/tools/android_resolution_config_contract.py'
spec=importlib.util.spec_from_file_location('resolution_contract_test',PATH);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class Tests(unittest.TestCase):
    def test_exact_LF_and_CRLF_inverse(self):
        for ending in [b'\n',b'\r\n']:
            raw=(b'prefix\n'+m.ENTRY+b'    original_body(c);\n}\n').replace(b'\n',ending)
            header=b'/* exact qualified helper */\n';result=m.compose(raw,header)
            entry=m.ENTRY.replace(b'\n',ending);call=m.CALL.replace(b'\n',ending)
            self.assertEqual(result[len(header):].replace(entry+call,entry,1),raw)
    def test_missing_duplicate_or_already_modified_function(self):
        for raw in [b'none',m.ENTRY+m.ENTRY,m.ENTRY+m.CALL]:
            with self.assertRaises(ValueError):m.compose(raw,b'helper\n')
    def test_builder_exact_inverse_and_third_edit(self):
        for name in ['tools/build_lifted.py','tools/build_startup.py']:
            before=(ROOT/m.BEFORE/name).read_bytes();candidate=m.builder_candidate(name,before)
            self.assertIn((ROOT/name).read_bytes(),(before,candidate))
            self.assertEqual(m.restored_source(name,candidate,'a'*64),before)
            with self.assertRaises(ValueError):m.restored_source(name,candidate+b'\n# unrelated edit\n','a'*64)
    def test_wrong_baseline_cannot_be_relabelled(self):
        with self.assertRaises(ValueError):m.builder_candidate('tools/build_lifted.py',b'wrong source')
    def test_original_algorithm_proof_is_immutable(self):
        self.assertEqual(m.FIXED[m.PROOF],'9dd0b2f2436809183c95197bdbeee350bd7570f79a9b30ae7fb04a9b8b918c80')
        self.assertEqual(m.FIXED[m.HARDWARE],'1b6f76e1da8d7e8fed9a179cf7351a1cc511115635356c196e5fdc1b764970e4')
    def test_next_APK_and_build_receipt_are_not_algorithm_dependencies(self):
        current={(ROOT/name).resolve()for name in ['build/app-game/frostpunk-static-recompilation.apk','research/android-game-build.json']}
        original=Path.read_bytes
        def future(path):
            if path.resolve()in current:return b'a different future APK or build receipt'
            return original(path)
        with patch.object(Path,'read_bytes',future):
            pins={};m.qualified_proof(pins);m.direct.qualified_proof(pins)
        self.assertTrue(current.isdisjoint(pins))
    def test_algorithm_binary_dataset_helper_and_raw_HA_pins_stay_strict(self):
        hardware=json.loads((ROOT/m.HARDWARE).read_bytes())
        raw=next(Path(name)for name in hardware['output_pins']if name.endswith('001-stdout.bin'))
        targets=[ROOT/m.BASE/'resolution-config-arm64',ROOT/m.BASE/'image-blocks.bin',
                 ROOT/m.BASE/'resolution_config_candidate.h',raw]
        original=Path.read_bytes
        for target in targets:
            with self.subTest(path=str(target)):
                def drift(path):return b'unqualified drift'if path.resolve()==target.resolve()else original(path)
                with patch.object(Path,'read_bytes',drift),self.assertRaises(ValueError):m.qualified_proof({})


if __name__=='__main__':unittest.main()
