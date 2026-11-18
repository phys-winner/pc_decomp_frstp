"""Extend the game's existing UTF-16 localization tables without changing old strings."""
import json
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE_LANGUAGES = {845718737: 'de', 984815035: 'fa', 1017381045: 'ko',
                   2014871732: 'en', 2106691904: 'pl', 2480292458: 'es',
                   2624505945: 'pt-BR', 2847863096: 'fa', 2862372073: 'ja',
                   3498510465: 'it', 3896553063: 'tr', 4062391267: 'ru',
                   4172357012: 'fr', 4242838552: 'zh-CN'}
LABELS = ('RenderScale', 'OutputResolution', 'MobileEffects', 'ObjectHighlight',
          'InterfaceScale', 'DeveloperMenu', 'TouchOffset', 'Upscaling', 'Bilinear', 'FrameLimit', 'FrameLimitAuto', 'GestureHelp', 'SwitchInterface',
          'ModelDetail', 'ModelDetailAuto', 'ModelDetailFull', 'ModelDetailHigh', 'ModelDetailMedium', 'ModelDetailLow', 'ModelDetailLowest',
          'BuildingModels', 'FreeBuilding')
PREFIX = 'UI/Android/'
PHOTO_PATH_KEY = 'UI/OtherPanels/PhotoMode/Screenshotpatch'
PHOTO_HELP_KEY = 'UI/OtherPanels/PhotoMode/TooltipCamera'


def translations():
    data = json.loads((ROOT / 'android/port-settings-localization.json').read_text(encoding='utf-8'))
    help_text = json.loads((ROOT / 'android/port-help-localization.json').read_text(encoding='utf-8'))
    if set(data) != set(TABLE_LANGUAGES.values()) or any(len(row) != 23 or not all(row) for row in data.values()):
        raise ValueError('Incomplete port settings translations')
    if set(help_text) != set(data) or any(len(row) != 3 or not all(row) for row in help_text.values()):
        raise ValueError('Incomplete gesture help translations')
    result = {}
    for language, row in data.items():
        values = dict(zip((PREFIX + key for key in LABELS), row[:9] + row[10:], strict=True))
        values.update({PREFIX + 'Offset%d' % value: '%d %s' % (value, row[9]) for value in range(0, 129, 16)})
        values[PREFIX + 'GestureHelpText'], values[PREFIX + 'GestureHelpClose'] = help_text[language][:2]
        result[language] = values
    return result


def parse_table(data):
    if len(data) < 8 or struct.unpack_from('<I', data)[0] != len(data) - 4:
        raise ValueError('Invalid localization table size')
    count, = struct.unpack_from('<I', data, 4)
    position = 8
    rows = {}
    for _ in range(count):
        key_size, = struct.unpack_from('<H', data, position)
        position += 2
        key = data[position:position + key_size].decode('utf-8', 'surrogateescape')
        position += key_size
        value_size, = struct.unpack_from('<H', data, position)
        position += 2
        end = position + value_size * 2
        if end > len(data) or key in rows:
            raise ValueError('Truncated or duplicate localization record')
        rows[key] = data[position:end].decode('utf-16-le', 'surrogatepass')
        position = end
    if position != len(data) or list(rows) != sorted(rows):
        raise ValueError('Localization table has a tail or is not sorted')
    return rows


def encode_table(rows):
    payload = bytearray(struct.pack('<I', len(rows)))
    for key, value in sorted(rows.items()):
        key_bytes = key.encode('utf-8', 'surrogateescape')
        value_bytes = value.encode('utf-16-le', 'surrogatepass')
        payload += struct.pack('<H', len(key_bytes)) + key_bytes
        payload += struct.pack('<H', len(value_bytes) // 2) + value_bytes
    return struct.pack('<I', len(payload)) + payload


def localize_table(data, language):
    before = parse_table(data)
    if encode_table(before) != data:
        raise ValueError('Localization table failed byte-exact round trip')
    added = translations()[language]
    if before.keys() & added.keys():
        raise ValueError('Port localization keys already exist')
    after = {**before, **added}
    after[PHOTO_HELP_KEY] = json.loads((ROOT / 'android/port-help-localization.json').read_text(encoding='utf-8'))[language][2]
    if PHOTO_PATH_KEY in before:
        if before[PHOTO_PATH_KEY].count('{Path}') != 1:
            raise ValueError('Original photo path caption differs')
        after[PHOTO_PATH_KEY] = before[PHOTO_PATH_KEY].replace('{Path}', 'Pictures/Frostpunk')
    else:
        if language != 'fa':
            raise ValueError('Original photo path caption is missing')
        after[PHOTO_PATH_KEY] = 'تصویر شما در این مسیر ذخیره شده است:\nPictures/Frostpunk'
    result = encode_table(after)
    restored = parse_table(result)
    for key in added:
        del restored[key]
    if PHOTO_HELP_KEY in before:
        restored[PHOTO_HELP_KEY] = before[PHOTO_HELP_KEY]
    else:
        del restored[PHOTO_HELP_KEY]
    if PHOTO_PATH_KEY in before:
        restored[PHOTO_PATH_KEY] = before[PHOTO_PATH_KEY]
    else:
        del restored[PHOTO_PATH_KEY]
    if encode_table(restored) != data:
        raise ValueError('Existing localized text changed')
    return result
