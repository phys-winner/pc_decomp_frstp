"""Stripped APK copies and the versioned, ARM64-filtered XZ game runtime.

XZ Embedded is pinned as in ../wiki/07-the-void-lessons.md. Build/debug DSOs
remain untouched; only these packaging copies lose their static symbol tables.
"""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
XZ_SOURCE = ROOT / 'reference/xz-embedded'
XZ_REVISION = 'ae63ae3a36ed01724674e8f3d750dc47bf125410'
RUNTIME = 'libfrostpunk_runtime.so'
CHUNK = 64 << 20
LINK_FLAGS = ['-Wl,--gc-sections', '-Wl,--icf=safe', '-Wl,--pack-dyn-relocs=android+relr']
COMPILE_FLAGS = ['-g0', '-ffunction-sections', '-fdata-sections']


def sha(path):
    with Path(path).open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def xz_tool():
    for candidate in (os.environ.get('XZ'), shutil.which('xz'), r'C:\Program Files\Git\mingw64\bin\xz.exe'):
        if candidate and Path(candidate).is_file():
            version = subprocess.check_output([candidate, '--robot', '--version'], text=True)
            number = int(next(line.split('=')[1] for line in version.splitlines() if line.startswith('XZ_VERSION=')))
            if number >= 50040002:
                return str(candidate)
    raise RuntimeError('XZ Utils >= 5.4 required for the ARM64 BCJ filter; set XZ or install Git for Windows')


def build_unpacker(llvm, work, *, cli=False):
    actual = subprocess.check_output(['git', '-C', str(XZ_SOURCE), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '-C', str(XZ_SOURCE), 'status', '--porcelain', '--untracked-files=no'], text=True)
    if actual != XZ_REVISION or dirty.strip():
        raise ValueError('XZ Embedded source pin changed: ' + XZ_REVISION)
    work.mkdir(parents=True, exist_ok=True)
    decoder = XZ_SOURCE / 'linux/lib/xz'
    sources = [ROOT / 'android/unpack/unpack.c', *(decoder / n for n in
               ('xz_crc32.c', 'xz_dec_stream.c', 'xz_dec_lzma2.c', 'xz_dec_bcj.c'))]
    output = work / ('xzunpack' if cli else 'libfrostpunk_xz.so')
    command = [str(llvm / 'clang.exe'), '--target=aarch64-linux-android30', '-O2', '-std=gnu11',
               '-fPIC', *COMPILE_FLAGS, '-Wall', '-Wextra', '-DXZ_DEC_ARM64', '-DXZ_DEC_SINGLE',
               '-I' + str(XZ_SOURCE / 'linux/include/linux'), '-I' + str(XZ_SOURCE / 'userspace'),
               *map(str, sources), *LINK_FLAGS, '-Wl,-z,max-page-size=16384', '-Wl,--no-undefined',
               '-Wl,--build-id=sha1', '-Wl,-s', '-pthread',
               *(['-DUNPACK_MAIN', '-fPIE', '-pie'] if cli else ['-shared', '-landroid']), '-o', str(output)]
    subprocess.run(command, check=True, capture_output=True)
    return output


def compress_library(path, work):
    digest = sha(path)
    work.mkdir(parents=True, exist_ok=True)
    output = work / f'{digest}-{CHUNK >> 20}m-arm64-lp2.xz'
    manifest = output.with_suffix('.json')
    if output.is_file() and manifest.is_file():
        previous = json.loads(manifest.read_text())
        if sha(output) == previous['xz_sha256']:
            return output, previous
    tool = xz_tool()
    command = [tool, '--stdout', '--arm64', f'--lzma2=preset=9e,dict={CHUNK},lc=0,lp=2,pb=2',
               '--check=crc32', '--threads=1']

    def pack(offset):
        with path.open('rb') as source:
            source.seek(offset)
            data = source.read(CHUNK)
        return subprocess.run(command, input=data, capture_output=True, check=True).stdout

    # Two 64 MiB encoders bound peak memory; decoding uses the output itself as dictionary.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        chunks = list(pool.map(pack, range(0, path.stat().st_size, CHUNK)))
    part = output.with_suffix('.part')
    with part.open('wb') as destination:
        for chunk in chunks:
            destination.write(chunk)
    part.replace(output)
    result = dict(bytes=path.stat().st_size, sha256=digest, chunk_bytes=CHUNK,
                  packed_sizes=[len(chunk) for chunk in chunks], xz_bytes=output.stat().st_size,
                  xz_sha256=sha(output), command=command, xz_version=subprocess.check_output([tool, '--version'], text=True).splitlines()[0])
    manifest.write_text(json.dumps(result, indent=2) + '\n')
    return output, result


def align_stripped_segments(path):
    """LLVM 18 can misalign a patchelf-added LOAD (the pinned Turnip's notes/dynstr)."""
    data = bytearray(path.read_bytes())
    phoff, shoff = struct.unpack_from('<QQ', data, 32)
    phsize, phnum, shsize, shnum = struct.unpack_from('<HHHH', data, 54)
    programs = [struct.unpack_from('<IIQQQQQQ', data, phoff+i*phsize) for i in range(phnum)]
    repaired = 0
    for segment in programs:
        kind, _, offset, address, _, size, _, alignment = segment
        if kind != 1 or alignment <= 1 or offset % alignment == address % alignment:
            continue
        if offset < 64 or offset+size > len(data):
            raise ValueError('Invalid stripped ELF segment')
        new = ((len(data)-address%alignment+alignment-1)//alignment)*alignment + address%alignment
        content = bytes(data[offset:offset+size])
        data.extend(b'\0'*(new-len(data)))
        data.extend(content)
        data[offset:offset+size] = b'\0'*size
        for i,p in enumerate(programs):
            if offset <= p[2] < offset+size:
                struct.pack_into('<Q', data, phoff+i*phsize+8, new+p[2]-offset)
        for i in range(shnum):
            section = shoff+i*shsize
            location = struct.unpack_from('<Q', data, section+24)[0]
            if offset <= location < offset+size and struct.unpack_from('<I', data, section+4)[0] != 8:
                struct.pack_into('<Q', data, section+24, new+location-offset)
        repaired += 1
    if repaired:
        path.write_bytes(data)
    return repaired


def prepare(libraries, llvm, work):
    """Return ZIP entries {name: (file, method)} and actual packaged-library sizes/hashes."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    entries, report = {}, {}
    for name, source in sorted(libraries.items()):
        if Path(name).name != name or not name.endswith('.so'):
            raise ValueError('Invalid native library name: ' + name)
        source = Path(source)
        target = work / name
        subprocess.run([str(llvm / 'llvm-strip.exe'), '--strip-unneeded', '-o', str(target), str(source)], check=True)
        realigned = align_stripped_segments(target)
        def exports(path):
            text = subprocess.check_output([str(llvm/'llvm-nm.exe'), '-D', '--defined-only', str(path)], text=True)
            return sorted(line.split()[-1] for line in text.splitlines())
        if exports(source) != exports(target):
            raise ValueError('Stripping changed the native export ABI: ' + name)
        report[name] = dict(source_bytes=source.stat().st_size, bytes=target.stat().st_size,
                            source_sha256=sha(source), sha256=sha(target), exports_unchanged=True,
                            realigned_segments=realigned)
        if name != RUNTIME:
            entries['lib/arm64-v8a/' + name] = (target, zipfile.ZIP_DEFLATED)
    if RUNTIME not in libraries:
        raise ValueError('Game APK requires ' + RUNTIME)
    unpacker = build_unpacker(Path(llvm), work)
    entries['lib/arm64-v8a/' + unpacker.name] = (unpacker, zipfile.ZIP_DEFLATED)
    packed, compression = compress_library(work / RUNTIME, ROOT / 'build/native-xz')
    index = work / 'index.txt'
    index.write_text(f'{RUNTIME} {compression["bytes"]} {compression["sha256"]} {CHUNK} '
                     + ','.join(map(str, compression['packed_sizes'])) + '\n', encoding='ascii')
    entries['assets/native/' + RUNTIME + '.xz'] = (packed, zipfile.ZIP_STORED)
    entries['assets/native/index.txt'] = (index, zipfile.ZIP_STORED)
    entries['assets/licenses/XZ-Embedded.txt'] = (XZ_SOURCE / 'COPYING', zipfile.ZIP_DEFLATED)
    origin = work / 'XZ-Embedded-origin.json'
    origin.write_text(json.dumps(dict(url='https://github.com/tukaani-project/xz-embedded.git',
                                    revision=XZ_REVISION, license='0BSD'), indent=2) + '\n')
    entries['assets/licenses/XZ-Embedded-origin.json'] = (origin, zipfile.ZIP_DEFLATED)
    receipt = dict(libraries=report, runtime_xz=compression, unpacker_sha256=sha(unpacker),
                   source_revision=XZ_REVISION, zip_native_compression='deflate-9', elf_page_alignment=16384)
    sources = [Path(__file__).resolve(), ROOT/'android/unpack/unpack.c', Path(llvm)/'clang.exe',
               Path(llvm)/'llvm-strip.exe', ROOT/'android/app/NativeLibrary.java',
               XZ_SOURCE/'COPYING', XZ_SOURCE/'linux/include/linux/xz.h', XZ_SOURCE/'userspace/xz_config.h',
               *sorted((XZ_SOURCE/'linux/lib/xz').glob('*.h')),
               *(XZ_SOURCE/'linux/lib/xz'/name for name in ('xz_crc32.c','xz_dec_stream.c','xz_dec_lzma2.c','xz_dec_bcj.c'))]
    receipt['source_inputs'] = {str(path):sha(path) for path in sources}
    (work / 'report.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return entries, receipt


def verify_payload(archive, receipt, work):
    """Verify actual APK contents, including decoding its runtime, for either builder."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    packed = receipt['runtime_xz']
    fields = archive.read('assets/native/index.txt').decode('ascii').split()
    expected = [RUNTIME, str(packed['bytes']), packed['sha256'], str(packed['chunk_bytes']),
                ','.join(map(str, packed['packed_sizes']))]
    if fields != expected or 'lib/arm64-v8a/' + RUNTIME in archive.namelist():
        raise ValueError('APK runtime index/placement mismatch')
    xz_name = 'assets/native/' + RUNTIME + '.xz'
    if archive.getinfo(xz_name).compress_type != zipfile.ZIP_STORED:
        raise ValueError('XZ payload must be stored without ZIP compression')
    xz = work / (RUNTIME + '.xz')
    with archive.open(xz_name) as source, xz.open('wb') as target:
        shutil.copyfileobj(source, target)
    if sha(xz) != packed['xz_sha256'] or xz.stat().st_size != sum(packed['packed_sizes']):
        raise ValueError('APK XZ content mismatch')
    with (work / RUNTIME).open('wb') as target:
        subprocess.run([xz_tool(), '--decompress', '--stdout', str(xz)], stdout=target, check=True)
    actual = {}
    for info in archive.infolist():
        if info.filename.startswith('lib/arm64-v8a/'):
            name = Path(info.filename).name
            path = work / name
            with archive.open(info) as source, path.open('wb') as target:
                shutil.copyfileobj(source, target)
            actual[name] = sha(path)
            expected_hash = receipt['unpacker_sha256'] if name == 'libfrostpunk_xz.so' else receipt['libraries'][name]['sha256']
            if actual[name] != expected_hash:
                raise ValueError('Packaged native library differs: ' + name)
    actual[RUNTIME] = sha(work / RUNTIME)
    if actual[RUNTIME] != packed['sha256'] or (work/RUNTIME).stat().st_size != packed['bytes']:
        raise ValueError('Decoded APK runtime differs')
    return actual
