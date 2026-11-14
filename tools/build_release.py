"""Build Frostpunk 0.1-dev from the validated private AOT/base-APK inputs."""
import shutil
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/release-0.1-dev'
import android_release as builder
builder.OUT = OUT
builder.EXTRA_HOST += [name for name in ('android/host/zip_files.c', 'reference/launcher-zipmount/zipmount.c') if name not in json.loads(builder.CANONICAL.read_text())['sources']]
builder.BASE_APK = ROOT / 'build/ui-20261114/v25/Frostpunk-ui-v25.apk'
assert builder.sha(builder.BASE_APK) == 'a7a62b8f72aa6b2fbd685a0c6c8a8807a9f191a5e56a9da11374b6d0e884b9e6'
OUT.mkdir(parents=True, exist_ok=True)
source_names = sorted(set(json.loads(builder.CANONICAL.read_text())['sources'] + builder.EXTRA_HOST))
source_pins = {name: builder.sha(ROOT / builder.SOURCE_OVERRIDE.get(name, name)) for name in source_names}
source_pins.update({str(p.relative_to(ROOT)): builder.sha(p) for p in sorted((ROOT / 'android/host').glob('*.h'))})
source_pins['android/host/switch_texture_caps.inc'] = builder.sha(ROOT / 'android/host/switch_texture_caps.inc')
sources = [ROOT / 'android/app/app.c', ROOT / 'android/host/port_settings.c', ROOT / 'android/host/switch_performance.c',
           ROOT / 'android/steam/offline_policy.cpp', ROOT / 'android/steam/probe.cpp',
           ROOT / 'android/texconv/texconv.cpp', ROOT / 'tools/android_launcher.py', ROOT / 'android/port.json',
           ROOT / 'android/launcher-compatibility-localization.json', *sorted((ROOT / 'android/launcher').glob('*.java')),
           ROOT / 'android/launcher/launch_checks.java.inc']
pins = {str(p.relative_to(ROOT)): builder.sha(p) for p in sources}
builder.build_dex()
builder.build_app()
runtime = builder.link('data-flow')
source_manifest_sha256 = __import__('hashlib').sha256(json.dumps(source_pins, sort_keys=True).encode()).hexdigest()
apk = builder.package(runtime, 'data-flow')
unsigned, aligned = OUT / 'data-unsigned.apk', OUT / 'data-aligned.apk'
extra = {p.name: p for p in (OUT / 'launcher-assets').glob('game-data-*.json')}
extra['game-inputs.tsv'] = OUT / 'launcher-assets/game-inputs.tsv'
with zipfile.ZipFile(apk) as z:
    provenance = json.loads(z.read('assets/licenses/runtime-provenance.json'))
    public_libraries = {}
    redactions = {}
    home = Path.home().as_posix().encode()
    assert len(home) >= len(b'/build')
    neutral = b'/build'.ljust(len(home), b'_')
    # shortcut: redact cached NDK assertion paths in place; use -ffile-prefix-map when rebuilding these libraries.
    for name in ('libdxvk_d3d11.so', 'libdxvk_dxgi.so', 'libopenal.so'):
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
proof = OUT / 'runtime-provenance.json'
proof.write_text(json.dumps(provenance, indent=2) + '\n')
extra['licenses/runtime-provenance.json'] = proof
extra['licenses/Frostpunk-port-GPL-3.0.txt'] = ROOT / 'LICENSE'
extra['licenses/Frostpunk-port-NOTICE.txt'] = ROOT / 'NOTICE'
driver_origin = json.loads((ROOT / 'reference/turnip-sm8250/provenance.json').read_text())
driver_origin.pop('qa_device', None)
public_driver = OUT / 'Turnip-SM8250-origin.json'
public_driver.write_text(json.dumps(driver_origin, indent=2) + '\n')
extra['licenses/Turnip-SM8250-origin.json'] = public_driver
with zipfile.ZipFile(apk) as source, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as target:
    for info in source.infolist():
        if info.filename.startswith(('META-INF/', 'assets/switch-ui/')) or info.filename.removeprefix('assets/') in extra:
            continue
        target.writestr(info, public_libraries[info.filename] if info.filename in public_libraries else source.read(info.filename))
    for name, path in extra.items():
        target.write(path, 'assets/' + name)
fb = builder.fb
subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
final = ROOT / 'pub/Frostpunk-0.1-dev.apk'
final.parent.mkdir(exist_ok=True)
for name, pin in {**pins, **source_pins}.items():
    assert builder.sha(ROOT / builder.SOURCE_OVERRIDE.get(name, name)) == pin, 'Source changed during build: ' + name
subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'sign',
                '--key', str(fb.SIGNER / fb.MACHINE['signer']['key']), '--cert', str(fb.SIGNER / fb.MACHINE['signer']['cert']),
                '--out', str(final), str(aligned)], check=True)
subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / fb.MACHINE['signer']['apksigner']), 'verify', str(final)], check=True)
subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(final)], check=True)
with zipfile.ZipFile(final) as packaged:
    metadata = json.loads(packaged.read('assets/launcher/build.json'))
    assert (metadata['versionName'], metadata['versionCode']) == ('0.1-dev', 2)
    assert packaged.read('assets/licenses/Frostpunk-port-GPL-3.0.txt') == (ROOT / 'LICENSE').read_bytes()
    assert packaged.read('assets/licenses/Frostpunk-port-NOTICE.txt') == (ROOT / 'NOTICE').read_bytes()
    assert not any(Path(name).suffix.lower() in ('.exe', '.dll', '.dat', '.idx', '.append', '.pk8', '.pem', '.jks', '.keystore')
                   or name.startswith('assets/switch-ui/') for name in packaged.namelist()), 'Private payload in APK'
    assert packaged.testzip() is None, 'APK CRC mismatch'
unsigned.unlink(); aligned.unlink()
(final.parent / (final.name + '.sha256')).write_text(builder.sha(final) + '  ' + final.name + '\n', encoding='ascii')
report = dict(apk_sha256=builder.sha(final), bytes=final.stat().st_size, runtime_sha256=builder.sha(runtime),
              sources=pins,
              assets={name: builder.sha(path) for name, path in extra.items()}, runtime_sources=source_pins, runtime_source_manifest_sha256=source_manifest_sha256)
report['public_binary_redactions'] = redactions
(OUT / 'receipt.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ('apk_sha256', 'bytes', 'runtime_sha256')}, indent=2), flush=True)

shutil.copyfile(final, final.parent / 'Frostpunk.apk')
(final.parent / 'Frostpunk.apk.sha256').write_text(builder.sha(final) + '  Frostpunk.apk\n', encoding='ascii')
