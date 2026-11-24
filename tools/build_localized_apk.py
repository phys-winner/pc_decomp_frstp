"""Build a private APK using locally obtained Czech and/or Hungarian localization files."""
import argparse
import json
from pathlib import Path
import re
import subprocess
from tempfile import mkdtemp
from zipfile import ZipFile, ZIP_DEFLATED

import android_launcher
import android_native_payload
import fastbuild
import fastlift
import build_vulkan_dev


def language_units(work):
    root = fastbuild.ROOT
    sources = root / 'build/fastlift'
    declarations = ('unsigned fp_fan_language_count(void);\n'
                    'uint64_t fp_fan_language_registry(CPU *, uint64_t);\n'
                    'int fp_fan_language_enabled(uint64_t);\n')
    # Retarget the built-in GUID/name searches, table loader and startup default selection.
    anchors = {
        807: ('140E5A998', '140E5A9B9', '140E5A9EE', '140E5AA04', '140E5F340'),
        809: ('140E834AA', '140E835B4', '140E839A1', '140E839CA', '140E83A58',
              '140E86A74', '140E86A87', '140E879B7', '140E879BF'),
    }

    def patch(match):
        line = match[0]
        if 'GVA(' in line:
            return re.sub(r'GVA\(0x[0-9A-F]+\)', r'fp_fan_language_registry(c, \g<0>)', line)
        if '0x10u' in line:
            bound = 'fp_fan_language_count() - 1' if '140E879B7' in line else 'fp_fan_language_count()'
            return line.replace('0x10u', bound, 1)
        if '000140E5F340: test rdx, rdx' in line:
            return ('    if (fp_fan_language_enabled(c->rdx)) {\n'
                    '        SET8L(c->rax, 1);\n        c->rsp += 8;\n        return;\n    }\n' + line)
        raise ValueError('Unexpected language hook: ' + line)

    units = {}
    for unit, addresses in anchors.items():
        name = f'recomp_funcs_{unit:04}'
        source = work / (name + '.c')
        text = (sources / 'src' / source.name).read_text(encoding='utf-8')
        for address in addresses:
            text, found = re.subn(rf'^    .*?/\* 000{address}: .*$', patch, text, flags=re.MULTILINE)
            expected = 2 if address in ('140E879B7', '140E879BF') else 1
            if found != expected:
                raise ValueError(f'Language hook {address}: expected {expected} source lines, found {found}')
        header = '#include "fastlift.h"\n'
        assert text.count(header) == 1
        source.write_text(text.replace(header, header + declarations, 1), encoding='utf-8', newline='\n')
        obj = source.with_suffix('.o')
        _, status, errors, _ = fastlift.compile_unit((source, obj, sources / 'include'))
        if status:
            raise RuntimeError(errors)
        units[unit] = str(obj)

    return units


def localized_runtime(work):
    units = language_units(work)
    root = fastbuild.ROOT
    host, _ = fastbuild.compile_one('android/host/fan_languages.c', fastbuild.COMPILE_FLAGS, fastbuild.header_digest())
    bridge = root / 'build/release-0.3-dev/host/libwin64_d3d11-fastlift.a'
    if not bridge.is_file():
        bridge = build_vulkan_dev.build_bridge(work)
    return build_vulkan_dev.link_runtime(work, bridge, extra_objects=(host,), unit_overrides=units)


def package_apk(base_apk, work, runtime):
    assets = work / 'assets'
    native, _ = android_native_payload.prepare({runtime.name: runtime}, fastbuild.BIN, work / 'payload')
    changed = {
        'classes.dex': (work / 'dex/classes.dex', ZIP_DEFLATED),
        'lib/arm64-v8a/libfrostpunk_app.so': (work / 'lib/libfrostpunk_app.so', ZIP_DEFLATED),
        'lib/arm64-v8a/libportprobe.so': (work / 'lib/libportprobe.so', ZIP_DEFLATED),
        **native,
    }
    for folder in ('launcher', 'port-ui'):
        for path in (assets / folder).rglob('*'):
            if path.is_file():
                changed['assets/' + path.relative_to(assets).as_posix()] = (path, ZIP_DEFLATED)
    for name in ('game-data-port-ui.json', 'game-inputs.tsv', 'game-data-contract.json'):
        changed['assets/' + name] = (assets / name, ZIP_DEFLATED)

    unsigned, aligned, apk = (work / name for name in ('unsigned.apk', 'aligned.apk', 'Frostpunk-localized.apk'))
    with ZipFile(base_apk) as base, ZipFile(unsigned, 'w', compression=ZIP_DEFLATED) as target:
        for info in base.infolist():
            if info.filename.startswith('META-INF/') or info.filename.startswith(('assets/port-ui/cs/', 'assets/port-ui/hu/')):
                continue
            replacement = changed.pop(info.filename, None)
            if replacement:
                path, method = replacement
                target.write(path, info.filename, compress_type=method)
            elif info.filename == 'assets/licenses/runtime-provenance.json':
                provenance = json.loads(base.read(info))
                provenance['binary_sha256'] = fastbuild.sha(runtime)
                provenance['binary_bytes'] = runtime.stat().st_size
                target.writestr(info, json.dumps(provenance, indent=2) + '\n')
            else:
                target.writestr(info, base.read(info))
        for name, (path, method) in sorted(changed.items()):
            target.write(path, name, compress_type=method)

    zipalign = str(fastbuild.BUILD_TOOLS / 'zipalign.exe')
    subprocess.run([zipalign, '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    signer = fastbuild.MACHINE['signer']
    sign = [str(fastbuild.JAVA), '-jar', str(fastbuild.SIGNER / signer['apksigner'])]
    subprocess.run(sign + ['sign', '--key', str(fastbuild.SIGNER / signer['key']),
                           '--cert', str(fastbuild.SIGNER / signer['cert']),
                           '--out', str(apk), str(aligned)], check=True)
    subprocess.run(sign + ['verify', str(apk)], check=True)
    subprocess.run([zipalign, '-c', '-P', '16', '4', str(apk)], check=True)
    with ZipFile(apk) as packaged:
        if packaged.testzip() is not None:
            raise ValueError('Corrupt localized APK')
    unsigned.unlink()
    aligned.unlink()
    return apk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('translations', type=Path, help='Directory containing cs/ and/or hu/ localizations.dat and .idx')
    parser.add_argument('--base-apk', type=Path, default=fastbuild.ROOT / 'pub/Frostpunk.apk')
    args = parser.parse_args()
    build = (fastbuild.ROOT / 'build').resolve()
    work = Path(mkdtemp(prefix='localized-frostpunk-', dir=build)).resolve()
    assert work.parent == build
    fastbuild.FAST = work
    fastbuild.build_dex()
    fastbuild.build_app()
    android_launcher.fan_localizations(args.translations, work / 'assets', work)
    print(package_apk(args.base_apk, work, localized_runtime(work)))


if __name__ == '__main__':
    main()
