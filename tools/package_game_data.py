"""Package the original Steam files and small, separately installable resource patches."""
import hashlib
import json
from pathlib import Path
import struct
import time
import zipfile
import zlib

from liquid_archive import Archive, RECORD

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/public-data-20261114'
PUB = ROOT / 'pub'
STEAM = Path(json.loads((ROOT / 'port.json').read_text())['game_directory'])
HOST = json.loads((ROOT / 'research/game-host-data-port-settings.json').read_text())


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    text = json.dumps(value, indent=2, sort_keys=True) + '\n'
    if not path.exists() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def identify(path):
    return dict(bytes=path.stat().st_size, sha256=sha(path))


def packed(payload):
    compressor = zlib.compressobj(9, zlib.DEFLATED, 31)
    result = compressor.compress(payload) + compressor.flush(zlib.Z_SYNC_FLUSH)
    return (result, 1) if len(result) * 100 <= len(payload) * 85 else (payload, 0)


def resource_patch(kind, changes, output=None):
    folder = (output or OUT / 'bundle-resources') / kind
    folder.mkdir(parents=True, exist_ok=True)
    manifest = dict(schema=1, kind=kind, original_exe_sha256=HOST['original_exe_sha256'], archives=[], payloads={})
    for stem, replacements in changes.items():
        original = Archive(STEAM / stem)
        index = bytearray((STEAM / (stem + '.idx')).read_bytes())
        alternate = bytearray(index)
        tail = bytearray()
        offset = (STEAM / (stem + '.dat')).stat().st_size
        for number, row in enumerate(RECORD.iter_unpack(index[11:])):
            key = row[0]
            if key not in replacements:
                continue
            payload = replacements[key]
            assert payload != original.read(key), 'A patch must change its resource'
            stored, flag = packed(payload)
            RECORD.pack_into(alternate, 11 + number * RECORD.size, key, len(stored), len(payload), offset + len(tail), flag)
            tail.extend(stored)
        assert len(replacements) == sum(a != b for a, b in zip(RECORD.iter_unpack(index[11:]), RECORD.iter_unpack(alternate[11:]), strict=True))
        append_name = kind + '.' + stem + '.append'
        index_name = kind + '.' + stem + ('-switch.idx' if kind == 'switch-ui' else '.idx')
        (folder / append_name).write_bytes(tail)
        (folder / index_name).write_bytes(alternate)
        digest = hashlib.sha256()
        with (STEAM / (stem + '.dat')).open('rb') as source:
            while block := source.read(8 << 20):
                digest.update(block)
        digest.update(tail)
        manifest['archives'].append(dict(stem=stem, append=append_name, index=index_name,
            base={suffix: identify(STEAM / (stem + suffix)) for suffix in ('.dat', '.idx')},
            output={'.dat': dict(bytes=offset + len(tail), sha256=digest.hexdigest()), '.idx': identify(folder / index_name)},
            replaced_entries=len(replacements)))
    if kind == 'switch-ui':
        for source in sorted((ROOT / 'android/switch-ui').glob('*.png')):
            (folder / ('switch-ui.' + source.name)).write_bytes(source.read_bytes())
    manifest['payloads'] = {path.name: identify(path)
                            for path in sorted(folder.rglob('*')) if path.is_file() and not path.name.endswith('.json')}
    marker = kind + '.json'
    write_json(folder / marker, manifest)
    write_json(ROOT / ('android/game-data-' + kind + '.json'), manifest)
    return folder, manifest


def port_ui_patch(output):
    from prepare_android_port_settings_asset import resource, SOURCE, KEY
    from port_settings_localization import localize_table, TABLE_LANGUAGES
    original = Archive(STEAM / 'localizations')
    setting = resource(Archive(SOURCE / 'common').read(KEY))[0]
    return resource_patch('port-ui', {
        'common': {KEY: setting},
        'localizations': {key: localize_table(original.read(key), language)
                          for key, language in TABLE_LANGUAGES.items()}}, output)


def package(sources, expected, originals):
    final = PUB / 'Frostpunk-data.zip'
    assert not final.exists(), 'Preserve an existing distribution'
    cache = OUT / 'steam-compression.json'
    source_pin = hashlib.sha256(json.dumps(originals, sort_keys=True).encode()).hexdigest()
    if not cache.exists() and (PUB / 'Frostpunk-packaging.json').exists():
        prior = json.loads((PUB / 'Frostpunk-packaging.json').read_text())
        base = next((row for row in prior.get('archives', []) if row['file'] == 'Frostpunk-Steam-data.zip'), None)
        if base:
            write_json(cache, dict(source_manifest_sha256=source_pin, deflate_bytes=base['measured_deflate_payload_bytes']))
    measured = json.loads(cache.read_text()) if cache.exists() else None
    assert measured is None or measured['source_manifest_sha256'] == source_pin
    trial_bytes = measured['deflate_bytes'] if measured else 0
    raw_bytes = sum(row['bytes'] for row in expected.values())
    stored = OUT / 'bundle.stored.zip'
    base_trial = 0
    started = time.monotonic()
    with zipfile.ZipFile(stored, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        for member, source in sources.items():
            before = source.stat(); digest = hashlib.sha256(); done = 0
            compressor = None if measured and member in originals else zlib.compressobj(9, zlib.DEFLATED, -15)
            compressed_bytes = 0
            info = zipfile.ZipInfo.from_file(source, member)
            with source.open('rb') as stream, archive.open(info, 'w', force_zip64=before.st_size >= (1 << 31)) as target:
                while block := stream.read(8 << 20):
                    target.write(block); digest.update(block); done += len(block)
                    if compressor: compressed_bytes += len(compressor.compress(block))
                    if done // (1 << 30) != (done - len(block)) // (1 << 30): print(member, done, '/', before.st_size, flush=True)
            if compressor:
                compressed_bytes += len(compressor.flush()); trial_bytes += compressed_bytes
                if member in originals: base_trial += compressed_bytes
            after = source.stat()
            assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns), member
            assert (done, digest.hexdigest()) == (expected[member]['bytes'], expected[member]['sha256']), member
            print('Packed and SHA256 verified:', member, flush=True)
    if not measured: write_json(cache, dict(source_manifest_sha256=source_pin, deflate_bytes=base_trial))
    chosen, method = stored, 'Store'
    compressed = OUT / 'bundle.deflated.zip'
    if trial_bytes * 100 <= raw_bytes * 85:
        with zipfile.ZipFile(compressed, 'w', zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True) as archive:
            for member, source in sources.items(): archive.write(source, member)
        if compressed.stat().st_size * 100 <= raw_bytes * 85: chosen, method = compressed, 'Deflate-9'
    with zipfile.ZipFile(chosen) as archive:
        assert len(archive.infolist()) == len(sources) and set(archive.namelist()) == set(sources)
        for info in archive.infolist():
            assert '/' not in info.filename and info.file_size == expected[info.filename]['bytes']
            with archive.open(info) as stream: assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected[info.filename]['sha256'], info.filename
    digest = sha(chosen); chosen.rename(final)
    for unused in (stored, compressed):
        if unused.exists(): unused.unlink()
    (PUB / (final.name + '.sha256')).write_text(digest + '  ' + final.name + '\n', encoding='ascii')
    report = dict(file=final.name, bytes=final.stat().st_size, sha256=digest, files=len(sources), raw_bytes=raw_bytes,
                  method=method, minimum_savings_percent=15, measured_deflate_payload_bytes=trial_bytes,
                  maximum_possible_deflate_savings_percent=100 * (raw_bytes - trial_bytes) / raw_bytes,
                  crc_and_sha256_verified=True, original_steam_files_byte_exact=True,
                  data_groups={'steam': len(originals), 'port-ui': sum(n.startswith('port-ui.') for n in sources),
                               'switch-ui': sum(n.startswith('switch-ui.') for n in sources)},
                  seconds=round(time.monotonic() - started, 2))
    write_json(PUB / 'Frostpunk-packaging.json', report)
    write_json(PUB / 'Frostpunk-data.manifest.json', expected)
    print(json.dumps(report, indent=2), flush=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    PUB.mkdir(exist_ok=True)
    pc_templates = Archive(ROOT / 'build/switch-ui-20261112/assets-v6/templates')
    switch_index = (ROOT / 'build/switch-ui-20261112/assets-v6/templates-switch.idx').read_bytes()
    switch = {}
    with pc_templates.stem.with_suffix('.dat').open('rb') as data:
        for key, stored, decoded, offset, flag in RECORD.iter_unpack(switch_index[11:]):
            if (stored, decoded, offset, flag) != pc_templates.rows[key]:
                assert not flag and stored == decoded
                data.seek(offset)
                switch[key] = data.read(stored)
    assert len(switch) == 63
    patches = [port_ui_patch(OUT / 'bundle-resources'), resource_patch('switch-ui', {'templates': switch})]
    originals = {name: {key: row[key] for key in ('bytes', 'sha256')} for name, row in sorted(HOST['original_files'].items())}
    assert len(originals) == 38 and all((STEAM / name).parent == STEAM for name in originals)
    write_json(ROOT / 'android/game-data-originals.json', originals)
    sources = {name: STEAM / name for name in originals}
    expected = dict(originals)
    for folder, manifest in patches:
        for path in sorted(folder.iterdir()):
            if path.is_file():
                assert path.name not in sources and path.name.startswith(manifest['kind'] + '.')
                sources[path.name] = path; expected[path.name] = identify(path)
    package(sources, expected, originals)



if __name__ == '__main__':
    main()
