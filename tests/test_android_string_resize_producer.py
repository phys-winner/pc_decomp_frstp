"""Source-drift checks for the single string-fill adapter; no compilation/device."""
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import test_android_string_resize_aot as producer


class ProducerTests(unittest.TestCase):
    def test_original_identity_rejects_before_any_output(self):
        with patch.object(producer, 'sha', return_value='0' * 64):
            with self.assertRaisesRegex(ValueError, 'Original executable changed'): producer.generate()

    def test_canonical_identity_rejects(self):
        with patch.object(producer, 'sha', return_value='0' * 64):
            with self.assertRaisesRegex(ValueError, 'Sealed canonical'): producer.body()

    def test_loop_preimage_rejects_even_if_metadata_was_accidentally_updated(self):
        canonical = Mock()
        canonical.name = producer.CANONICAL.name
        canonical.read_text.return_value = producer.CANONICAL.read_text().replace('0x3Fu));', '0x3Eu));')
        expected = producer.sha(producer.CANONICAL)
        with patch.object(producer, 'CANONICAL', canonical), patch.object(producer, 'sha', return_value=expected):
            with self.assertRaisesRegex(ValueError, 'Exact six-instruction loop changed'): producer.body()

    def test_only_loop_and_symbol_differ_from_whole_canonical_body(self):
        source = producer.body()
        text = (ROOT / 'android/host/string_resize_aot.c').read_text()
        expected = source.replace(producer.LOOP, 'L_000140E223E0:\n    fill_growth(c);\n').replace(
            'void L_000140E222E0(', 'static void string_resize(')
        self.assertEqual(text.count(expected), 1)
        self.assertEqual(len(source.split('L_000140E223E0:')), 2)
        self.assertIn('uint64_t written=c->rcx+c->rax;', text)
        self.assertIn('if(written==UINT64_MAX||next!=written+1||((next^written)&~UINT64_C(4095))', text)
        self.assertIn('overlap(next,n,c->rdi,8)', text)
        self.assertIn('overlap(next,n,(uintptr_t)c,sizeof(*c))', text)


if __name__ == '__main__': unittest.main(verbosity=2)
