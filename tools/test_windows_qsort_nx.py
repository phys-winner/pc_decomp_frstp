"""Check System32 qsort's NX fallback and verified descending-u32 callback fast path."""
import hashlib
import json
from pathlib import Path
import subprocess
import pefile
from leaf_probe import ROOT, windows_toolchain


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    config=json.loads((ROOT/'port.json').read_text())
    binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out=ROOT/'build/windows-qsort-nx-test';out.mkdir(parents=True,exist_ok=True)
    overlay=out/'include/cpu64.h';overlay.parent.mkdir(exist_ok=True)
    overlay.write_text((ROOT/'native/runtime64/cpu64.h').read_text().replace(
        '#if defined(__aarch64__)','#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)',1),encoding='utf-8',newline='\n')
    originals=[ROOT/'generated/frostpunk-x64/recomp_funcs_0986.c',
               ROOT/'generated/frostpunk-x64/recomp_funcs_0832.c']
    original_exe=Path(config['game_directory'])/'Frostpunk.exe'
    assert digest(original_exe)==config['original_exe_sha256'],'Original Frostpunk hash changed'
    pe=pefile.PE(str(original_exe),fast_load=True)
    raw=bytes.fromhex('8b018b0a3bc87606b801000000c31bc0c3')
    assert pe.get_data(0x14102f320-pe.OPTIONAL_HEADER.ImageBase,len(raw))==raw
    pointer_raw=bytes.fromhex('488b01448b00488b0233d28b088bc2443bc10f97c00f92c22bc2c3')
    assert pe.get_data(0x141c561a0-pe.OPTIONAL_HEADER.ImageBase,len(pointer_raw))==pointer_raw
    float_raw=bytes.fromhex('488b024c8b01f30f108000020000f3410f5c8000020000f30f2cc085c07e06b801000000c37906b8ffffffffc38b42102b4110750e8b42182b411875068b42202b4120c3')
    assert pe.get_data(0x1410310d0-pe.OPTIONAL_HEADER.ImageBase,len(float_raw))==float_raw
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    assert any(i.address==0x141cc80f0 and i.name==b'qsort'
               for library in pe.DIRECTORY_ENTRY_IMPORT for i in library.imports)
    bodies=[]
    for original,name in [(originals[0],'L_000141C561A0'),(originals[1],'L_00014102F320'),
                          (originals[1],'L_0001410310D0')]:
        text=original.read_text();start=text.index('void '+name+'(CPU *c)\n{')
        end=text.index('    /* int3 */',start);bodies.append(text[start:end]+'}\n')
    comparator=out/'include/qsort_original_comparator.h'
    comparator.write_text('\n'.join(bodies)+'\nstatic const unsigned char qsort_u32_original_bytes[]={'
                          +','.join(hex(b) for b in raw)+'};\n'
                          +'static const unsigned char qsort_pointer_original_bytes[]={'
                          +','.join(hex(b) for b in pointer_raw)+'};\n'
                          +'static const unsigned char qsort_float_original_bytes[]={'
                          +','.join(hex(b) for b in float_raw)+'};\n',encoding='utf-8',newline='\n')
    sources=[ROOT/'tests/windows_qsort_nx_fixture.c',ROOT/'native/runtime64/eh64.c',
        *[p for p in sorted((ROOT/'native/windows').glob('*.c')) if p.name!='host.c'],
        *sorted((ROOT/'native/windows').glob('*.asm'))]
    watched=[*sources,ROOT/'native/windows/host.c',*sorted((ROOT/'native/windows').glob('*.h')),
        *sorted((ROOT/'native/windows').glob('*.inc')),*sorted((ROOT/'native/runtime64').glob('*.h')),
        *originals,original_exe,ROOT/'port.json',overlay,comparator,Path(__file__).resolve()]
    before={str(p):digest(p) for p in watched}
    def run(command):
        process=subprocess.run(list(map(str,command)),env=env,cwd=out,capture_output=True,
            text=True,encoding='utf-8',errors='replace',timeout=60)
        if process.returncode:raise RuntimeError(process.stdout+process.stderr)
        return process.stdout+process.stderr
    includes=[overlay.parent,ROOT/'native/runtime64',ROOT/'native/windows',
        ROOT/'generated/frostpunk-x64',ROOT/'generated/supplemental']
    objects=[]
    for source in sources:
        obj=out/(source.stem+'.obj');objects.append(obj)
        if source.suffix=='.asm':run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(obj),source])
        else:run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing',
            '-fwrapv','-ffp-contract=off','-DFROSTPUNK_WINDOWS_HOST=1','-include',overlay,
            *['-I'+str(p) for p in includes],'-c',source,'-o',obj])
    exe=out/'fixture.exe'
    run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000','/DYNAMICBASE:NO',
        '/STACK:16777216','/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',*objects,'libcmt.lib','oldnames.lib',
        'kernel32.lib','user32.lib','advapi32.lib','shell32.lib','ole32.lib','winmm.lib','version.lib',
        'dbghelp.lib','d3d11.lib','dxgi.lib'])
    output=run([exe]);proof=json.loads(output);assert proof['success']
    assert before=={p:digest(Path(p)) for p in before},'Fixture sources changed during compilation'
    proof.update(source_sha256=before,executable_sha256=digest(exe),game_boot_proven=False,
                 original_exe_sha256=config['original_exe_sha256'],qsort_u32_va='0x14102f320',
                 qsort_u32_original_bytes=raw.hex(),qsort_import_slot='0x141cc80f0',
                 qsort_pointer_va='0x141c561a0',qsort_pointer_original_bytes=pointer_raw.hex(),
                 qsort_float_va='0x1410310d0',qsort_float_original_bytes=float_raw.hex(),
                 float_MXCSR_contract='Preserve current NX callback input-MXCSR restoration; original return values exact; original sticky flags intentionally differ from current callback boundary',
                 gameplay_FPS_measured=False)
    (out/'result.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in proof.items() if k!='source_sha256'},indent=2))


if __name__=='__main__':main()
