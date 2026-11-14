"""Inventory the original serialized UI recipes; never write into a game archive."""
import collections
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import struct

from prepare_raw_archives import entries
from prepare_ui_astc_archives import packed_key

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'build/raw-archives/templates'
OUTPUT = ROOT / 'research/mobile-layout-inventory-2026-11-08'
NODE = re.compile(rb'\x01(UI[A-Za-z0-9_]*Recipe)\x00\x01([^\x00\x01-\x1f]{0,256})\x00')
REFERENCE = re.compile(rb'\x01(UI/(?:Controls|Panels|Menu|Tooltips|Misc|WorldMapPanels)/[A-Za-z0-9_/ .-]+)\x00')
FAMILIES = {
    'selection': ('selection', 'workforce', 'gather', 'workplace', 'resourcepile'),
    'laws': ('bookofrules', 'book_of_rules', 'law', 'sociotech'),
    'photo': ('photo',),
    'top_population': ('resourcespanel', 'resources_panel', 'population', 'citizen',
                       'society_issues', 'society_state', 'homeless', 'top'),
    'construction': ('construction', 'building', 'road'),
    'technology': ('technology', 'research'),
    'dialogs': ('message', 'dilemma', 'tooltip', 'confirmation', 'tutorial'),
    'menus': ('menu', 'settings', 'savegame', 'loadgame'),
}


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def families(text):
    text = text.lower()
    return [family for family, words in FAMILIES.items() if any(word in text for word in words)]


def number(value):
    if math.isnan(value):
        return 'NaN'
    if math.isinf(value):
        return '+Infinity' if value > 0 else '-Infinity'
    return value


def recipes(payload):
    """The tag/name locate a node; decode its shared binary transform and reference fields."""
    matches = list(NODE.finditer(payload))
    result = []
    for index, match in enumerate(matches):
        fields = match.end()
        if fields + 64 > len(payload):
            raise ValueError('Truncated UI recipe transform')
        values = struct.unpack_from('<16f', payload, fields)
        if not all(math.isfinite(value) and abs(value) < 100000 for value in values[8:10]):
            raise ValueError('Invalid UI recipe size')
        node = dict(type=match[1].decode('ascii'), name=match[2].decode('utf-8'),
                    offset=match.start(), transform_offset=fields,
                    position=[number(value) for value in values[:4]],
                    rotation=[number(value) for value in values[4:8]],
                    size=list(values[8:10]), scale=[number(value) for value in values[12:16]])
        anomalies = [offset for offset, value in enumerate(values)
                     if not math.isfinite(value) or abs(value) > 1000000]
        if anomalies:
            node['anomalous_float_indices'] = anomalies
        node['families'] = families(node['name'])
        if node['type'] == 'UIExternRecipe':
            end = matches[index + 1].start() if index + 1 < len(matches) else len(payload)
            references = list(REFERENCE.finditer(payload, fields + 64, end))
            if not references and node['name'] == 'EXT_GAMEOVER_PANEL':
                # The original city template has an empty optional asset path for this disabled shell.
                node['sublayout'] = dict(path='', root='FRAME', offset=None)
                result.append(node)
                continue
            if len(references) != 1:
                raise ValueError('Expected one path/root reference in an extern recipe')
            reference, = references
            root_at = reference.end()
            if payload[root_at] == 0:
                root = ''
            elif payload[root_at] == 1:
                root_end = payload.find(b'\0', root_at + 1, end)
                if root_end < 0:
                    raise ValueError('Truncated extern root name')
                root = payload[root_at + 1:root_end].decode('ascii')
            else:
                raise ValueError('Invalid optional string marker in extern reference')
            node['sublayout'] = dict(path=reference[1].decode('ascii'), root=root, offset=reference.start())
            node['families'] = sorted(set(node['families'] + families(node['sublayout']['path'])))
        result.append(node)
    return result


def self_check(master):
    nodes = {node['name']: node for node in recipes(master)}
    expected = {
        'EXT_SELECTION_PANEL': ([-407, 52, 0, 2], [342, 596], 'UI/Panels/City/SelectionPanel'),
        'EXT_POPULATION_PANEL': ([-261, -506, 0, 1], [367, 135], 'UI/Panels/City/PopulationPanel'),
        'EXT_RESOURCES_PANEL': ([220, -12, 0, 2], [1073, 187], 'UI/Panels/City/ResourcesPanel'),
    }
    for name, (position, size, target) in expected.items():
        node = nodes[name]
        assert node['position'] == position and node['size'] == size
        assert node['sublayout']['path'] == target and node['sublayout']['root'] == 'FRAME'
    sample = b'\x01UIElementRecipe\x00\x01Fixture\x00' + struct.pack('<16f', *range(16))
    parsed, = recipes(sample)
    assert parsed['position'] == [0, 1, 2, 3] and parsed['size'] == [8, 9]
    try:
        recipes(sample[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError('Truncated transform was accepted')
    return dict(success=True, actual_master_transforms=3, synthetic_transform=True, truncated_transform_rejected=True)


def main():
    data_path, index_path = ARCHIVE / 'templates.dat', ARCHIVE / 'templates.idx'
    snapshots = {path: (path.stat().st_size, path.stat().st_mtime_ns) for path in (data_path, index_path)}
    rows = entries(index_path.read_bytes(), data_path.stat().st_size)
    resources = []
    master = None
    with data_path.open('rb') as stream:
        for key, stored, _, offset, flag in rows:
            if flag:
                raise ValueError('Expected a lossless raw templates archive')
            stream.seek(offset)
            payload = stream.read(stored)
            nodes = recipes(payload)
            if not nodes:
                continue
            if key == 1098054804:
                master = payload
            resources.append(dict(key=key, archive_offset=offset, bytes=stored,
                                  resource_guid_bytes=payload[:16].hex(),
                                  root_name=nodes[0]['name'], nodes=nodes))
    if master is None:
        raise ValueError('Original city UI resource is missing')
    check = self_check(master)
    by_key = {resource['key']: resource for resource in resources}
    unresolved = []
    resolved = 0
    for resource in resources:
        for node in resource['nodes']:
            reference = node.get('sublayout')
            if not reference or not reference['path']:
                continue
            key = packed_key(reference['path'].lower() + '.binarytemplate')
            reference['key'] = key
            reference['resolved'] = key in by_key
            if key in by_key:
                resolved += 1
                reference['resource_guid_bytes'] = by_key[key]['resource_guid_bytes']
                paths = by_key[key].setdefault('known_asset_paths', [])
                if reference['path'] not in paths:
                    paths.append(reference['path'])
            else:
                unresolved.append(dict(referencing_resource=resource['key'], node=node['name'], **reference))
    assert packed_key('ui/panels/city/selectionpanel.binarytemplate') == 3004156919
    check['known_sublayout_hash'] = True
    all_nodes = [node for resource in resources for node in resource['nodes']]
    anomalies = [dict(key=resource['key'], node=node) for resource in resources
                 for node in resource['nodes'] if node.get('anomalous_float_indices')]
    externs = [dict(key=resource['key'], node=node) for resource in resources
               for node in resource['nodes'] if node['type'] == 'UIExternRecipe']
    oversized = [dict(key=resource['key'], name=node['name'], offset=node['offset'], size=node['size'])
                 for resource in resources for node in resource['nodes']
                 if node['type'] in ('UIElementRecipe', 'UILayoutRecipe', 'UIExternRecipe', 'UIScrollPaneRecipe')
                 and (node['size'][0] > 1280 or node['size'][1] > 720)]
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(),
                  source={path.relative_to(ROOT).as_posix(): dict(bytes=path.stat().st_size, sha256=sha(path))
                          for path in snapshots},
                  source_read_only=True, self_check=check,
                  decoded_schema=dict(transform_base='byte after the node name NUL',
                                      position_vec4=0, rotation_vec4=16, size_vec4=32, scale_vec4=48,
                                      extern_reference='serialized path string and root name; not an inline GUID',
                                      resource_guid='first sixteen resource bytes, shown in serialized byte order',
                    limitations='Hierarchy, presets, animation tracks and docking flags are not decoded; sizes are asset coordinates, not measured touch targets. EXT_GAMEOVER_PANEL has a serialized empty asset path.'),
                  counts=dict(archive_resources=len(rows), ui_resources=len(resources), nodes=len(all_nodes),
                              extern_references=len(externs), oversized_containers=len(oversized),
                              resolved_extern_references=resolved,
                              anomalous_transforms=len(anomalies),
                              types=dict(collections.Counter(node['type'] for node in all_nodes))),
                  families={family: [row for row in externs if family in row['node']['families']] for family in FAMILIES},
                  unresolved_extern_references=unresolved,
                  anomalous_transforms=anomalies, oversized_containers=oversized, resources=resources)
    for path, snapshot in snapshots.items():
        if (path.stat().st_size, path.stat().st_mtime_ns) != snapshot:
            raise ValueError('Source archive changed during inventory')
    OUTPUT.with_suffix('.json').write_text(json.dumps(report, ensure_ascii=False, indent=1, allow_nan=False) + '\n', encoding='utf-8')
    lines = ['# Mobile UI layout inventory — 2026-11-08', '',
             'Read-only decoding of the prepared, lossless original templates archive. This is an asset inventory; runtime visibility and hit targets require device verification.', '',
             f"- {len(resources)} UI resources; {len(all_nodes)} UI recipe nodes; {len(externs)} path/root references.",
             f'- {resolved} references resolve to an exact resource key/GUID using the original MurmurHash2 of the lowercase path plus `.binarytemplate`; three legacy paths are absent and the game-over shell has an empty path.',
             f'- {len(oversized)} containers exceed the 1280 × 720 reference canvas; these include intentional scrollable trees and backgrounds.',
             f'- {len(anomalies)} transforms contain non-finite or unusually large coordinates in the unchanged source.', '',
             'The decoder reads four floats each at +0 position, +16 rotation, +32 size, +48 scale from the byte after the node-name NUL. Extern references contain an actual sublayout path and root name. Resource GUIDs are recorded in serialized byte order. Hierarchy, docking and animation are not inferred.', '',
             'Self-check: three known city extern transforms and references match exact values; a synthetic transform decodes and a truncated transform is rejected.', '',
             '| Family | Referencing resource | Extern | Sublayout | Target key | Position | Size |',
             '| --- | ---: | --- | --- | --- | --- | --- |']
    for family in FAMILIES:
        seen = set()
        for row in report['families'][family]:
            node = row['node']
            identity = (node['name'], node['sublayout']['path'])
            if identity in seen:
                continue
            seen.add(identity)
            lines.append(f"| {family} | {row['key']} | {node['name']} | {node['sublayout']['path']} | {node['sublayout'].get('key', '')} | {node['position']} | {node['size']} |")
    lines += ['', '## Findings for the runtime adaptation', '',
              '- Selection/resource panels use a 342 × 596 desktop selection shell. Preserve the requested mobile scale for its controls and reflow/scroll its interior when the available viewport is smaller; shrinking the entire shell defeats the setting.',
              '- The resources bar is 1073 × 187 with a baked 0.85 scale; its master position is (220, −12). Scale the whole bar and its children together, then anchor it to the actual top edge.',
              '- Population, citizen search and society issues are independent extern siblings. Correct their anchors as a family; a fix only for the population panel does not reach the issue icons.',
              '- The live laws screen is SociotechPanel (resource 3912184452), with SOCIOTECHTREE and ACTIVATE_SOCIOTECH_BUTTON. The old BookOfRulesPanel extern remains in the master, but its hashed target is absent. Keep viewport, paint mask and camera projection consistent; center the content when opening rather than fitting the whole tree as a dialog.',
              '- The photo screen is an external General/PhotoModePanel; its background fades and controls need distinct full-canvas and mobile-control transforms.',
              '- Oversized trees, masks and backgrounds are intentional in many cases. The JSON identifies each container; it does not treat every large dimension as a bug.', '',
              '| Source anomaly resource | Node | Recipe offset | Position |', '| ---: | --- | ---: | --- |']
    for row in anomalies:
        node = row['node']
        lines.append(f"| {row['key']} | {node['name']} | {node['offset']} | {node['position']} |")
    lines += ['', 'The full JSON includes every resource GUID, resource key, node transform, extern path/root reference and oversized container. No original file was modified.']
    OUTPUT.with_suffix('.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps(report['counts']))


if __name__ == '__main__':
    main()
