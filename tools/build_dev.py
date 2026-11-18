"""Development APK of the working tree: launcher/DEX, libfrostpunk_app.so, D3D11 bridge, runtime
(release host sources + fastlift units + native Vulkan hooks) and libfrostpunk_rdvk.so, packaged
over the pinned v25 base APK like tools/build_release.py, signed with the project key.

    python -X utf8 -B tools/build_dev.py --name NAME [--install SERIAL] [--no-rdvk]

No commit/version assertions (unlike the release recipe); outputs go to build/dev-ui/NAME/.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import android_release as builder  # noqa: E402
import build_vulkan_dev as vulkan_dev  # noqa: E402
import build_shaderlib  # noqa: E402

fb = builder.fb
WORK = ROOT / 'build/dev-ui'
BASE = ROOT / 'build/ui-20261114/v25/Frostpunk-ui-v25.apk'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--name', required=True)
    ap.add_argument('--install', help='adb serial to install the APK on')
    ap.add_argument('--no-rdvk', action='store_true', help='link without the native Vulkan backend')
    ap.add_argument('--dxvk', type=Path, help='use rebuilt DXVK libraries from this folder')
    ap.add_argument('--skip-dex', action='store_true', help='reuse the DEX/launcher assets of the previous build')
    args = ap.parse_args()
    out = WORK / args.name
    out.mkdir(parents=True, exist_ok=True)
    shared = WORK / 'shared'          # DEX/app/launcher outputs, reused with --skip-dex
    builder.OUT = shared
    builder.BASE_APK = BASE
    if not args.skip_dex or not (shared / 'dex/classes.dex').exists():
        builder.build_dex()
    builder.build_app()
    archive = vulkan_dev.build_bridge(shared)
    runtime = vulkan_dev.link_runtime(shared, archive, rdvk=not args.no_rdvk)
    libraries = []
    if not args.no_rdvk:
        import build_rdvk
        libraries.append(build_rdvk.build_lib())
    apk = builder.package(runtime, args.name, dxvk=args.dxvk, native_libraries=libraries)
    # The release recipe's extra assets: data contract, manifests and licence notes of this tree.
    extra = {p.name: p for p in (shared / 'launcher-assets').glob('game-data-*.json')}
    extra['game-inputs.tsv'] = shared / 'launcher-assets/game-inputs.tsv'
    extra['licenses/Frostpunk-port-GPL-3.0.txt'] = ROOT / 'LICENSE'
    extra['licenses/Frostpunk-port-NOTICE.txt'] = ROOT / 'NOTICE'
    extra['licenses/meshoptimizer-MIT.txt'] = ROOT / 'reference/meshoptimizer/LICENSE.md'
    extra['licenses/Turnip-Mesa-MIT.txt'] = ROOT / 'android/licenses/Mesa-MIT.txt'
    extra['licenses/Turnip-LICENSE-NOTE.txt'] = ROOT / 'android/licenses/Turnip-LICENSE-NOTE.txt'
    unsigned, aligned = out / 'dev-unsigned.apk', out / 'dev-aligned.apk'
    with zipfile.ZipFile(apk) as source, zipfile.ZipFile(unsigned, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as target:
        for info in source.infolist():
            if info.filename.startswith(('META-INF/', 'assets/switch-ui/')) or info.filename.removeprefix('assets/') in extra \
                    or info.filename == 'assets/' + build_shaderlib.ASSET:
                continue
            target.writestr(info, source.read(info.filename))
        for name, path in extra.items():
            target.write(path, 'assets/' + name)
        if not args.no_rdvk:
            build_shaderlib.add_to_apk(target)   # stored: the native Vulkan device maps it from the APK
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    final = out / ('Frostpunk-dev-%s.apk' % args.name)
    signer = fb.MACHINE['signer']
    subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / signer['apksigner']), 'sign', '--key', str(fb.SIGNER / signer['key']),
                    '--cert', str(fb.SIGNER / signer['cert']), '--out', str(final), str(aligned)], check=True)
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(final)], check=True)
    unsigned.unlink()
    aligned.unlink()
    report = dict(apk=str(final.relative_to(ROOT)), apk_sha256=sha(final), bytes=final.stat().st_size,
                  runtime_sha256=sha(runtime), libraries={p.name: sha(p) for p in libraries})
    (out / 'dev-manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report, indent=1), flush=True)
    if args.install:
        subprocess.run(['adb', '-s', args.install, 'install', '-r', str(final)], check=True)


if __name__ == '__main__':
    main()
