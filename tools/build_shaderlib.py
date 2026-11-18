"""Build-time shader library of the native Vulkan rendering device (android/vulkan).

    python -X utf8 -B tools/build_shaderlib.py build            # build/vulkan-native/shaderlib/rdvk_shaderlib.bin
    python -X utf8 -B tools/build_shaderlib.py validate         # spirv-val of every SPIR-V variant
    python -X utf8 -B tools/build_shaderlib.py verify SERIAL    # the same translator on the device: byte-identical?

The game ships its shaders as DXBC inside common.dat (Common/Shaders/Compiled, one container per
entry, 1,227 unique ones for the pinned Steam build).  `build` extracts them, tags the passes of the
mobile pass policy (tools/dxvk_mobile_overlay.py, by DXBC SHA-1, as policy.cpp did at run time),
and translates them on the PC with android/vulkan/shaderlib/rdvk_shaderc.cpp: DXVK 2.4.1's DXBC
compiler with the options the device used.  tools/build_rdvk.py links the result into
libfrostpunk_rdvk.so, which then holds no shader translator.  `verify` builds the same translator
for arm64 Android, runs it on the device over the same input and compares the output bytes.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastbuild as fb  # noqa: E402

WORK = ROOT / 'build/vulkan-native/shaderlib'
INPUT = WORK / 'dxbc-input.bin'
RAW = WORK / 'rdvk_shaderlib-raw.bin'    # rdvk_shaderc output: raw SPIR-V (compared with the device run)
OUTPUT = WORK / 'rdvk_shaderlib.bin'     # packed for the APK: one zlib stream per variant
MANIFEST = WORK / 'manifest.json'
ASSET = 'shaders/rdvk_shaderlib.bin'   # stored in the APK, read by android/app/app.c
SOURCE = ROOT / 'android/vulkan/shaderlib'
ROLE_VALUES = {'D3D11FpRoleTonemapOpaque': 2, 'D3D11FpRoleOutlineMask': 3, 'D3D11FpRoleOutlineDilate': 4,
               'D3D11FpRoleOutlineComposite': 5, 'D3D11FpRoleDepthMinMax': 6}   # objects.h ROLE_*
ROLE_TONEMAP = 1


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def game_directory():
    return Path(json.loads((ROOT / 'port.json').read_text())['game_directory'])


def program_type(blob):
    count, = struct.unpack_from('<I', blob, 28)
    for i in range(count):
        at, = struct.unpack_from('<I', blob, 32 + 4 * i)
        if blob[at:at + 4] in (b'SHEX', b'SHDR'):
            return struct.unpack_from('<I', blob, at + 8)[0] >> 16
    raise ValueError('DXBC without SHEX/SHDR')


def role_of(blob):
    """policy.cpp pass_role() of the device before the build-time library."""
    import dxvk_mobile_overlay as overlay
    kind = program_type(blob)
    if kind not in (0, 5):          # pixel and compute shaders only
        return 0
    name = ('FS_' if kind == 0 else 'CS_') + hashlib.sha1(blob).hexdigest()
    for role_name, role in overlay.ROLES:
        if name == role_name:
            return ROLE_VALUES[role]
    if kind == 0 and name[3:] in overlay.TONEMAPS:
        return ROLE_TONEMAP
    return 0


def extract():
    """Unique DXBC containers of common.dat, ordered by SHA-1 (deterministic input)."""
    from liquid_archive import Archive
    archive = Archive(game_directory() / 'common')
    blobs = {}
    containers = 0
    for key in sorted(archive.rows):
        data = archive.read(key)
        at = data.find(b'DXBC')
        if data[:4] != b'iV\xbe\xba' or at < 0:
            continue
        containers += 1
        while at >= 0:
            size, = struct.unpack_from('<I', data, at + 24)
            blob = data[at:at + size]
            if len(blob) != size:
                raise ValueError('truncated DXBC in entry %08x' % key)
            blobs[hashlib.sha1(blob).hexdigest()] = blob
            at = data.find(b'DXBC', at + size)
    return containers, [blobs[k] for k in sorted(blobs)]


def write_input(blobs):
    WORK.mkdir(parents=True, exist_ok=True)
    out = bytearray()
    roles = {}
    for blob in blobs:
        role = role_of(blob)
        if role:
            roles[hashlib.sha1(blob).hexdigest()] = role
        out += struct.pack('<B3xI', role, len(blob)) + blob + b'\0' * (-len(blob) % 4)
    INPUT.write_bytes(bytes(out))
    return roles


def compiler_sources():
    import build_rdvk
    return build_rdvk.compiler_tree() + [SOURCE / 'rdvk_shaderc.cpp']


def includes():
    import build_rdvk
    return [SOURCE, build_rdvk.TREE, build_rdvk.DXVK / 'include/vulkan/include', build_rdvk.DXVK / 'include/spirv/include']


def compile_all(sources, flags, out, compiler):
    out.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256()
    for d in includes():
        for p in sorted(Path(d).rglob('*.h')):
            digest.update(p.as_posix().encode() + sha(p).encode())
    headers = digest.hexdigest()

    def one(src):
        key = hashlib.sha256((sha(src) + json.dumps(flags) + headers).encode()).hexdigest()[:16]
        obj = out / ('%s-%s.o' % (src.stem, key))
        if not obj.exists():
            done = subprocess.run([*compiler, *flags, *['-I' + str(p) for p in includes()], '-c', str(src), '-o', str(obj.with_suffix('.tmp.o'))],
                                  capture_output=True, text=True, env=compiler_env)
            if done.returncode:
                raise RuntimeError('compile failed: %s\n%s' % (src, done.stderr[-6000:]))
            obj.with_suffix('.tmp.o').replace(obj)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        return list(pool.map(one, sources))


compiler_env = None
COMMON_FLAGS = ['-std=c++17', '-O2', '-w', '-DVK_NO_PROTOTYPES', '-DNDEBUG']


def zig():
    import shutil
    found = os.environ.get('ZIG') or shutil.which('zig')
    if not found:
        raise RuntimeError('zig (0.16) is required for the host translator: set ZIG or put zig on PATH')
    return found


def host_exe():
    """The translator for the PC, built with zig's clang for x86_64-windows-gnu.

    The Itanium C++ ABI matters: under the MSVC ABI clang evaluates call arguments right to left,
    and DXVK's compiler allocates SPIR-V ids inside such arguments, so an MSVC-ABI build numbers
    the ids differently from the arm64 device build (equivalent but not byte-identical SPIR-V)."""
    global compiler_env
    compiler_env = None
    flags = ['-target', 'x86_64-windows-gnu', *COMMON_FLAGS, '-DNOMINMAX']
    objects = compile_all(compiler_sources(), flags, WORK / 'host-obj', [zig(), 'c++'])
    exe = WORK / 'rdvk_shaderc.exe'
    subprocess.run([zig(), 'c++', '-target', 'x86_64-windows-gnu', *map(str, objects), '-o', str(exe)], check=True, capture_output=True)
    return exe


def arm_exe():
    global compiler_env
    compiler_env = None
    import build_rdvk   # DXVK's stand-in Windows headers, as the device library used them before
    flags = ['--target=aarch64-linux-android30', *COMMON_FLAGS, '-fPIE',
             '-I' + str(build_rdvk.DXVK / 'include/native/windows'), '-I' + str(build_rdvk.DXVK / 'include/native/directx')]
    objects = compile_all(compiler_sources(), flags, WORK / 'arm-obj', [str(fb.BIN / 'clang++.exe')])
    exe = WORK / 'rdvk_shaderc-arm64'
    subprocess.run([str(fb.BIN / 'clang++.exe'), '--target=aarch64-linux-android30', '-fPIE', '-pie', '-static-libstdc++',
                    *map(str, objects), '-o', str(exe)], check=True)
    return exe


def build():
    containers, blobs = extract()
    roles = write_input(blobs)
    exe = host_exe()
    done = subprocess.run([str(exe), str(INPUT), str(RAW)], capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError('rdvk_shaderc failed:\n' + done.stderr[-6000:])
    result = json.loads(done.stdout)
    pack(RAW, OUTPUT)
    key = stamp_key(OUTPUT)
    messages = [line for line in done.stderr.splitlines() if line.strip()]
    game = game_directory()
    manifest = dict(library=str(OUTPUT.relative_to(ROOT)).replace('\\', '/'), library_sha256=sha(OUTPUT), key='%016x' % key,
                    raw_sha256=sha(RAW), raw_bytes=RAW.stat().st_size, packed_bytes=OUTPUT.stat().st_size,
                    asset=ASSET,
                    input_sha256=sha(INPUT), containers=containers, unique_dxbc=len(blobs), dxbc_bytes=sum(map(len, blobs)),
                    roles=len(roles), translator=result, compiler_messages=messages[:50],
                    game=dict(exe_sha256=json.loads((ROOT / 'port.json').read_text())['original_exe_sha256'],
                              common_idx_sha256=sha(game / 'common.idx'), common_dat_bytes=(game / 'common.dat').stat().st_size),
                    sources={str(p.relative_to(ROOT)).replace('\\', '/'): sha(p) for p in compiler_sources() + [Path(__file__).resolve()]})
    MANIFEST.write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps({k: manifest[k] for k in ('library_sha256', 'unique_dxbc', 'roles', 'translator')}, indent=1), flush=True)
    return OUTPUT


def stamp_key(path):
    """Header bytes 24..31: the first 8 bytes of the SHA-256 of the library with them zero.  The
    device library is built with the same key (rdvk_shaderlib_key.h) and declines another one."""
    data = bytearray(path.read_bytes())
    data[24:32] = bytes(8)
    key = int.from_bytes(hashlib.sha256(bytes(data)).digest()[:8], 'little')
    data[24:32] = key.to_bytes(8, 'little')
    path.write_bytes(bytes(data))
    return key


def ensure():
    """The library and its key; rebuilt when the translator sources or the game's index changed."""
    if MANIFEST.exists() and OUTPUT.exists():
        manifest = json.loads(MANIFEST.read_text())
        current = {str(p.relative_to(ROOT)).replace('\\', '/'): sha(p) for p in compiler_sources() + [Path(__file__).resolve()]}
        if (manifest.get('sources') == current and manifest.get('library_sha256') == sha(OUTPUT) and
                manifest['game']['common_idx_sha256'] == sha(game_directory() / 'common.idx') and 'key' in manifest):
            return OUTPUT, int(manifest['key'], 16)
    build()
    return OUTPUT, int(json.loads(MANIFEST.read_text())['key'], 16)


def add_to_apk(target):
    """Write the library into an open zipfile.ZipFile as a stored entry (mapped in place on the device)."""
    import zipfile
    library, _ = ensure()
    target.write(library, 'assets/' + ASSET, compress_type=zipfile.ZIP_STORED)


SHADER = struct.Struct('<QI16sIIIIBBHIIII')     # shaderlib.h Shader (64 bytes)
VARIANT = struct.Struct('<8I')                    # shaderlib.h VariantRecord (32 bytes)


def parse(lib):
    """Shaders of a library (raw or packed) as dicts with uniform bytes and raw SPIR-V variants."""
    magic, version, count, shaders, total = struct.unpack_from('<8sIIII', lib, 0)
    if magic != b'RDVKSHL1' or version != 2 or total != len(lib):
        raise ValueError('not a version 2 shader library')
    for i in range(count):
        (h, size, checksum, stage, inputs, outputs, patch, role, nvar, _, uniform, usize, voff, _) = SHADER.unpack_from(lib, shaders + 64 * i)
        rows = []
        for v in range(nvar):
            lo, hi, spirv, words, bindings, nbind, packed, _ = VARIANT.unpack_from(lib, voff + VARIANT.size * v)
            code = zlib.decompress(lib[spirv:spirv + packed]) if packed else lib[spirv:spirv + 4 * words]
            if len(code) != 4 * words:
                raise ValueError('variant size mismatch')
            rows.append(dict(lo=lo, hi=hi, spirv=code, bindings=lib[bindings:bindings + 12 * nbind], count=nbind))
        yield dict(hash=h, size=size, checksum=checksum, stage=stage, inputs=inputs, outputs=outputs, patch=patch, role=role,
                   uniform=lib[uniform:uniform + usize], variants=rows)


def pack(raw, out):
    """The APK form: the same records, each SPIR-V variant compressed on its own (zlib level 9)."""
    shaders = list(parse(raw.read_bytes()))
    data = bytearray(32 + 64 * len(shaders))

    def put(blob):
        data.extend(b'\0' * (-len(data) % 4))
        at = len(data)
        data.extend(blob)
        return at
    for i, s in enumerate(shaders):
        uniform = put(s['uniform']) if s['uniform'] else 0
        records = b''
        for v in s['variants']:
            stream = zlib.compress(v['spirv'], 9)
            at = put(stream)
            bindings = put(v['bindings'])
            records += VARIANT.pack(v['lo'], v['hi'], at, len(v['spirv']) // 4, bindings, v['count'], len(stream), 0)
        voff = put(records)
        SHADER.pack_into(data, 32 + 64 * i, s['hash'], s['size'], s['checksum'], s['stage'], s['inputs'], s['outputs'], s['patch'],
                         s['role'], len(s['variants']), 0, uniform, len(s['uniform']), voff, 0)
    data.extend(b'\0' * (-len(data) % 16))
    struct.pack_into('<8sIIIIII', data, 0, b'RDVKSHL1', 2, len(shaders), 32, len(data), 0, 0)
    out.write_bytes(bytes(data))
    if [dict(s, variants=[dict(v) for v in s['variants']]) for s in parse(bytes(data))] != shaders:
        raise ValueError('packed library does not decode to the raw one')


def variants(lib):
    """(name, raw SPIR-V bytes) of every variant in the library."""
    for s in parse(lib):
        for v in s['variants']:
            yield '%016x-%d-a%d' % (s['hash'], s['stage'], v['lo']), v['spirv']


def validate():
    lib = RAW.read_bytes()
    validator = fb.NDK / 'shader-tools/windows-x86_64/spirv-val.exe'
    rows = list(variants(lib))
    with tempfile.TemporaryDirectory(prefix='rdvk-shaderlib-') as tmp:
        def one(row):
            name, code = row
            path = Path(tmp) / (name + '.spv')
            path.write_bytes(code)
            done = subprocess.run([str(validator), '--target-env', 'vulkan1.3', str(path)], capture_output=True, text=True)
            return name, done.returncode, (done.stdout + done.stderr).strip()[:400]
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            results = list(pool.map(one, rows))
    failed = [dict(name=n, error=e) for n, rc, e in results if rc]
    report = dict(variants=len(results), valid=len(results) - len(failed), failed=failed)
    (WORK / 'validation.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(dict(variants=report['variants'], valid=report['valid'], failed=len(failed)), indent=1))
    for f in failed[:5]:
        print(f['name'], f['error'][:200])
    return report


def verify(serial):
    """Run the arm64 build of the translator on the device and compare the library bytes."""
    if not OUTPUT.exists():
        build()
    exe = arm_exe()
    env = dict(os.environ, MSYS_NO_PATHCONV='1')
    remote = '/data/local/tmp/rdvk-shaderlib'
    adb = ['adb', '-s', serial]
    subprocess.run([*adb, 'shell', 'rm -rf %s && mkdir -p %s' % (remote, remote)], check=True, env=env)
    subprocess.run([*adb, 'push', str(exe), remote + '/rdvk_shaderc'], check=True, env=env, capture_output=True)
    subprocess.run([*adb, 'push', str(INPUT), remote + '/input.bin'], check=True, env=env, capture_output=True)
    done = subprocess.run([*adb, 'shell', 'cd %s && chmod 755 rdvk_shaderc && ./rdvk_shaderc input.bin out.bin 2>/dev/null && sha256sum out.bin' % remote],
                          capture_output=True, text=True, env=env)
    subprocess.run([*adb, 'shell', 'rm -rf ' + remote], env=env)
    lines = done.stdout.strip().splitlines()
    device_sha = lines[-1].split()[0] if lines else ''
    result = dict(serial='device', device_stdout=lines[:1], device_sha256=device_sha, host_sha256=sha(RAW),
                  identical=device_sha == sha(RAW), arm_exe_sha256=sha(exe))
    (WORK / 'device-equivalence.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps(result, indent=1))
    if not result['identical']:
        raise SystemExit('device translation differs from the build-time library')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    sub.add_parser('build')
    sub.add_parser('validate')
    p = sub.add_parser('verify')
    p.add_argument('serial')
    args = ap.parse_args()
    if args.command == 'build':
        build()
    elif args.command == 'validate':
        validate()
    else:
        verify(args.serial)


if __name__ == '__main__':
    main()
