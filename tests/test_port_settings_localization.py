import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from port_settings_localization import (LABELS, LANGUAGES, PHOTO_HELP_KEY, PHOTO_PATH_KEY, PREFIX, TABLE_LANGUAGES, encode_table,
                                        localize_table, parse_table, translations)
from prepare_android_port_settings_asset import GENERAL, GRAPHICS, KEY, ROWS, SOURCE, resource
from prepare_raw_archives import entries
from android_launcher import game_input_manifest
from liquid_archive import Archive, RECORD, packed_key
from package_game_data import resource_patch


class PortSettingsLocalizationTest(unittest.TestCase):
    def test_language_archive_adds_both_fan_entries_alongside_english(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = root / 'localizations.dat'
            data.write_bytes(b'English')
            idx = root / 'localizations.idx'
            idx.write_bytes(b'\x00\x02\x01' + struct.pack('<Q', 1) +
                            RECORD.pack(packed_key('english.lang'), 7, 7, 0, 0))
            additions = {packed_key('czech.lang'): b'Czech text', packed_key('hungarian.lang'): b'Hungarian text'}
            with patch('package_game_data.STEAM', root):
                folder, _ = resource_patch('port-ui', {'localizations': additions}, root, private=True)
            idx.write_bytes((folder / 'port-ui.localizations.idx').read_bytes())
            with data.open('ab') as stream:
                stream.write((folder / 'port-ui.localizations.append').read_bytes())
            result = Archive(root / 'localizations')
            self.assertEqual(list(result.rows), sorted({packed_key('english.lang'), *additions}))
            self.assertEqual(result.read('english.lang'), b'English')
            self.assertEqual(result.read('czech.lang'), b'Czech text')
            self.assertEqual(result.read('hungarian.lang'), b'Hungarian text')

    def test_native_input_manifest_accepts_each_localization_variant(self):
        def sizes(text, suffix):
            return next(set(row.split('\t')[0].split(',')) for row in text.splitlines()
                        if row.endswith('\tlocalizations' + suffix))

        original = game_input_manifest()
        variants = [{'archives': [{'stem': 'localizations', 'output': {'.dat': {'bytes': 9871234}, '.idx': {'bytes': 649}}}]}]
        localized = game_input_manifest(variants)
        self.assertEqual(sizes(localized, '.dat'), sizes(original, '.dat') | {'9871234'})
        self.assertEqual(sizes(localized, '.idx'), sizes(original, '.idx') | {'649'})

    def test_utf16_lengths_and_old_text_roundtrip(self):
        original = encode_table({'A': 'Snow \U0001f328', PHOTO_PATH_KEY: '写真: {Path}',
                                 PHOTO_HELP_KEY: 'Original desktop help', 'Z': '\ud800'})
        result = localize_table(original, 'ja')
        after = parse_table(result)
        self.assertEqual(after['A'], 'Snow \U0001f328')
        self.assertEqual(after['Z'], '\ud800')
        self.assertEqual(after[PREFIX + 'InterfaceScale'], 'インターフェース倍率')
        self.assertEqual(len(after), 4 + 33)
        self.assertEqual(after[PHOTO_PATH_KEY], '写真: Pictures/Frostpunk')
        help_text = json.loads((ROOT / 'android/port-help-localization.json').read_text(encoding='utf-8'))
        self.assertEqual(after[PHOTO_HELP_KEY], help_text['ja'][2])
        self.assertEqual(after[PREFIX + 'GestureHelpText'], help_text['ja'][0])
        self.assertEqual(after[PREFIX + 'GestureHelpClose'], help_text['ja'][1])
        with self.assertRaises(ValueError):
            localize_table(result, 'ja')
        with self.assertRaises(ValueError):
            parse_table(original[:-1])
        with self.assertRaises(ValueError):
            localize_table(encode_table({'A': 'Preserve me', PHOTO_PATH_KEY: 'Missing placeholder'}), 'ja')
        with self.assertRaises(ValueError):
            localize_table(encode_table({'A': 'Preserve me', PHOTO_PATH_KEY: '{Path} {Path}'}), 'ja')

    def test_all_setting_labels_and_options_resolve_in_every_language(self):
        all_translations = translations()
        expected_labels = ('RenderScale', 'OutputResolution', 'MobileEffects', 'ObjectHighlight',
                           'InterfaceScale', 'DeveloperMenu', 'TouchOffset', 'Upscaling', 'Bilinear',
                           'FrameLimit', 'FrameLimitAuto', 'GestureHelp', 'SwitchInterface',
                           'ModelDetail', 'ModelDetailAuto', 'ModelDetailFull', 'ModelDetailHigh',
                           'ModelDetailMedium', 'ModelDetailLow', 'ModelDetailLowest', 'BuildingModels',
                           'FreeBuilding')
        self.assertEqual(LABELS, expected_labels)
        expected_keys = {PREFIX + key for key in expected_labels}
        expected_keys.update(PREFIX + 'Offset%d' % value for value in range(0, 129, 16))
        expected_keys.update((PREFIX + 'GestureHelpText', PREFIX + 'GestureHelpClose'))
        self.assertEqual(len(expected_keys), 33)
        required = {text[1:] for _, text, _ in ROWS}
        required.add(PREFIX + LABELS[0])
        required.update(name[1:] for _, _, options in ROWS for _, name in options
                        if name.startswith('@UI/Android/'))
        self.assertEqual(set(all_translations), LANGUAGES)
        for language, values in all_translations.items():
            self.assertTrue(required <= values.keys(), language)
            self.assertEqual(set(values), expected_keys, language)
            self.assertTrue(all(isinstance(value, str) and value for value in values.values()), language)

    def test_actual_game_tables_preserve_every_old_string(self):
        archive = ROOT / 'build/raw-archives/localizations'
        data = (archive / 'localizations.dat').read_bytes()
        all_translations = translations()
        help_text = json.loads((ROOT / 'android/port-help-localization.json').read_text(encoding='utf-8'))
        seen = set()
        for key, size, _, offset, _ in entries((archive / 'localizations.idx').read_bytes(), len(data)):
            if key not in TABLE_LANGUAGES:
                continue
            original = data[offset:offset + size]
            result = localize_table(original, TABLE_LANGUAGES[key])
            before, after = parse_table(original), parse_table(result)
            language = TABLE_LANGUAGES[key]
            allowed_changes = {PHOTO_PATH_KEY, PHOTO_HELP_KEY}
            self.assertEqual({name: value for name, value in before.items() if name not in allowed_changes},
                             {name: after[name] for name in before if name not in allowed_changes})
            expected_keys = set(before) | set(all_translations[language]) | allowed_changes
            self.assertEqual(set(after), expected_keys)
            for name, value in all_translations[language].items():
                self.assertEqual(after[name], value)
            self.assertEqual(after[PHOTO_HELP_KEY], help_text[language][2])
            self.assertIn('Pictures/Frostpunk', after[PHOTO_PATH_KEY])
            self.assertNotIn('{Path}', after[PHOTO_PATH_KEY])
            if PHOTO_PATH_KEY in before:
                self.assertEqual(after[PHOTO_PATH_KEY], before[PHOTO_PATH_KEY].replace('{Path}', 'Pictures/Frostpunk'))
            else:
                self.assertEqual(language, 'fa')
                self.assertEqual(after[PHOTO_PATH_KEY], 'تصویر شما در این مسیر ذخیره شده است:\nPictures/Frostpunk')
            restored = {name: value for name, value in after.items() if name not in all_translations[language]}
            for name in allowed_changes:
                if name in before:
                    restored[name] = before[name]
                else:
                    del restored[name]
            self.assertEqual(encode_table(restored), original)
            seen.add(key)
        self.assertEqual(seen, set(TABLE_LANGUAGES))

    def test_render_scale_and_new_rows_use_original_localization_handlers(self):
        index = (SOURCE / 'common.idx').read_bytes()
        data_path = SOURCE / 'common.dat'
        selected = [row for row in entries(index, data_path.stat().st_size) if row[0] == KEY]
        self.assertEqual(len(selected), 1)
        row, = selected
        with data_path.open('rb') as stream:
            stream.seek(row[3])
            original = stream.read(row[1])
        result, insert, inserted_bytes = resource(original)
        self.assertEqual([number for number, _, _ in ROWS], list(range(21, 34)))
        self.assertEqual(struct.unpack_from('<I', result, 0x160)[0], 32)
        self.assertNotIn(b'\1Render scale\0', result)
        self.assertIn(b'\1@UI/Android/RenderScale\0', result)
        for _, label, _ in ROWS:
            self.assertEqual(result.count(b'\1' + label.encode() + b'\0'), 1)
        by_id = {number: (number, label, options) for number, label, options in ROWS}
        self.assertEqual(by_id[28], (28, '@UI/Android/FrameLimit', [(0, '@UI/Android/FrameLimitAuto')]))
        self.assertEqual(by_id[29], (29, '@UI/Android/GestureHelp', [(0, '@UI/Menu/Settings/Off'), (1, '@UI/Menu/Settings/On')]))
        self.assertEqual(by_id[30][0], 30)
        self.assertEqual(by_id[31][1], '@UI/Android/ModelDetail')
        self.assertEqual([value for value, _ in by_id[31][2]], [0, 1, 2, 3, 4, 5])
        self.assertEqual(by_id[32][1], '@UI/Android/BuildingModels')
        self.assertEqual([value for value, _ in by_id[32][2]], [0, 1, 2, 3, 4])
        self.assertEqual(by_id[33], (33, '@UI/Android/FreeBuilding', [(0, '@UI/Menu/Settings/Off'), (1, '@UI/Menu/Settings/On')]))
        self.assertEqual(result.count(b'\1@UI/Android/FrameLimitAuto\0'), 1)
        head = b'\1FrostpunkSettingCombo\0'
        for number, label, _ in ROWS:
            row_head = b'\1FrostpunkSettingYesNo\0' if number in (30, 33) else head
            row_start = result.index(row_head + struct.pack('<II', number, 0) + b'\1' + label.encode() + b'\0')
            category_at = row_start + len(row_head) + 8 + len(label) + 2 + 4
            self.assertEqual(result[category_at:category_at + 16], GENERAL if number in (24, 25, 26, 29, 30, 33) else GRAPHICS)
        unlocalized = result.replace(b'\1@UI/Android/RenderScale\0', b'\1Render scale\0')
        restored = bytearray(unlocalized[:insert] + unlocalized[insert + inserted_bytes:])
        struct.pack_into('<I', restored, 0x160, 19)
        self.assertEqual(bytes(restored), original)


if __name__ == '__main__':
    unittest.main()
