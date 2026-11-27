"""Build a Frostpunk release from the validated private AOT/base-APK inputs."""
import os
import shutil
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
from android_launcher import VERSION, fan_localizations
OUT = ROOT / ('build/release-' + VERSION[0])
import android_release as builder
import atomic_sites
import build_dxvk
import build_localized_apk
import build_rdvk
import build_shaderlib
import build_vulkan_dev as vulkan_dev
builder.OUT = OUT
builder.BASE_APK = ROOT / 'build/ui-20261114/v25/Frostpunk-ui-v25.apk'
assert builder.sha(builder.BASE_APK) == 'a7a62b8f72aa6b2fbd685a0c6c8a8807a9f191a5e56a9da11374b6d0e884b9e6'
OUT.mkdir(parents=True, exist_ok=True)
extra_host = ['android/host/zip_files.c', 'reference/launcher-zipmount/zipmount.c']
release_host = ['android/host/' + name + '.c' for name in
                ('fan_languages', 'guid_search_native', 'entity_walk_native', 'visibility_native')]
source_names = sorted(set(json.loads(builder.CANONICAL.read_text())['sources'] + builder.EXTRA_HOST + extra_host + release_host))
source_pins = {name: builder.sha(ROOT / builder.SOURCE_OVERRIDE.get(name, name)) for name in source_names}
source_pins.update({str(p.relative_to(ROOT)): builder.sha(p) for p in sorted((ROOT / 'android/host').glob('*.h'))})
source_pins['android/host/switch_texture_caps.inc'] = builder.sha(ROOT / 'android/host/switch_texture_caps.inc')
sources = [ROOT / 'android/app/app.c', ROOT / 'android/dxvk/bridge.cpp', ROOT / 'android/dxvk/mobile_geometry.inc', ROOT / 'android/host/port_settings.c', ROOT / 'android/host/switch_performance.c',
           ROOT / 'android/steam/offline_policy.cpp', ROOT / 'android/steam/probe.cpp',
           ROOT / 'android/texconv/texconv.cpp', ROOT / 'tools/android_launcher.py', ROOT / 'android/port.json',
           ROOT / 'android/launcher-compatibility-localization.json', *sorted((ROOT / 'android/launcher').glob('*.java')),
           ROOT / 'android/launcher/launch_checks.java.inc']
pins = {str(p.relative_to(ROOT)): builder.sha(p) for p in sources}
pins.update({str(p.relative_to(ROOT)): builder.sha(p) for p in (ROOT / 'android/vulkan').rglob('*') if p.is_file()})
# The normal APK carries the native Vulkan renderer only (owner's decision of 24 November 2026): DXVK is
# neither built nor packaged, and the FSR 3 module, which only the DXVK path loaded, leaves with it.
DROPPED = {'lib/arm64-v8a/libdxvk_d3d11.so', 'lib/arm64-v8a/libdxvk_dxgi.so', 'lib/arm64-v8a/libfrostpunk_fsr3.so',
           'assets/licenses/FSR3.txt', 'assets/licenses/FSR3-SDK.txt', 'assets/licenses/FSR3-origin.json'}
PUB = Path(os.environ['FROSTPUNK_PUB']).resolve() if os.environ.get('FROSTPUNK_PUB') else ROOT / 'pub'
builder.build_dex()
fan_localizations(ROOT / 'build/fan-localizations', OUT / 'launcher-assets', OUT)
builder.build_app()
archive = vulkan_dev.build_bridge(OUT)

runtime_units = build_localized_apk.language_units(OUT)
visibility_build = OUT / 'visibility'
subprocess.run([sys.executable, '-X', 'utf8', '-B', str(ROOT / 'tools/fastlift.py'), 'build',
                '--units', '795,822', '--out', str(visibility_build)], check=True)
runtime_units[795] = str(visibility_build / 'obj/recomp_funcs_0795.o')
native_hooks = {
    810: ('L_000140E92AF0', 'L_000140E92B80'),
    812: ('L_000140EB45F0',),
    822: ('L_000140F8F530',),
}
for unit, names in native_hooks.items():
    source = (visibility_build if unit == 822 else builder.fb.FASTLIFT) / 'obj' / f'recomp_funcs_{unit:04}.o'
    symbols = build_rdvk.symbols_of(source)
    args = []
    for name in names:
        offset, section, binding, visibility = symbols[name]
        assert binding == 'GLOBAL', (source, name)
        alias = 'fp_cpu_original_' + name
        scope = 'hidden' if visibility == 'HIDDEN' else 'default'
        args += ['--weaken-symbol=' + name, f'--add-symbol={alias}={section}:0x{offset:x},global,function,{scope}']
    target = OUT / f'recomp_funcs_{unit:04}-native.o'
    subprocess.run([str(builder.fb.BIN / 'llvm-objcopy.exe'), *args, str(source), str(target)], check=True)
    checked = build_rdvk.symbols_of(target)
    for name in names:
        assert checked[name][2] == 'WEAK' and checked['fp_cpu_original_' + name][:2] == checked[name][:2]
    runtime_units[unit] = str(target)

atomic_manifest = atomic_sites.overlay(OUT / 'atomic')
atomic_objects = {}
for row in atomic_manifest['units']:
    original, source = row['source'], row['overlay']
    path = Path(original)
    if 'recomp' not in path.parts:
        if original not in source_names:
            continue
        builder.SOURCE_OVERRIDE[original] = source
    else:
        key = path.parent.parent / 'objects' / path.with_suffix('.o').name
        atomic_objects[key.as_posix()] = str(builder.compile_sources([source])[0])
    source_pins[original] = row['source_sha256']
    source_pins[source] = row['overlay_sha256']
base_unit_objects = builder.unit_objects


def release_unit_objects(rows):
    assert atomic_objects.keys() <= {row.replace('\\', '/') for row in rows}
    return [atomic_objects.get(Path(path).relative_to(ROOT).as_posix(), path) for path in base_unit_objects(rows)]


builder.unit_objects = release_unit_objects
runtime = vulkan_dev.link_runtime(OUT, archive, unit_overrides=runtime_units, host_sources=release_host)
rdvk = build_rdvk.build_lib()
source_manifest_sha256 = hashlib.sha256(json.dumps(source_pins, sort_keys=True).encode()).hexdigest()
apk = builder.package(runtime, 'data-flow', native_libraries=(rdvk,))
unsigned, aligned = OUT / 'data-unsigned.apk', OUT / 'data-aligned.apk'
extra = {p.name: p for p in (OUT / 'launcher-assets').glob('game-data-*.json')}
extra['game-inputs.tsv'] = OUT / 'launcher-assets/game-inputs.tsv'
with zipfile.ZipFile(apk) as z:
    provenance = json.loads(z.read('assets/licenses/runtime-provenance.json'))
    build_metadata = json.loads(z.read('assets/launcher/build.json'))
    assert build_metadata['commit'] == subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', '--short=8', 'HEAD'], text=True).strip()
    assert len(build_metadata['commit']) == 8, 'APK commit suffix must contain exactly eight characters'
    assert (build_metadata['versionName'], build_metadata['versionCode']) == VERSION
    public_libraries = {}
    redactions = {}
    home = Path.home().as_posix().encode()
    assert len(home) >= len(b'/build')
    neutral = b'/build'.ljust(len(home), b'_')
    # shortcut: redact cached NDK assertion paths in place; use -ffile-prefix-map when rebuilding these libraries.
    for name in ('libopenal.so',):
        entry = 'lib/arm64-v8a/' + name
        original = z.read(entry)
        cleaned = original.replace(b'\0' + home + b'/', b'\0' + neutral + b'/')
        assert len(original) == len(cleaned) and home not in cleaned, entry
        public_libraries[entry] = cleaned
        redactions[name] = dict(input_sha256=hashlib.sha256(original).hexdigest(),
                               sha256=hashlib.sha256(cleaned).hexdigest(),
                               diagnostic_paths=original.count(b'\0' + home + b'/'))
provenance['public_binary_redactions'] = redactions
provenance['source_manifest_sha256'] = source_manifest_sha256
provenance['data_flow'] = 'Stored ZIP; original Steam files; optional prefixed archive-entry patches; on-device ASTC sidecars'
packaging = json.loads((OUT / 'data-flow/manifest.json').read_text())['packaging']['libraries']
services = packaging['libfrostpunk_offline_services.so']
provenance['runtime_dependencies']['libfrostpunk_offline_services.so']['sha256'] = services['source_sha256']
provenance['runtime_dependencies'][rdvk.name] = {'sha256': packaging[rdvk.name]['sha256']}
proof = OUT / 'runtime-provenance.json'
proof.write_text(json.dumps(provenance, indent=2) + '\n')
extra['licenses/runtime-provenance.json'] = proof
extra['licenses/Frostpunk-port-GPL-3.0.txt'] = ROOT / 'LICENSE'
extra['licenses/Frostpunk-port-NOTICE.txt'] = ROOT / 'NOTICE'
extra['licenses/meshoptimizer-MIT.txt'] = ROOT / 'reference/meshoptimizer/LICENSE.md'
extra['licenses/etcpak-BSD-3-Clause.txt'] = ROOT / 'reference/etcpak/LICENSE.txt'
extra['licenses/Turnip-Mesa-MIT.txt'] = ROOT / 'android/licenses/Mesa-MIT.txt'
extra['licenses/Turnip-LICENSE-NOTE.txt'] = ROOT / 'android/licenses/Turnip-LICENSE-NOTE.txt'
general_origin = json.loads((ROOT / 'reference/turnip-android/provenance.json').read_text())
public_general = OUT / 'Turnip-origin.json'
public_general.write_text(json.dumps({key: general_origin[key] for key in
    ('publisher_repository', 'release_url', 'release_tag', 'asset_sha256', 'driver_sha256', 'package_metadata')}, indent=2) + '\n')
extra['licenses/Turnip-origin.json'] = public_general
with zipfile.ZipFile(apk) as source, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as target:
    for info in source.infolist():
        if info.filename.startswith(('META-INF/', 'assets/switch-ui/')) or info.filename.removeprefix('assets/') in extra \
                or info.filename == 'assets/' + build_shaderlib.ASSET or info.filename in DROPPED:
            continue
        target.writestr(info, public_libraries[info.filename] if info.filename in public_libraries else source.read(info.filename))
    for name, path in extra.items():
        target.write(path, 'assets/' + name)
    build_shaderlib.add_to_apk(target)   # stored: the native Vulkan device maps it from the APK
fb = builder.fb
subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
final = PUB / ('Frostpunk-' + VERSION[0] + '-' + build_metadata['commit'] + '.apk')
final.parent.mkdir(exist_ok=True)
atomic_originals = {row['source'] for row in atomic_manifest['units']}
for name, pin in {**pins, **source_pins}.items():
    actual = name if name in atomic_originals else builder.SOURCE_OVERRIDE.get(name, name)
    assert builder.sha(ROOT / actual) == pin, 'Source changed during build: ' + name
subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'sign',
                '--key', str(fb.SIGNER / fb.MACHINE['signer']['key']), '--cert', str(fb.SIGNER / fb.MACHINE['signer']['cert']),
                '--out', str(final), str(aligned)], check=True)
subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'verify', str(final)], check=True)
subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(final)], check=True)
with zipfile.ZipFile(final) as packaged:
    metadata = json.loads(packaged.read('assets/launcher/build.json'))
    assert metadata == build_metadata
    assert (metadata['versionName'], metadata['versionCode']) == VERSION
    assert all(info.compress_type == zipfile.ZIP_DEFLATED for info in packaged.infolist()
               if info.filename.startswith('lib/') and info.filename.endswith('.so')), 'Uncompressed native library'
    assert hashlib.sha256(packaged.read('lib/arm64-v8a/' + rdvk.name)).hexdigest() == packaging[rdvk.name]['sha256']
    assert packaged.read('assets/licenses/Frostpunk-port-GPL-3.0.txt') == (ROOT / 'LICENSE').read_bytes()
    assert packaged.read('assets/licenses/Frostpunk-port-NOTICE.txt') == (ROOT / 'NOTICE').read_bytes()
    ui_manifest = json.loads(packaged.read('assets/game-data-port-ui.json'))
    bundled_names = {'assets/port-ui/' + name for name in ui_manifest['payloads']}
    for name, pin in ui_manifest['payloads'].items():
        data = packaged.read('assets/port-ui/' + name)
        assert len(data) == pin['bytes'] and hashlib.sha256(data).hexdigest() == pin['sha256'], name
    assert packaged.read('assets/licenses/Turnip-Mesa-MIT.txt') == (ROOT / 'android/licenses/Mesa-MIT.txt').read_bytes()
    assert not any(Path(name).suffix.lower() in ('.exe', '.dll', '.dat', '.idx', '.append', '.pk8', '.pem', '.jks', '.keystore')
                   or name.startswith('assets/switch-ui/') for name in packaged.namelist() if name not in bundled_names), 'Private payload in APK'
    driver_entries = [info.filename for info in packaged.infolist()
                      if info.filename.endswith('.so') and hashlib.sha256(packaged.read(info)).hexdigest() == fb.TURNIP_SHA256]
    assert driver_entries == ['lib/arm64-v8a/libpurple_turnip.so'], driver_entries
    assert not any(name.startswith('assets/turnip/') or name == 'lib/arm64-v8a/libfrostpunk_turnip.so'
                   for name in packaged.namelist()), 'Duplicate bundled Turnip'
    assert packaged.read('assets/licenses/meshoptimizer-MIT.txt') == (ROOT / 'reference/meshoptimizer/LICENSE.md').read_bytes()
    shaderlib_info = packaged.getinfo('assets/' + build_shaderlib.ASSET)
    assert shaderlib_info.compress_type == zipfile.ZIP_STORED, 'shader library must be stored'
    assert hashlib.sha256(packaged.read(shaderlib_info)).hexdigest() == build_shaderlib.sha(build_shaderlib.OUTPUT)
    assert not any(name.startswith('lib/') and ('dxvk' in name.lower() or 'fsr3' in name.lower()) for name in packaged.namelist()), 'DXVK payload in APK'
    assert packaged.testzip() is None, 'APK CRC mismatch'
unsigned.unlink(); aligned.unlink()
(final.parent / (final.name + '.sha256')).write_text(builder.sha(final) + '  ' + final.name + '\n', encoding='ascii')
report = dict(apk=str(final.relative_to(ROOT)), apk_sha256=builder.sha(final), bytes=final.stat().st_size, runtime_sha256=builder.sha(runtime),
              sources=pins,
              assets={name: builder.sha(path) for name, path in extra.items()}, runtime_sources=source_pins, runtime_source_manifest_sha256=source_manifest_sha256)
report['public_binary_redactions'] = redactions
(OUT / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ('apk', 'apk_sha256', 'bytes', 'runtime_sha256')}, indent=2), flush=True)

for alias in ('Frostpunk-' + VERSION[0] + '.apk', 'Frostpunk.apk') if PUB == ROOT / 'pub' else ():
    shutil.copyfile(final, final.parent / alias)
    (final.parent / (alias + '.sha256')).write_text(builder.sha(final) + '  ' + alias + '\n', encoding='ascii')
