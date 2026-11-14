"""Exact TID source bridge only; historical EH foundations stay immutable."""
import hashlib
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import android_eh_contract as eh
import android_eh_tid_bridge as bridge


class BridgeTests(unittest.TestCase):
    def test_actual_qualified_bridge_and_all_side_pins(self):
        pins={};path=bridge.checked_threads(ROOT,bridge.THREADS,bridge.BEFORE,pins,eh.checked,eh.TID_BRIDGE_SHA)
        self.assertEqual(path,ROOT/bridge.THREADS)
        self.assertEqual(pins[path],bridge.AFTER)
        for name in bridge.FIXED:self.assertEqual(pins[ROOT/name],bridge.FIXED[name])
        self.assertIn(ROOT/'tools/android_eh_tid_bridge.py',pins)
        self.assertGreater(len(pins),200)

    def test_third_expression_drift_rejected(self):
        raw=(ROOT/bridge.THREADS).read_bytes().replace(b'++cs->recursion;',b'cs->recursion+=2;',1)
        with self.assertRaises(ValueError):bridge.restored_threads(raw)

    def test_wrong_EH_baseline_rejected(self):
        with self.assertRaises(ValueError):bridge.checked_threads(ROOT,bridge.THREADS,'0'*64,{},eh.checked,eh.TID_BRIDGE_SHA)

    def test_missing_sideproof_rejected(self):
        original=Path.is_file
        for name in bridge.FIXED:
            with self.subTest(name=name),patch.object(Path,'is_file',lambda p:False if p==ROOT/name else original(p)):
                with self.assertRaises(ValueError):bridge.checked_threads(ROOT,bridge.THREADS,bridge.BEFORE,{},eh.checked,eh.TID_BRIDGE_SHA)

    def test_wrong_sideproof_rejected(self):
        original=bridge.digest
        with patch.object(bridge,'digest',side_effect=lambda p:'0'*64 if Path(p)==ROOT/bridge.HARDWARE else original(p)):
            with self.assertRaises(ValueError):bridge.checked_threads(ROOT,bridge.THREADS,bridge.BEFORE,{},eh.checked,eh.TID_BRIDGE_SHA)

    def test_bridge_code_drift_rejected(self):
        with self.assertRaises(ValueError):bridge.checked_threads(ROOT,bridge.THREADS,bridge.BEFORE,{},eh.checked,'0'*64)

    def test_exact_validator_reconstruction(self):
        proof=json.loads((ROOT/eh.INTEGRATION).read_text());expected=proof['source_hashes'][bridge.VALIDATOR]
        restored=bridge.restored_validator((ROOT/bridge.VALIDATOR).read_bytes(),expected,eh.TID_BRIDGE_SHA)
        self.assertEqual(hashlib.sha256(restored).hexdigest(),'433c5b09dd0918af014ffd0318d23a3a1e399db6f12135adbad0f03d3ac75da3')
        with self.assertRaises(ValueError):bridge.restored_validator((ROOT/bridge.VALIDATOR).read_bytes()+b'\n# drift\n',expected,eh.TID_BRIDGE_SHA)

    def test_unmodified_foundation_still_uses_strict_check(self):
        name='native/runtime64/eh64.h';pins={};expected=eh.digest(ROOT/name)
        self.assertEqual(bridge.checked_threads(ROOT,name,expected,pins,eh.checked,eh.TID_BRIDGE_SHA),ROOT/name)
        with self.assertRaises(ValueError):bridge.checked_threads(ROOT,name,'0'*64,{},eh.checked,eh.TID_BRIDGE_SHA)

    def test_unchanged_thread_baseline_needs_no_new_sideproof(self):
        def strict(name,expected,pins):pins[ROOT/name]=expected;return ROOT/name
        with patch.object(bridge,'digest',return_value=bridge.BEFORE):
            pins={};bridge.checked_threads(ROOT,bridge.THREADS,bridge.BEFORE,pins,strict,'unused')
            self.assertEqual(pins,{ROOT/bridge.THREADS:bridge.BEFORE})

    def test_UI_extension_does_not_change_historical_CPU_scope(self):
        import generate_android_aot_overlay as overlay
        selected=set(json.loads((ROOT/eh.INTEGRATION).read_text())['contract']['selected_units'])
        current=overlay.reserved_units();ui=overlay.render_scale.UNITS
        self.assertEqual(len(current-ui),51)
        self.assertEqual(len(selected&(current-ui)),43)
        self.assertEqual(len(selected&current),45)
        self.assertEqual(len(ui),2)
        self.assertTrue(ui<=selected)


if __name__=='__main__':unittest.main()
