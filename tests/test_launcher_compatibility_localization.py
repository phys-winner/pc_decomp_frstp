import json
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from port_settings_localization import LANGUAGES


class CompatibilityLocalizationTest(unittest.TestCase):
    def test_all_game_languages_and_format_arguments(self):
        words = json.loads((ROOT / 'android/launcher-compatibility-localization.json').read_text(encoding='utf-8'))
        self.assertEqual(set(words), LANGUAGES)
        for language, row in words.items():
            self.assertEqual(set(row), set(words['en']), language)
            for key, value in row.items():
                self.assertTrue(value.strip(), (language, key))
                self.assertEqual(re.findall(r'%\w', value), re.findall(r'%\w', words['en'][key]), (language, key))
                if language != 'en':
                    self.assertNotEqual(value, words['en'][key], (language, key))

    def test_launcher_settings_and_format_arguments(self):
        spec = json.loads((ROOT / 'android/port.json').read_text(encoding='utf-8'))
        words = json.loads((ROOT / 'android/launcher-localization.json').read_text(encoding='utf-8'))
        self.assertEqual(set(spec['app']['languages']), LANGUAGES)
        self.assertEqual(set(words), LANGUAGES - {'en'})
        settings = {'Options', 'Language', 'Advanced graphics settings', 'Game language', 'Texture quality',
                    'Czech (community translation)', 'Hungarian (community translation)'}
        for option in spec['options']:
            settings.update(option[field]['en'] for field in ('title', 'hint') if field in option)
            settings.update(choice['title']['en'] for choice in option.get('choices', []))
        formats = re.compile(r'%(?:\d+\$)?(?:\d+(?:\.\d+)?)?[sd%]')
        for language, row in words.items():
            self.assertTrue(settings <= row.keys(), language)
            self.assertEqual(row.keys(), words['cs'].keys(), language)
            for english, text in row.items():
                self.assertTrue(text.strip(), (language, english))
                self.assertEqual(sorted(formats.findall(text)), sorted(formats.findall(english)), (language, english))


if __name__ == '__main__':
    unittest.main()
