"""English launcher strings of a generated launcher that lack a translation.

    python -X utf8 tools/launcher_strings.py PREPARED [--write missing.json]

PREPARED is the folder tools/android_launcher.py prepare() wrote (…/launcher/prepared). The shared base
carries en/ru/de/pl; android/launcher-localization.json holds the other languages keyed by the English
text, plus de/pl for the port's own tr(english, russian) strings. Exit code 1 when a string lacks a language.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STRING = r'"((?:[^"\\]|\\.)*)"'
CONCAT = r'((?:"(?:[^"\\]|\\.)*"\s*\+?\s*)+)'
BASE = ('Texts', 'HudField', 'HyperOs')


def joined(expression):
    return ''.join(json.loads('"' + part + '"') for part in re.findall(STRING, expression))


def first_element(row):
    """The first array element of a Java initializer row: up to the first comma outside a string."""
    text, quoted, index = '', False, 0
    while index < len(row):
        char = row[index]
        if quoted:
            text += char
            if char == '\\':
                text += row[index + 1]
                index += 1
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
            text += char
        elif char == ',':
            break
        else:
            text += char
        index += 1
    return joined(text)


def needed(prepared):
    sources = {path.name: path.read_text(encoding='utf-8') for path in Path(prepared).rglob('*.java')}
    need = {}
    texts = sources['Texts.java']
    body = texts[texts.index('TEXTS = {'):texts.index('private static int column')]
    for row in re.finditer(r'\n        \{((?:"(?:[^"\\]|\\.)*"|[^{}"])*)\}', body):
        english = first_element(row.group(1))
        if english.strip():
            need.setdefault(english, 'Texts')
    for match in re.finditer(r'^\s+[A-Z_0-9]+\(' + STRING, sources['HudField.java'], re.M):
        need.setdefault(json.loads('"' + match.group(1) + '"'), 'HudField')
    for match in re.finditer(r'pick\(column,\s*' + CONCAT + ',', sources['HyperOs.java']):
        need.setdefault(joined(match.group(1)), 'HyperOs')
    for name, text in sources.items():
        for match in re.finditer(r'\btr\((?:this|context),\s*' + CONCAT + ',', text):
            need.setdefault(joined(match.group(1)), name)
    for path in (ROOT / 'android/app').glob('*.java'):
        for match in re.finditer(r'\bsay\(\s*' + CONCAT + ',', path.read_text(encoding='utf-8')):
            need.setdefault(joined(match.group(1)), path.name)

    def walk(node):
        if isinstance(node, dict):
            if isinstance(node.get('en'), str):
                need.setdefault(node['en'], 'port.json:' + ','.join(sorted(node)))
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(json.loads((Path(prepared) / 'assets/launcher/port.json').read_text(encoding='utf-8')))
    return need


def missing(prepared):
    words = json.loads((ROOT / 'android/launcher-localization.json').read_text(encoding='utf-8'))
    result = {}
    for english, origin in needed(prepared).items():
        for language, row in words.items():
            if english in row:
                continue
            if language in ('ru', 'de', 'pl') and origin in BASE:
                continue
            if language == 'ru' and origin.endswith('.java'):
                continue
            if origin.startswith('port.json:') and language in origin.split(':')[1].split(','):
                continue
            result.setdefault(english, []).append(language)
    return result


def main():
    lacking = missing(sys.argv[1])
    if '--write' in sys.argv:
        Path(sys.argv[sys.argv.index('--write') + 1]).write_text(
            json.dumps(lacking, ensure_ascii=False, indent=1) + '\n', encoding='utf-8')
    print(len(needed(sys.argv[1])), 'strings;', len(lacking), 'lack a language')
    for english, languages in lacking.items():
        print(len(languages), repr(english)[:160])
    return 1 if lacking else 0


if __name__ == '__main__':
    sys.exit(main())
