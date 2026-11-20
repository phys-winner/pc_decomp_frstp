"""Patch release: an existing release APK with some files replaced, aligned and signed with the project key.

    python -X utf8 -B tools/repack_release.py --base pub/Frostpunk-0.3-dev-6e31a5c1.apk --name NAME \
        --replace lib/arm64-v8a/libfrostpunk_rdvk.so=build/dev-ui/X/libfrostpunk_rdvk.so [--from-apk APK:ENTRY ...]

Every other entry keeps its bytes and its compression method (stored assets such as the shader library
stay stored, which the native Vulkan device needs to map them).  Output: build/patch/NAME/Frostpunk-NAME.apk.
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
import fastbuild as fb  # noqa: E402


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', type=Path, required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--replace', action='append', default=[], help='ENTRY=FILE')
    ap.add_argument('--from-apk', action='append', default=[], help='APK:ENTRY (take ENTRY from another APK)')
    ap.add_argument('--install', help='adb serial')
    args = ap.parse_args()
    data = {}
    for item in args.replace:
        entry, _, path = item.partition('=')
        data[entry] = Path(path).read_bytes()
    for item in args.from_apk:
        apk, _, entry = item.rpartition(':')
        with zipfile.ZipFile(apk) as z:
            data[entry] = z.read(entry)
    out = ROOT / 'build/patch' / args.name
    out.mkdir(parents=True, exist_ok=True)
    unsigned, aligned, final = out / 'unsigned.apk', out / 'aligned.apk', out / ('Frostpunk-%s.apk' % args.name)
    seen = set()
    with zipfile.ZipFile(args.base) as source, zipfile.ZipFile(unsigned, 'w') as target:
        for info in source.infolist():
            if info.filename.startswith('META-INF/'):
                continue
            body = data.get(info.filename)
            if body is None:
                body = source.read(info.filename)
            else:
                seen.add(info.filename)
            copy = zipfile.ZipInfo(info.filename, info.date_time)
            copy.compress_type = info.compress_type
            copy.external_attr = info.external_attr
            target.writestr(copy, body, compress_type=info.compress_type, compresslevel=9 if info.compress_type == zipfile.ZIP_DEFLATED else None)
    missing = set(data) - seen
    if missing:
        raise SystemExit('entries not in the base APK: %s' % sorted(missing))
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-P', '16', '-f', '4', str(unsigned), str(aligned)], check=True)
    signer = fb.MACHINE['signer']
    subprocess.run([str(fb.JAVA), '-jar', str(fb.SIGNER / signer['apksigner']), 'sign', '--key', str(fb.SIGNER / signer['key']),
                    '--cert', str(fb.SIGNER / signer['cert']), '--out', str(final), str(aligned)], check=True)
    subprocess.run([str(fb.BUILD_TOOLS / 'zipalign.exe'), '-c', '-P', '16', '4', str(final)], check=True)
    unsigned.unlink()
    aligned.unlink()
    report = dict(base=str(args.base), base_sha256=sha(args.base), apk=str(final.relative_to(ROOT)), apk_sha256=sha(final),
                  replaced={k: hashlib.sha256(v).hexdigest() for k, v in data.items()})
    (out / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report, indent=1), flush=True)
    if args.install:
        subprocess.run(['adb', '-s', args.install, 'install', '-r', str(final)], check=True)


if __name__ == '__main__':
    main()
