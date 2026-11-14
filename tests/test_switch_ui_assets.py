import math
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from prepare_switch_ui_assets import adapt, nodes
from prepare_switch_ui_images import texture, unswizzle


def widget(name, x=1, state=1, kind='UIElementRecipe'):
    return (b'\1' + kind.encode() + b'\0\1' + name.encode() + b'\0' +
            struct.pack('<16f', x, 2, 0, 1, 0, 0, 0, 0, 100, 50, 0, 0, 1, 1, 1, 0) +
            bytes(48) + bytes([state]) * 5 + struct.pack('<I', 0) + b'unchanged callback')


class SwitchUIAssetTest(unittest.TestCase):
    def test_tegra_gob_bit_planes_and_truncation(self):
        source = bytearray(512)
        for offset, value in ((0, 11), (16, 12), (32, 13), (64, 14), (256, 15), (511, 16)):
            source[offset] = value
        linear = unswizzle(source, 64, 8, 1, 1)
        self.assertEqual([linear[y * 64 + x] for x, y in ((0, 0), (0, 1), (16, 0), (0, 2), (32, 0), (63, 7))],
                         [11, 12, 13, 14, 15, 16])
        with self.assertRaises(ValueError):
            unswizzle(source[:-1], 64, 8, 1, 1)

    def test_texture_alpha_and_declared_length(self):
        header = bytearray(28)
        struct.pack_into('<IIIHH', header, 0, 0x1c7865, 1, 1, 1, 1)
        struct.pack_into('<I', header, 24, 512)
        payload = bytes(header) + bytes([79]) + bytes(511)
        self.assertEqual(texture(payload).getpixel((0, 0)), (255, 255, 255, 79))
        with self.assertRaises(ValueError):
            texture(payload[:-1])

    def test_schema_and_callback_bytes_preserved(self):
        pc, switch = widget('BUTTON', 1, 1), widget('BUTTON', 17, 0)
        result, changed, ranges = adapt(pc, switch)
        self.assertEqual(len(result), len(pc))
        at, _, _ = nodes(pc)[((b'UIElementRecipe', b'BUTTON'),)][0]
        self.assertEqual(struct.unpack_from('<f', result, at)[0], 17)
        self.assertEqual(result[at + 112:], pc[at + 112:])
        self.assertEqual(changed, ['UIElementRecipe:BUTTON'])
        self.assertEqual(ranges, [(at, 112)])

    def test_ambiguous_new_types_and_animated_nan_are_not_transplanted(self):
        for source in (widget('BUTTON', 2) * 2,
                       widget('BUTTON', 2, kind='UIRoundedRectRecipe'), widget('BUTTON', math.nan)):
            pc = widget('BUTTON')
            self.assertEqual(adapt(pc, source), (pc, [], []))
        with self.assertRaises(ValueError):
            nodes(widget('BUTTON')[:-40])

    def test_same_name_under_a_different_parent_is_not_transplanted(self):
        def parent(name, child):
            return widget(name)[:-len(b'unchanged callback')] + struct.pack('<I', 1) + child
        pc = parent('FRAME', parent('PC_CONTAINER', widget('BUTTON', 1)))
        switch = parent('FRAME', parent('SWITCH_CONTAINER', widget('BUTTON', 99)))
        self.assertEqual(adapt(pc, switch), (pc, [], []))

    def test_law_prototypes_keep_their_pc_camera_geometry(self):
        def parent(name, child, x=1):
            return widget(name, x)[:-len(b'unchanged callback')] + struct.pack('<I', 1) + child
        pc = parent('SOCIOTECHTREE', parent('TREE_PANEL', widget('TECH_ENTRY', 1)))
        switch = parent('SOCIOTECHTREE', parent('TREE_PANEL', widget('TECH_ENTRY', 9), 7), 5)
        result, changed, _ = adapt(pc, switch)
        self.assertEqual(changed, ['UIElementRecipe:SOCIOTECHTREE'])
        for key, locations in nodes(pc).items():
            if key[-1][1] in (b'TREE_PANEL', b'TECH_ENTRY'):
                at, end, _ = locations[0]
                self.assertEqual(result[at:end], pc[at:end])

    def test_preset_transform_keeps_its_action_flag_and_copies_the_last_float(self):
        def resource(flag, x, last):
            base = widget('BUTTON')[:-len(b'unchanged callback') - 4]
            preset = (b'\1UIElementPreset\0\1ENABLED\0' + bytes([flag]) +
                      struct.pack('<12f', x, 2, 0, 1, 100, 50, 0, 0, 1, 1, 1, last))
            return base + struct.pack('<I', 1) + preset + b'unchanged callback'
        pc, switch = resource(0, 1, 0), resource(1, 17, .25)
        result, changed, ranges = adapt(pc, switch)
        at = result.index(b'ENABLED\0') + len(b'ENABLED\0')
        self.assertEqual(result[at], 0)
        self.assertEqual(struct.unpack_from('<12f', result, at + 1), (17, 2, 0, 1, 100, 50, 0, 0, 1, 1, 1, .25))
        self.assertEqual(ranges, [(at + 1, 48)])
        self.assertTrue(result.endswith(b'unchanged callback'))


if __name__ == '__main__':
    unittest.main()
