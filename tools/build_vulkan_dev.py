"""Development APK for the native Vulkan rendering-device work (android/vulkan).

    python -X utf8 -B tools/build_vulkan_dev.py [--name NAME] [--base APK] [--install SERIAL]

Steps: compile the D3D11 bridge archive (fastlift recipe), link libfrostpunk_runtime.so with the
release host sources and fastlift units (tools/android_release.py), optionally build
libfrostpunk_rdvk.so (android/vulkan), then replace only the runtime payload and native libraries
of an existing signed release APK (DEX, resources and launcher assets stay byte-identical, so
uncommitted launcher edits in the working tree never reach this APK; older APKs get the legacy
native Vulkan toggle) and sign it with
the project key.  Outputs go to build/vulkan-native/NAME/.
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import android_release as builder  # noqa: E402
import android_native_payload as payload  # noqa: E402

fb = builder.fb
WORK = ROOT / 'build/vulkan-native'
DEFAULT_BASE = ROOT / 'pub/Frostpunk-0.2-dev-c04019c1.apk'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_bridge(out):
    subprocess.run([sys.executable, '-X', 'utf8', '-B', str(ROOT / 'tools/fastlift.py'), 'bridge', '--out', str(out)], check=True)
    return out / 'host/libwin64_d3d11-fastlift.a'


def link_runtime(out, bridge_archive, extra_objects=(), rdvk=True, unit_overrides=None, host_sources=()):
    builder.OUT = out
    extra = ['android/host/zip_files.c', 'reference/launcher-zipmount/zipmount.c']
    sources = json.loads(builder.CANONICAL.read_text())['sources']
    builder.EXTRA_HOST = ['android/host/mobile_lod.c', 'android/host/thread_placement.c'] + [n for n in extra if n not in sources] + list(host_sources)
    builder.ARCHIVES['build/dxvk-bridge/libwin64_d3d11.a'] = str(bridge_archive.relative_to(ROOT))
    replaced = dict(unit_overrides or {})
    if rdvk:
        import build_rdvk
        manifest = build_rdvk.build_hooks()
        for unit, path in manifest['units'].items():
            if int(unit) in replaced:
                raise ValueError(f'Unit {unit} has two runtime replacements')
            replaced[int(unit)] = str(ROOT / path)
        builder.EXTRA_HOST += ['android/vulkan/rdvk_runtime.c', manifest['stubs'].replace('\\', '/')]
        for include in ('android/vulkan', 'build/vulkan-native/hooks'):
            if include not in fb.INCLUDES:
                fb.INCLUDES.append(include)
    # Lifted library code that is unreachable or native now (tools/dead_code.py): the static
    # OpenAL Soft of Frostpunk.exe and its Lua 5.1.1 core (android/host/lua_native.c).
    import dead_code
    dead = dead_code.objects()
    for source in dead['host']:
        if source not in builder.EXTRA_HOST:
            builder.EXTRA_HOST.append(source)
    for include in dead['includes']:
        if include not in fb.INCLUDES:
            fb.INCLUDES.append(include)
    for unit, path in dead['units'].items():
        if unit in replaced:
            raise ValueError('unit %d is both hooked and trimmed' % unit)
        replaced[unit] = str(path)
    original_unit_objects = builder.unit_objects

    def objects(rows):
        out_rows = []
        for path in original_unit_objects(rows):
            name = Path(path).name
            unit = int(name[13:17]) if name.startswith('recomp_funcs_') and name[13:17].isdigit() else None
            if Path(path) == ROOT / dead_code.TABLE_OBJECT:
                out_rows.append(str(dead['table']))
                continue
            if Path(path) == Path(dead['crc_routed']):   # unit 0982 with the CRC route: dead_code's copy
                out_rows.append(str(dead['units'][dead_code.CRC_UNIT]))
                continue
            out_rows.append(replaced.get(unit, path) if Path(path).parent == ROOT / 'build/fastlift/obj' else path)
        if str(dead['table']) not in out_rows:
            raise ValueError('the main dispatch table is not in the link')
        missing = set(replaced.values()) - set(out_rows)
        if missing:
            raise ValueError('hooked units not in the link: %s' % sorted(missing))
        return out_rows + [str(p) for p in extra_objects]
    builder.unit_objects = objects
    return builder.link('runtime')


LAUNCHER_SPEC = 'assets/launcher/port.json'


def launcher_spec(base):
    """Preserve selection by renderer on new bases and enable the legacy toggle on old ones."""
    with zipfile.ZipFile(base) as z:
        original = z.read(LAUNCHER_SPEC)
    spec = json.loads(original)
    keys = [o['key'] for o in spec['options']]
    if 'native_vulkan' in keys or any(m['id'] == 'native_vulkan' for m in spec['renderers']['modes']):
        return original
    source = json.loads((ROOT / 'android/port.json').read_text(encoding='utf-8'))
    mode = next(m for m in source['renderers']['modes'] if m['id'] == 'native_vulkan')
    option = dict(key=mode['id'], type='bool', default='0', only=['frostpunk'], title=mode['title'], hint=mode['description'])
    spec['options'].insert(keys.index('switch_graphics') if 'switch_graphics' in keys else len(keys), option)
    return (json.dumps(spec, indent=1, ensure_ascii=False) + '\n').encode('utf-8')


def package(base, out, runtime, libraries):
    """Replace runtime payload + given lib/arm64-v8a libraries in a signed base APK."""
    entries, receipt = payload.prepare({runtime.name: runtime, **{p.name: p for p in libraries}}, fb.BIN, out / 'payload')
    unsigned, aligned = out / 'unsigned.apk', out / 'aligned.apk'
    pending = dict(entries)
    replaced = {LAUNCHER_SPEC: launcher_spec(base)}
    with zipfile.ZipFile(base) as src, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as dst:
        for info in src.infolist():
            if info.filename.startswith('META-INF/'):
                continue
            if info.filename in pending:
                path, method = pending.pop(info.filename)
                dst.write(path, info.filename, compress_type=method)
            elif info.filename in replaced:
                dst.writestr(info, replaced[info.filename], compress_type=info.compress_type)
            else:
                dst.writestr(info, src.read(info.filename), compress_type=info.compress_type)
        for name, (path, method) in sorted(pending.items()):
            dst.write(path, name, compress_type=method)
        if any(p.name == 'libfrostpunk_rdvk.so' for p in libraries):
            import build_shaderlib
            build_shaderlib.add_to_apk(dst)   # stored: the native Vulkan device maps it from the APK
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    apk = out / ('Frostpunk-vulkan-%s.apk' % out.name)
    signer = fb.MACHINE['signer']
    subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / signer['apksigner']), 'sign', '--key', str(fb.SIGNER / signer['key']),
                    '--cert', str(fb.SIGNER / signer['cert']), '--out', str(apk), str(aligned)], check=True)
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(apk)], check=True)
    unsigned.unlink()
    aligned.unlink()
    manifest = dict(apk=str(apk.relative_to(ROOT)), apk_sha256=sha(apk), base=str(base), base_sha256=sha(base),
                    runtime_sha256=sha(runtime), libraries={p.name: sha(p) for p in libraries},
                    runtime_xz=receipt['runtime_xz']['xz_sha256'])
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps(manifest, indent=1), flush=True)
    return apk


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', default='dev')
    ap.add_argument('--base', default=str(DEFAULT_BASE))
    ap.add_argument('--lib', action='append', default=[], help='extra native library for lib/arm64-v8a')
    ap.add_argument('--object', action='append', default=[], help='extra object linked into the runtime')
    ap.add_argument('--install', help='adb serial to install the APK on')
    ap.add_argument('--no-rdvk', action='store_true', help='link without the Vulkan rendering-device hooks')
    args = ap.parse_args()
    out = WORK / args.name
    out.mkdir(parents=True, exist_ok=True)
    archive = build_bridge(out)
    runtime = link_runtime(out, archive, [Path(p) for p in args.object], rdvk=not args.no_rdvk)
    libraries = [Path(p) for p in args.lib]
    if not args.no_rdvk:
        import build_rdvk
        libraries.append(build_rdvk.build_lib())
    apk = package(Path(args.base), out, runtime, libraries)
    if args.install:
        subprocess.run(['adb', '-s', args.install, 'install', '-r', str(apk)], check=True)


if __name__ == '__main__':
    main()
