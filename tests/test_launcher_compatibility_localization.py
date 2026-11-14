import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from port_settings_localization import TABLE_LANGUAGES


class CompatibilityLocalizationTest(unittest.TestCase):
    def test_all_game_languages_and_format_arguments(self):
        words = json.loads((ROOT / 'android/launcher-compatibility-localization.json').read_text(encoding='utf-8'))
        self.assertEqual(set(words), set(TABLE_LANGUAGES.values()))
        for language, row in words.items():
            self.assertEqual(set(row), set(words['en']), language)
            for key, value in row.items():
                self.assertTrue(value.strip(), (language, key))
                self.assertEqual(re.findall(r'%\w', value), re.findall(r'%\w', words['en'][key]), (language, key))
                if language != 'en':
                    self.assertNotEqual(value, words['en'][key], (language, key))


if __name__ == '__main__':
    unittest.main()
