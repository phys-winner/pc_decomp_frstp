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

    def test_every_text_the_launcher_asks_for_exists(self):
        # 0.3.2-dev shipped profileProblem() keys without their texts: the launcher closed on every start.
        words = json.loads((ROOT / 'android/launcher-compatibility-localization.json').read_text(encoding='utf-8'))
        asked = set()
        for path in sorted((ROOT / 'android/launcher').glob('*.java*')):
            source = path.read_text(encoding='utf-8')
            asked.update(re.findall(r'\btext\(\s*\w+\s*,\s*"(\w+)"', source))
            problem = re.search(r'static String profileProblem\(.*?\n    }\n', source, re.S)
            if problem:
                asked.update(re.findall(r'return "(\w+)";', problem.group(0)))
        self.assertTrue({'driver_turnip', 'driver_panvk', 'no_texture_format', 'features'} <= asked, asked)
        # The Qualcomm system driver is judged like any other since 24 November 2026: no port-policy reason.
        self.assertNotIn('driver_qualcomm', asked)
        self.assertNotIn('driver_qualcomm', words['en'])
        self.assertEqual(asked - set(words['en']), set())

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
                self.assertEqual(text.count('\n'), english.count('\n'), (language, english))

    def test_no_launcher_string_left_in_english(self):
        # 24 November 2026: the 26 shared-base strings and the rest are translated in every language the port ships.
        from launcher_strings import english_left
        words = json.loads((ROOT / 'android/launcher-localization.json').read_text(encoding='utf-8'))
        self.assertEqual(english_left(words), {})
        self.assertFalse((ROOT / 'android/launcher-untranslated.json').exists())


if __name__ == '__main__':
    unittest.main()
