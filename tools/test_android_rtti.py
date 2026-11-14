"""Bounded main-image public-SI __RTDynamicCast; UCRT data-only oracle, no ADB."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from leaf_probe import ROOT, windows_toolchain

OUT = ROOT / 'build/android-rtti-test'
INPUT = struct.Struct('<8I')
ROW = struct.Struct('<8IQIiII')
ROUTE = '    if (!strcasecmp(dll,"vcruntime140.dll") && win64_rtti_call(c,name)) { c->rsp+=8; return 1; }'
PRODUCTION_ROUTE = '    if (!strcasecmp(dll,"vcruntime140.dll") && win64_rtti_call(c,name)) IMPORT_DONE;'


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    original = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(original) != cfg['original_exe_sha256']:
        raise ValueError('Original PE source pin differs')
    files = ['android/host/win64_rtti.c', 'android/host/win64_rtti.h', 'android/host/win64_rtti_names.inc', 'android/host/win64.h', 'android/host/win64_stdio.h',
             'native/runtime64/cpu64.h', 'tests/rtti_fixture.c', 'tests/rtti_oracle.c',
             'tools/test_android_rtti.py']
    core_path = ROOT / 'android/host/win64.c'
    bootstrap_path = ROOT / 'android/host/bootstrap.c'
    core = core_path.read_text()
    bootstrap = bootstrap_path.read_text()
    integrated = core.count(PRODUCTION_ROUTE) == 1 and 'win64_rtti_init(image,pe.image_size)' in bootstrap
    if '#define IMPORT_DONE do { *handled=step; c->rsp+=8; return 1; } while (0)' not in core:
        raise ValueError('Production import completion must pop exactly once')
    if integrated:
        if '#include "win64_rtti.h"' not in core or '#include "win64_rtti.h"' not in bootstrap or bootstrap.index('win64_rtti_init(image,pe.image_size)') > bootstrap.index('win64_init(pe.base,pe.image_size,stack_lo,STACK_BYTES)'):
            raise ValueError('Integrated RTTI header/init order differs')
        files += ['android/host/win64.c', 'android/host/bootstrap.c']
    pins = {name: sha(ROOT / name) for name in files}
    import pefile
    pe = pefile.PE(str(original), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    imports = [(entry.dll.decode(), symbol.name.decode(), hex(symbol.address))
               for entry in pe.DIRECTORY_ENTRY_IMPORT for symbol in entry.imports
               if symbol.name and symbol.address == 0x141cc79c0]
    if imports != [('VCRUNTIME140.dll', '__RTDynamicCast', '0x141cc79c0')]:
        raise ValueError('Original RTTI import differs')
    image_base = pe.OPTIONAL_HEADER.ImageBase
    mapped = pe.get_memory_mapped_image()
    read = lambda address, size: mapped[address - image_base:address - image_base + size]
    u32 = lambda address: struct.unpack('<I', read(address, 4))[0]
    type_name = lambda address: read(address + 16, 512).split(b'\0', 1)[0].decode('ascii')
    spans = []
    expected = {
        0x141ae324c: 'ff256e471e00',
        0x141020561: '44896424204c8d0ddb0989014c8d05ac09890133d2e8d12cac00',
    }
    for address, text in expected.items():
        if read(address, len(bytes.fromhex(text))) != bytes.fromhex(text):
            raise ValueError('Original actual cast boundary differs at ' + hex(address))
    for address, size, role in [(0x141020561, 0x4c, 'SFXElementContext to AnimatedAlphaQuad, then Particle; VfDelta0,isReference0'),
                                 (0x141ae324c, 6, 'RTDynamicCast IAT thunk')]:
        data = read(address, size)
        spans.append(dict(va=hex(address), role=role, bytes=data.hex(), sha256=hashlib.sha256(data).hexdigest()))
    sfx = []
    total = 0
    for section in pe.sections:
        if not section.Characteristics & 0x40000000 or section.Characteristics & 0x20000000:
            continue
        raw = section.get_data()
        for offset in range(0, len(raw) - 24, 4):
            signature, displacement, construction, td, hierarchy, self_rva = struct.unpack_from('<6I', raw, offset)
            if signature != 1 or self_rva != section.VirtualAddress + offset or td + 20 > len(mapped) or hierarchy + 16 > len(mapped):
                continue
            if mapped[td + 16:td + 20] not in (b'.?AV', b'.?AU'):
                continue
            total += 1
            sig, attributes, count, array = struct.unpack('<4I', read(image_base + hierarchy, 16))
            if not 0 < count <= 256:
                continue
            bases = []
            for index in range(count):
                descriptor = image_base + u32(image_base + array + index * 4)
                target, contained, member, vbpointer, vbentry, flags, sub = struct.unpack('<IIiiiII', read(descriptor, 28))
                bases.append(dict(va=hex(descriptor), type=hex(image_base + target), name=type_name(image_base + target),
                    contained=contained, pmd=[member, vbpointer, vbentry], attributes=flags, hierarchy=hex(image_base + sub),
                    bytes=read(descriptor, 28).hex()))
            if any(base['type'] == '0x1428b0f20' for base in bases):
                if displacement or construction or sig or attributes or any(base['pmd'] != [0, -1, 0] or base['attributes'] != 0x40 or base['contained'] != count-index-1 for index, base in enumerate(bases)):
                    raise ValueError('Actual SFX closure escaped public zero-PMD SI profile')
                sfx.append(dict(va=hex(image_base + self_rva), name=type_name(image_base + td), type=hex(image_base + td),
                    bytes=read(image_base + self_rva, 24).hex(), hierarchy_va=hex(image_base + hierarchy),
                    hierarchy_bytes=read(image_base + hierarchy, 16).hex(), array_bytes=read(image_base + array, count * 4).hex(), bases=bases))
    if len(sfx) != 11:
        raise ValueError('Original SFX class closure count differs')
    (OUT / 'original-sfx-rtti.json').write_text(json.dumps(dict(original_sha256=cfg['original_exe_sha256'],
        scanned_locators=total, sfx=sfx, actual_import=imports, actual_call_spans=spans), indent=2) + '\n')

    # Compact relocation preserves original metadata values/names and zero-PMD
    # topology. No original function pointer or instruction is executed.
    data = bytearray(32768)
    cursor = 256
    def allocate(raw, alignment=4):
        nonlocal cursor
        cursor = (cursor + alignment - 1) & -alignment
        offset = cursor
        data[offset:offset + len(raw)] = raw
        cursor += len(raw)
        if cursor > len(data):
            raise ValueError('Compact metadata image overflow')
        return offset
    descriptors = {}
    names = [model['name'] for model in sfx] + ['.?AVFixtureMid@@', '.?AVFixtureLeaf@@', '.?AVFixtureAlien@@']
    for name in names:
        descriptors[name] = allocate(bytes(16) + name.encode() + b'\0', 8)
    root = '.?AVSFXElementContext@@'
    models = [(model['name'], [base['name'] for base in model['bases']]) for model in sfx]
    models += [('FixtureMid', ['.?AVFixtureMid@@', root]),
               ('FixtureLeaf', ['.?AVFixtureLeaf@@', '.?AVFixtureMid@@', root]),
               ('FixtureAlien', ['.?AVFixtureAlien@@'])]
    tables = []
    for label, chain in models:
        bases = []
        for index, name in enumerate(chain):
            # pClassDescriptor is present/within image, but SI native search
            # does not traverse it. Point at the local hierarchy below.
            bases.append(allocate(bytes(28)))
        array = allocate(struct.pack('<' + 'I' * len(bases), *bases))
        hierarchy = allocate(struct.pack('<4I', 0, 0, len(bases), array))
        for index, (address, name) in enumerate(zip(bases, chain)):
            struct.pack_into('<IIiiiII', data, address, descriptors[name], len(chain) - index - 1, 0, -1, 0, 0x40, hierarchy)
        locator = allocate(bytes(24))
        struct.pack_into('<6I', data, locator, 1, 0, 0, descriptors[chain[0]], hierarchy, locator)
        prefix = allocate(struct.pack('<QQ', locator, 0), 8)
        tables.append((label, prefix + 8))
    # Extra descriptors model native name fallback and its identity-pass
    # precedence, without claiming arbitrary foreign-module metadata support.
    for name in [root, '.?AVSFXAnimatedAlphaQuadElementContext@@', '.?AVFixtureMid@@']:
        descriptors['clone:' + name] = allocate(bytes(16) + name.encode() + b'\0', 8)
    image_path = OUT / 'metadata.bin'
    image_path.write_bytes(data)
    inputs = OUT / 'inputs.bin'
    count = 0
    with inputs.open('wb') as stream:
        for label, table in tables:
            for source in descriptors.values():
                for target in descriptors.values():
                    for mode in range(16):
                        stream.write(INPUT.pack(table, source, target, 0, 0, 0, mode, count));count += 1
        for mode in range(16):
            stream.write(INPUT.pack(tables[0][1], 0xffffffff, 0xffffffff, 1, 0xdead1234, 1, mode, count));count += 1
    (OUT / 'rtti_route.inc').write_text(ROUTE + '\n')
    (OUT / 'integration.patch').write_text('Add #include "win64_rtti.h" to win64.c/bootstrap.c as appropriate.\n'
        'Call win64_rtti_init(mapped_main_image,image_size) before guest threads.\n' + ROUTE + '\n')
    bins, env = windows_toolchain()
    commands = []
    def run(arguments, expected=0):
        command = list(map(str, arguments));commands.append(command)
        result = subprocess.run(command, cwd=OUT, env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != expected:
            (OUT / 'failure.json').write_text(json.dumps(dict(command=command, returncode=result.returncode, stdout=result.stdout, stderr=result.stderr), indent=2) + '\n')
            raise RuntimeError(subprocess.list2cmdline(command) + '\n' + result.stdout[-6000:] + result.stderr[-6000:])
        return result
    oracle = OUT / 'rtti-oracle.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', ROOT / 'tests/rtti_oracle.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(oracle)])
    cases = OUT / 'cases.bin'
    native_run = run([oracle, image_path, inputs, cases])
    native = json.loads(native_run.stdout)
    if not native['success'] or native['cases'] != count or cases.stat().st_size != count * ROW.size:
        raise ValueError('Native data-only RTTI oracle differs')
    include = [ROOT / 'native/runtime64', ROOT / 'android/host', OUT]
    host = OUT / 'rtti-host.exe'
    run([bins / 'cl.exe', '/nologo', '/O2', '/fp:strict', '/std:c11', '/experimental:c11atomics',
         *['/I' + str(path) for path in include], ROOT / 'tests/rtti_fixture.c', ROOT / 'android/host/win64_rtti.c',
         '/Fo' + str(OUT) + '/', '/Fe' + str(host)])
    host_run = run([host, image_path, cases]);host_proof = json.loads(host_run.stdout)
    (OUT / 'host.log').write_text(host_run.stdout + host_run.stderr)
    if not host_proof['success'] or host_proof['rows'] != count or host_proof['calls'] != count or host_proof['failures']:
        raise ValueError('Host RTTI equivalence differs')
    negatives = ['delta', 'reference', 'stack', 'stack_overflow', 'object', 'vtable', 'locator_self', 'locator_offset',
                 'construction', 'multiple', 'virtual', 'count', 'array', 'pmd', 'private', 'source', 'target', 'uninitialized']
    negative_results = []
    for name in negatives:
        result = run([host, image_path, cases, name], expected=73)
        if 'STOP Win64 RTTI' not in result.stderr:
            raise ValueError('Missing typed RTTI rejection diagnostic')
        negative_results.append(dict(arg=name, exit_code=73, stderr=result.stderr))
    ndk = Path(cfg['android_sdk']) / 'ndk' / cfg['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'
    arm = OUT / 'rtti-arm64'
    run([ndk / 'clang.exe', '--target=aarch64-linux-android30', '-O2', '-std=c11', '-ffp-contract=off', '-fno-strict-aliasing',
         '-fwrapv', '-ffunction-sections', '-fdata-sections', *['-I' + str(path) for path in include], ROOT / 'tests/rtti_fixture.c',
         ROOT / 'android/host/win64_rtti.c', '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384', '-pthread', '-o', arm])
    if pins != {name: sha(ROOT / name) for name in files}:
        raise ValueError('Owned qualification source changed during proof')
    msvc = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
    native_sources = {str(path): sha(path) for path in [msvc / 'include/rttidata.h', msvc / 'crt/src/vcruntime/rtti.cpp']}
    report = dict(success=True, sources=pins, native_reference_sources=native_sources, original_sha256=cfg['original_exe_sha256'],
        original_import=imports, original_spans=spans, original_sfx_closure=dict(path=str((OUT / 'original-sfx-rtti.json').relative_to(ROOT)),
            sha256=sha(OUT / 'original-sfx-rtti.json'), dynamic_classes=len(sfx), all_public_SI_zero_PMD=True),
        native_oracle=native, host=host_proof, negative_cases=negative_results, commands=commands,
        fixture_data=dict(path=str(image_path.relative_to(ROOT)), sha256=sha(image_path)),
        dataset=dict(path=str(cases.relative_to(ROOT)), sha256=sha(cases), row_bytes=ROW.size, count=count),
        binary=dict(path=str(arm.relative_to(ROOT)), sha256=sha(arm), args=['metadata.bin', 'cases.bin']),
        integration=dict(route=ROUTE, init='win64_rtti_init(mapped_main_image,image_size) before guest threads; checked return required',
            integrated=integrated, core_modified_by_this_producer=False, bootstrap_modified_by_this_producer=False),
        scope='Main-image public single inheritance only; COL1/offset0/cdOffset0, PMD0/-1/0, complete-to-root chain <=64, bounded512byte type names, pointer casts. Native SI pointer-identity pass precedes names; source must occur after target. Actual SFX11 models + synthetic3depth, same-name copies, direct API upcast/same-type failures, null-fastpath with invalid unused arguments. No MI/virtual/construction/nonpublic/reference/foreign-image claim; unsupported forms STOP73.',
        fixture_scope='Exact production provider compiled on host and ARM. Pointer-region checker is an explicit bounded fixture double; main core initialization/provider precedence is not simulated. Exact DLL route and one outer pop are tested separately; integration presence/sourcepins recorded explicitly.',
        documentation='https://learn.microsoft.com/en-us/cpp/c-runtime-library/rtdynamiccast?view=msvc-170',
        original_bytes_executed=False, device_actions=False, arm_executed=False)
    result_path = OUT / 'result.json';result_path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(success=True, native=native, host=host_proof, data=report['fixture_data'], dataset=report['dataset'],
        arm=report['binary'], receipt_sha256=sha(result_path)), indent=2))


if __name__ == '__main__':
    main()
