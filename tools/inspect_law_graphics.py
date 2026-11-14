"""Read-only evidence for the original law-tree ornament and converted state atlas."""
import hashlib
import json
from pathlib import Path
import re
import sys
import zlib

import numpy as np
import texture2ddecoder

from inspect_mobile_layouts import NODE
from prepare_raw_archives import entries
from prepare_ui_astc_archives import packed_key

ROOT = Path(__file__).resolve().parents[1]


def resource(folder, archive, key):
    data_path, index_path = folder / (archive + '.dat'), folder / (archive + '.idx')
    rows = [row for row in entries(index_path.read_bytes(), data_path.stat().st_size) if row[0] == key]
    if len(rows) != 1:
        raise ValueError('Missing or duplicate law graphics resource')
    row, = rows
    with data_path.open('rb') as stream:
        stream.seek(row[3])
        data = stream.read(row[1])
    if row[4]:
        inflater = zlib.decompressobj(31)
        data = inflater.decompress(data) + inflater.flush()
    if len(data) != row[2]:
        raise ValueError('Law graphics resource size differs')
    return data


def strings(data):
    return [match.decode('ascii') for match in re.findall(rb'[A-Za-z][A-Za-z0-9_\\/ .-]{5,}', data)]


def main():
    template_folder = ROOT / 'build/raw-archives/templates'
    panel = resource(template_folder, 'templates', 3912184452)
    matches = list(NODE.finditer(panel))
    views = []
    for index, match in enumerate(matches):
        if match[1] != b'UITemplateViewRecipe':
            continue
        end = matches[index + 1].start() if index + 1 < len(matches) else len(panel)
        body = panel[match.end():end]
        views.append(dict(name=match[2].decode(), offset=match.start(),
                          template_paths=[value for value in strings(body) if value.startswith('Graphics/')]))
    selected = [row for row in views if row['name'] == 'SELECTED_TEMPLATE_VIEW']
    assert len(selected) == 4 and all(row['template_paths'] for row in selected)
    effects = []
    for path in sorted({path for row in selected for path in row['template_paths']}):
        key = packed_key(path.lower() + '.binarytemplate')
        data = resource(template_folder, 'templates', key)
        names = strings(data)
        textures = sorted({name for name in names if name.endswith('.dds')})
        assert any(name.endswith('light_node.dds') for name in textures)
        assert any(name.endswith('dots2_node.dds') for name in textures)
        effects.append(dict(path=path, key=key, bytes=len(data), textures=textures,
                            sha256=hashlib.sha256(data).hexdigest()))
    config = json.loads((ROOT / 'port.json').read_text())
    original = resource(Path(config['game_directory']), 'textures-s3', 3010673867)
    converted = resource(ROOT / 'build/ui-astc6-archives/textures-s3', 'textures-s3', 3010673867)
    assert original[:8] == converted[:8] == bytes.fromhex('0008000000080000')
    assert original[8:12] == b'DXT5' and converted[8:12] == b'AST6'
    source_rgba = np.frombuffer(texture2ddecoder.decode_bc3(original[144:], 2048, 2048), dtype=np.uint8).reshape(2048, 2048, 4)
    derived_rgba = np.frombuffer(texture2ddecoder.decode_astc(converted[144:], 2048, 2048, 6, 6), dtype=np.uint8).reshape(2048, 2048, 4)
    difference = np.abs(source_rgba.astype(np.int16) - derived_rgba.astype(np.int16))
    alpha_mae = float(difference[:, :, 3].mean())
    assert alpha_mae < 1, 'The law-state alpha atlas lost its original coverage'
    report = dict(success=True, original_files_read_only=True,
                  panel=dict(key=3912184452, sha256=hashlib.sha256(panel).hexdigest(),
                             selected_template_views=selected, effect_resources=effects),
                  state_atlas=dict(path='ui/sociotech/sociotechtreenodesatlas.texture', key=3010673867,
                                   width=2048, height=2048, original_format='DXT5', derived_format='ASTC6',
                                   original_sha256=hashlib.sha256(original).hexdigest(),
                                   derived_sha256=hashlib.sha256(converted).hexdigest(),
                                   rgba_mae=difference.mean(axis=(0, 1)).tolist(), alpha_mae=alpha_mae,
                                   alpha_p99=float(np.quantile(difference[:, :, 3], .99))),
                  interpretation='SELECTED_TEMPLATE_VIEW is the original selection glow effect, not a per-law pictogram. The source state atlas and derived alpha coverage do not demonstrate a missing icon texture.',
                  unresolved='Runtime draw/paint-template state and an original-PC visual comparison are required before classifying empty law circles as a graphics defect. Terrain corruption is a separate unconfirmed cause; no graphics default was changed.')
    output = ROOT / 'research/law-graphics-evidence-2026-11-08.json'
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, effect_resources=len(effects), alpha_mae=alpha_mae)))


if __name__ == '__main__':
    main()
