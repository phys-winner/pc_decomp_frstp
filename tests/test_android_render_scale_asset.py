"""Small raw archives exercise the same one-record append producer; never ADB."""
import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_android_render_scale_asset as asset


class AppendSetting(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT / 'build', prefix='settings-asset-test-')
        self.root = Path(self.temp.name)
        self.index, self.data, self.setting = [self.root / n for n in ['common.idx', 'common.dat', 'setting.bin']]
        original = (ROOT / 'build/render-scale-ui-candidate/settings-original.bin').read_bytes()
        self.data.write_bytes(b'untouched-before' + original + b'untouched-after')
        self.rows = [(1, 16, 16, 0, 0), (asset.KEY, len(original), len(original), 16, 0),
                     (2, 15, 15, 16 + len(original), 0)]
        self.index.write_bytes(b'\0\2\1' + struct.pack('<Q', 3) + b''.join(asset.RECORD.pack(*r) for r in self.rows))
        self.setting.write_bytes((ROOT / 'build/render-scale-ui-candidate/settings-render-scale.bin').read_bytes())
        self.expected = {name: (asset.sha(p), p.stat().st_size) for name, p in
                         [('common.idx', self.index), ('common.dat', self.data), ('setting', self.setting)]}
        self.before = {p: p.read_bytes() for p in [self.index, self.data, self.setting]}

    def tearDown(self):
        self.temp.cleanup()

    def run_append(self):
        return asset.append_setting(self.index, self.data, self.setting, self.root / 'derived', self.expected)

    def test_only_selected_record_and_appended_bytes_change(self):
        result = self.run_append()
        self.assertEqual(result['unselected_entries'], 2)
        output = self.root / 'derived'
        self.assertEqual((output / 'common.dat').read_bytes(), self.before[self.data] + self.before[self.setting])
        rows = asset.entries((output / 'common.idx').read_bytes(), (output / 'common.dat').stat().st_size)
        self.assertEqual(rows[0], self.rows[0]); self.assertEqual(rows[2], self.rows[2])
        self.assertEqual(rows[1], (asset.KEY, 4963, 4963, len(self.before[self.data]), 0))
        self.assertTrue(all(p.read_bytes() == raw for p, raw in self.before.items()))

    def test_input_drift_rejects_before_output(self):
        self.setting.write_bytes(b'x' * 4963)
        with self.assertRaisesRegex(ValueError, 'qualified bytes'):
            self.run_append()
        self.assertFalse((self.root / 'derived').exists())

    def test_unknown_original_payload_rejected(self):
        raw = bytearray(self.data.read_bytes()); raw[16] ^= 1; self.data.write_bytes(raw)
        self.expected['common.dat'] = (asset.sha(self.data), len(raw))
        with self.assertRaisesRegex(ValueError, 'payload differs'):
            self.run_append()

    def test_duplicate_key_rejected(self):
        rows = copy.copy(self.rows); rows[0] = (asset.KEY, *rows[1][1:])
        self.index.write_bytes(b'\0\2\1' + struct.pack('<Q', 3) + b''.join(asset.RECORD.pack(*r) for r in rows))
        self.expected['common.idx'] = (asset.sha(self.index), self.index.stat().st_size)
        with self.assertRaisesRegex(ValueError, 'one SettingsConfig'):
            self.run_append()

    def test_invalid_geometry_rejected(self):
        self.data.write_bytes(b'truncated')
        self.expected['common.dat'] = (asset.sha(self.data), 9)
        with self.assertRaisesRegex(ValueError, 'geometry'):
            self.run_append()

    def test_existing_output_is_preserved(self):
        self.run_append()
        before = (self.root / 'derived/common.dat').read_bytes()
        with self.assertRaises(FileExistsError):
            self.run_append()
        self.assertEqual((self.root / 'derived/common.dat').read_bytes(), before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
