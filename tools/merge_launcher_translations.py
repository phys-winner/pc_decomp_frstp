"""Merges translation batches into android/launcher-localization.json.

    python -X utf8 tools/merge_launcher_translations.py PREPARED INDEX.json BATCH.json [BATCH.json ...]

INDEX.json is the output of `launcher_strings.py --write` the batches were numbered against (the n-th key is
index n). A batch is {language: {index: translation}}. ru/de/pl rows of strings the shared base or the
port's Java already carries are copied from the generated sources (PREPARED), so that all rows keep one
key set. Strings a language still lacks (or still shows in English, launcher_strings.english_left) are
listed in android/launcher-untranslated.json: the launcher shows them in English, and that file is the list
to translate. With nothing left the file is removed.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import launcher_strings as strings  # noqa: E402

FORMAT = re.compile(r'%(?:\d+\$)?(?:\d+(?:\.\d+)?)?[sd%]')


def elements(row):
    """Top-level comma separated elements of a Java initializer row, each a joined string."""
    parts, text, quoted, depth, index = [], '', False, 0, 0
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
        elif char == '(':
            depth += 1
            text += char
        elif char == ')':
            depth -= 1
            text += char
        elif char == ',' and depth == 0:
            parts.append(strings.joined(text))
            text = ''
        else:
            text += char
        index += 1
    parts.append(strings.joined(text))
    return parts


def base_rows(prepared):
    """english -> {'ru':…, 'de':…, 'pl':…} from the generated sources and port.json."""
    sources = {path.name: path.read_text(encoding='utf-8') for path in Path(prepared).rglob('*.java')}
    rows = {}

    def add(parts):
        if len(parts) >= 4 and parts[0].strip():
            rows.setdefault(parts[0], dict(zip(('ru', 'de', 'pl'), parts[1:4])))
    texts = sources['Texts.java']
    body = texts[texts.index('TEXTS = {'):texts.index('private static int column')]
    for row in re.finditer(r'\n        \{((?:"(?:[^"\\]|\\.)*"|[^{}"])*)\}', body):
        add(elements(row.group(1)))
    for match in re.finditer(r'^\s+[A-Z_0-9]+\(((?:"(?:[^"\\]|\\.)*"|[^()"])*)\)[,;]', sources['HudField.java'], re.M):
        add(elements(match.group(1)))
    for match in re.finditer(r'pick\(column,((?:"(?:[^"\\]|\\.)*"|[^()";])*)\);', sources['HyperOs.java']):
        add(elements(match.group(1)))
    for text in sources.values():
        for match in re.finditer(r'\btr\((?:this|context),\s*' + strings.CONCAT + r',\s*' + strings.CONCAT + r'\)', text):
            rows.setdefault(strings.joined(match.group(1)), {})['ru'] = strings.joined(match.group(2))
    for path in (ROOT / 'android/app').glob('*.java'):
        for match in re.finditer(r'\bsay\(\s*' + strings.CONCAT + r',\s*' + strings.CONCAT + r'\)', path.read_text(encoding='utf-8')):
            rows.setdefault(strings.joined(match.group(1)), {})['ru'] = strings.joined(match.group(2))

    def walk(node):
        if isinstance(node, dict):
            if isinstance(node.get('en'), str):
                for language in ('ru', 'de', 'pl'):
                    if isinstance(node.get(language), str):
                        rows.setdefault(node['en'], {}).setdefault(language, node[language])
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(json.loads((Path(prepared) / 'assets/launcher/port.json').read_text(encoding='utf-8')))
    return rows


def main():
    prepared = sys.argv[1]
    keys = list(json.loads(Path(sys.argv[2]).read_text(encoding='utf-8')))
    path = ROOT / 'android/launcher-localization.json'
    words = json.loads(path.read_text(encoding='utf-8'))
    added = {language: 0 for language in words}
    for batch in sys.argv[3:]:
        for language, row in json.loads(Path(batch).read_text(encoding='utf-8')).items():
            for index, text in row.items():
                english = keys[int(index)]
                assert text.strip() and sorted(FORMAT.findall(text)) == sorted(FORMAT.findall(english)), (language, english, text)
                assert english.count('\n') == text.count('\n'), (language, english)
                if words[language].get(english) != text:
                    words[language][english] = text
                    added[language] += 1
    base = base_rows(prepared)
    need = strings.needed(prepared)
    untranslated = {}
    for english in need:
        for language, row in words.items():
            if english in row:
                continue
            known = base.get(english, {}).get(language)
            if known:
                row[english] = known
                added[language] += 1
            else:
                # English in the app until translated; listed for the translator.
                untranslated.setdefault(language, []).append(english)
    everything = set().union(*(set(row) for row in words.values()))
    for language, row in words.items():
        for english in sorted(everything - set(row)):
            known = base.get(english, {}).get(language)
            if known:
                row[english] = known
            else:
                row[english] = english
                if english not in untranslated.get(language, []):
                    untranslated.setdefault(language, []).append(english)
    for english in need:
        for language, row in words.items():
            if english not in row:
                row[english] = english
    path.write_text(json.dumps(words, ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    for language, rows in strings.english_left(words).items():
        for english in rows:
            if english not in untranslated.get(language, []):
                untranslated.setdefault(language, []).append(english)
    pending = ROOT / 'android/launcher-untranslated.json'
    if untranslated:
        pending.write_text(json.dumps({language: sorted(rows) for language, rows in sorted(untranslated.items())},
                                      ensure_ascii=False, indent=2) + '\n', encoding='utf-8', newline='\n')
    elif pending.exists():
        pending.unlink()  # every string has its translation (24 November 2026)
    print('added', added)
    print('untranslated (English shown)', {language: len(rows) for language, rows in untranslated.items()})


if __name__ == '__main__':
    main()
