"""Read-only PC save metadata inspection; reports omit filenames and city names.

Input is the existing isolated QA seed TAR, never an extracted/mutated Steam folder.
Case numbers are joined by SHA-256 to the QA manifest, not filesystem ordering.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tarfile
import zlib


def u32(data, at):
    return struct.unpack_from('<I', data, at)[0]


def inflated(data, at):
    expected = u32(data, at - 4)
    if expected > 64 * 1024 * 1024:
        raise ValueError('Metadata exceeds inspection limit')
    decoder = zlib.decompressobj()
    result = decoder.decompress(data[at:], expected + 1)
    if not decoder.eof or len(result) != expected:
        raise ValueError('Invalid metadata size or zlib stream')
    return result


def schema_and_root(data):
    """Decode the actual reflected schema and length-delimited first object."""
    if data[:4] != b'\xffgJ\xb1' or u32(data, 4) != 102:
        raise ValueError('Unsupported reflected metadata header')
    schema_count = u32(data, 8)
    if not 1 <= schema_count <= 128:
        raise ValueError('Invalid schema count')
    at, schemas = 12, []
    for _ in range(schema_count):
        fixed_size, count = struct.unpack_from('<IH', data, at)
        at += 6
        fields = []
        if count > 1024:
            raise ValueError('Invalid field count')
        for _ in range(count):
            end = data.index(0, at)
            name = data[at:end].decode('ascii')
            at = end + 1
            field_type, subtype, reference = struct.unpack_from('<HHI', data, at)
            at += 8
            field_size = None
            if fixed_size != 0xffffffff:
                field_size = u32(data, at)
                at += 4
            fields.append((name, field_type, subtype, reference, field_size))
        schemas.append((fixed_size, fields))
    if schemas[0][0] != 0xffffffff or u32(data, at) != 1:
        raise ValueError('Expected one dynamic root object')
    at += 4
    root = schemas[0][1]
    sizes = struct.unpack_from('<' + 'I' * len(root), data, at)
    at += 4 * len(root)
    if at + sum(sizes) != len(data):
        raise ValueError('Root field lengths do not consume metadata')
    result = {}
    for field, size in zip(root, sizes):
        if field[0] in result:
            raise ValueError('Duplicate field name')
        result[field[0]] = data[at:at + size]
        at += size
    return result


def profile_last_game(data):
    root = schema_and_root(inflated(data, 64))
    # Data is a dynamic map of length-delimited (StringId, Variant) pairs.
    content = root['Data']
    count, at, match = u32(content, 0), 4, None
    for _ in range(count):
        key_size, value_size = struct.unpack_from('<II', content, at)
        at += 8
        key = content[at:at + key_size]
        value = content[at + key_size:at + key_size + value_size]
        at += key_size + value_size
        if key == b'\x01LAST_GAME_ID\0':
            if match is not None or len(value) != 21 or value[:5] != b'\x04Guid':
                raise ValueError('Unexpected LAST_GAME_ID variant')
            match = value[5:]
    if at != len(content) or match is None:
        raise ValueError('Invalid profile data map')
    return match


def save_metadata(data):
    if data[:4] != b'\x12\x02\xda\xfe' or u32(data, 4) != 105:
        raise ValueError('Unsupported save header')
    root = schema_and_root(inflated(data, 105))
    scenario_key = root['ScenarioDisplayName']
    scenarios = {
        b'\x01@Generated/ScenariosConfig/Scenarios/DEFAULT/DisplayName\0': 'A New Home',
        b'\x01@Generated/ScenariosConfig/Scenarios/TechHaven/DisplayName\0': 'The Arks',
    }
    if scenario_key not in scenarios or len(root['GameId']) != 16:
        raise ValueError('Unknown scenario or invalid campaign ID')
    # Q32.32 conversion and 300-unit day length match four visible menu entries.
    # This derives the hidden older autosave's day; it is labelled as inferred.
    game_time = struct.unpack('<Q', root['GameTime'])[0]
    return {
        'internal_name': root['GameSaveName'].decode('utf-16-le').rstrip('\0'),
        'campaign_id': root['GameId'],
        'scenario': scenarios[scenario_key],
        'day': game_time // (300 * (1 << 32)) + 1,
        'game_time_q32_32': game_time,
        'game_time_units': game_time / (1 << 32),
        'survivor': root['IronmanMode'] == b'\x01',
        'version': '.'.join(map(str, struct.unpack('<III', root['GameVersion']))),
        'generation': u32(data, 8),
    }


def inspect(backup, manifest):
    by_hash = {row['sha256']: row for row in manifest}
    if len(by_hash) != len(manifest):
        raise ValueError('Duplicate QA input hash')
    found, profile = {}, None
    with tarfile.open(backup, 'r') as archive:
        for member in archive.getmembers():
            if not member.isfile():
                continue
            name = member.name.replace('\\', '/')
            if '/saves/' not in '/' + name and not name.endswith('/ProfileData.bin'):
                continue
            stream = archive.extractfile(member)
            data = stream.read()
            digest = hashlib.sha256(data).hexdigest()
            if name.endswith('/ProfileData.bin'):
                if profile is not None:
                    raise ValueError('Multiple profiles in QA backup')
                profile = {'bytes': len(data), 'sha256': digest,
                           'last_game': profile_last_game(data)}
            elif digest in by_hash:
                expected = by_hash[digest]
                if expected['bytes'] != len(data) or expected['case'] in found:
                    raise ValueError('Save size mismatch or duplicate case')
                base = name.rsplit('/', 1)[-1]
                role = ('autosave_base' if base == 'autosave.save' else
                        'autosave_alternate' if base == 'autosave.save.alt' else 'manual')
                metadata = save_metadata(data)
                internal_name = metadata.pop('internal_name')
                matches_stem = internal_name == base.removesuffix('.alt').removesuffix('.save')
                if not matches_stem:
                    raise ValueError('Metadata name does not match QA source filename')
                found[expected['case']] = {'case': expected['case'], 'bytes': len(data),
                                          'sha256': digest, 'file_role': role,
                                          'metadata_name_matches_file_stem': matches_stem,
                                          **metadata}
    if profile is None or len(found) != len(manifest):
        raise ValueError('QA backup does not contain every case and profile')
    campaigns, cases = {}, []
    for case in sorted(found):
        row = found[case]
        game_id = row.pop('campaign_id')
        if game_id not in campaigns:
            campaigns[game_id] = 'campaign_' + str(len(campaigns))
        row['campaign'] = campaigns[game_id]
        row['profile_resume_match'] = game_id == profile['last_game']
        if row['survivor']:
            row['file_role'] = 'survivor'
        cases.append(row)
    profile.pop('last_game')
    auto = [row for row in cases if row['file_role'].startswith('autosave_')]
    active = max(auto, key=lambda row: row['generation']) if auto else None
    return {'format': 1, 'source': 'isolated PC QA backup; original inputs read only',
            'day_formula': 'floor(GameTime_Q32_32 / (300 * 2^32)) + 1',
            'day_formula_evidence': 'Matches load-menu days for cases 0, 2, 3, 4; case 1 is inferred',
            'profile': profile,
            'profile_resume_cases': [row['case'] for row in cases if row['profile_resume_match']],
            'active_autosave_case': active['case'] if active else None,
            'cases': cases}


def self_check():
    # Real schema/field-size fixture, with no game data or private names.
    header = b'\xffgJ\xb1' + struct.pack('<II', 102, 1)
    schema = struct.pack('<IH', 0xffffffff, 2)
    schema += b'Alpha\0' + struct.pack('<HHI', 13, 0, 0)
    schema += b'Beta\0' + struct.pack('<HHI', 25, 0, 0)
    valid = header + schema + struct.pack('<III', 1, 4, 3) + b'\x01\0\0\0abc'
    assert schema_and_root(valid) == {'Alpha': b'\x01\0\0\0', 'Beta': b'abc'}
    zipped = struct.pack('<I', len(valid)) + zlib.compress(valid)
    assert inflated(zipped, 4) == valid
    for bad in (valid[:-1], valid + b'x', b'invalid'):
        try:
            schema_and_root(bad)
        except (ValueError, struct.error):
            pass
        else:
            raise AssertionError('Truncated or inconsistent metadata accepted')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--output-prefix', type=Path)
    parser.add_argument('--self-check', action='store_true')
    args = parser.parse_args()
    self_check()
    if args.self_check and args.backup is None:
        print('PC save metadata schema self-check PASS')
        return
    if not all((args.backup, args.manifest, args.output_prefix)):
        parser.error('--backup, --manifest and --output-prefix are required')
    before = args.backup.stat()
    report = inspect(args.backup, json.loads(args.manifest.read_text(encoding='utf-8')))
    after = args.backup.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError('Input backup changed during inspection')
    args.output_prefix.with_suffix('.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    rows = ['# PC save case mapping', '',
            'Read-only inspection of the isolated QA copy. Original names, city names and account paths are omitted.', '',
            '| Case | File role | Bytes | Scenario | Day | Survivor | Generation | Resume |',
            '| --- | --- | ---: | --- | ---: | --- | ---: | --- |']
    for row in report['cases']:
        gen = 'manual sentinel' if row['generation'] == 0xffffffff else str(row['generation'])
        rows.append(f"| {row['case']} | {row['file_role']} | {row['bytes']} | {row['scenario']} | {row['day']} | {row['survivor']} | {gen} | {row['profile_resume_match']} |")
    rows.extend(['', 'Profile `LAST_GAME_ID` matches case 4. Resume therefore tests the large manual New Home save, not either autosave.', '',
                 'The standard autosave entry uses case 2 (generation 46), superseding base case 1 (generation 45). Case 1 needs a separate QA filename/copy to test it independently.', '',
                 'Each decoded `GameSaveName` exactly matches its source filename after removing `.save` and optional `.alt`. The two autosave versions contain the same internal name. Profile resume selection uses campaign `LAST_GAME_ID`, not a stored absolute filesystem path.', '',
                 'Day values use `floor(GameTime_Q32_32 / (300 * 2^32)) + 1`. This matches the visible load-menu days for cases 0, 2, 3 and 4. The hidden older case 1 day 2 is inferred and still needs a runtime load.', '',
                 'At the time of this mapping, the parent QA run has loaded case 0 (Survivor, day 1) and case 4 (Resume, day 18). Cases 1, 2 and 3 remain separate runtime checks. Metadata compatibility alone does not prove successful gameplay.', '',
                 'Complete SHA-256 values, version, raw fixed-point time and campaign grouping are in the adjacent JSON.'])
    args.output_prefix.with_suffix('.md').write_text('\n'.join(rows) + '\n', encoding='utf-8')
    print(f"Inspected {len(report['cases'])} unique PC saves; resume case {report['profile_resume_cases']}; active autosave case {report['active_autosave_case']}")


if __name__ == '__main__':
    main()
