import argparse
import json
import statistics
import struct
import subprocess
import sys
from pathlib import Path
from elftools.elf.elffile import ELFFile
import fastbuild as fb
from fastbuild import sha

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/perf/fps45-20261112'
sys.path.insert(0,str(ROOT/'tools'))
from leaf_probe import windows_toolchain
from test_android_render_scale import function

def main():
    parser=argparse.ArgumentParser(description='Exact native frustum differential test')
    parser.add_argument('--serial',required=True)
    parser.add_argument('--compile-only',action='store_true')
    parser.add_argument('--fallback',action='store_true')
    args=parser.parse_args()
    folder=OUT/'native-frustum'
    folder.mkdir(parents=True,exist_ok=True)
    original=ROOT/'build/arm64/eh-overlay-objects/recomp_funcs_0804.o'
    assert sha(original)=='b4254ad1c258f5d8eca75419ea027f56c5cad0d5b46736f6191a6d5a07643c2a'
    cfg=json.loads((ROOT/'port.json').read_text())
    exe=Path(cfg['game_directory'])/'Frostpunk.exe'
    assert sha(exe)==cfg['original_exe_sha256']
    mapped=folder/'original-mapped.bin'
    raw=exe.read_bytes();nt=struct.unpack_from('<I',raw,0x3c)[0]
    count,optional=struct.unpack_from('<H',raw,nt+6)[0],struct.unpack_from('<H',raw,nt+20)[0]
    size,headers=struct.unpack_from('<II',raw,nt+24+56)
    assert size==0x4539000 and headers<=len(raw)
    image=bytearray(size);image[:headers]=raw[:headers]
    for i in range(count):
        at=nt+24+optional+i*40;va,bytes_,offset=struct.unpack_from('<III',raw,at+12)
        assert va<=size and bytes_<=size-va and offset<=len(raw) and bytes_<=len(raw)-offset
        image[va:va+bytes_]=raw[offset:offset+bytes_]
    mapped.write_bytes(image)
    sources=[ROOT/'tests/android_frustum_native_fixture.c',ROOT/'android/host/frustum_native.c',ROOT/'android/host/aot_cpu.c',ROOT/'android/host/aot_cpu_float.c']
    pins={str(p):sha(p) for p in [*sources,Path(__file__).resolve(),ROOT/'build/arm64/generated-overlay/recomp_funcs_0804.c',
                                ROOT/'android/host/aot_cpu_sites.inc',ROOT/'native/runtime64/cpu64.h',original]}
    bins,env=windows_toolchain()


    def run(command):
        result=subprocess.run(list(map(str,command)),env=env,capture_output=True,text=True,timeout=120)
        if result.returncode:raise RuntimeError(str(result.returncode)+'\n'+result.stdout[-1200:]+result.stderr[-4000:])
        return result.stdout


    renamed=folder/'production-original.o'
    run([fb.BIN/'llvm-objcopy.exe','--redefine-sym=L_000140E2C890=fp_frustum_original',original,renamed])
    with original.open('rb') as a,renamed.open('rb') as b:
        old,new=ELFFile(a),ELFFile(b);assert old.get_section_by_name('.text').data()==new.get_section_by_name('.text').data()
        undefined=[s.name for s in old.get_section_by_name('.symtab').iter_symbols() if s['st_shndx']=='SHN_UNDEF' and s.name]
    stubs=folder/'unreachable.c'
    stubs.write_text('#include <stdlib.h>\n'+'\n'.join('void '+name+'(void){abort();}' for name in undefined
                     if name.startswith(('L_','dispatch','es3_','frostpunk_','fp_dispatch')))+'\n')
    oracle=folder/'host-original.c'
    oracle.write_text('#include "cpu64.h"\n#include "aot_cpu.h"\n'+function((ROOT/'build/arm64/generated-overlay/recomp_funcs_0804.c').read_text(),'140E2C890')
                      .replace('void L_000140E2C890(', 'void fp_frustum_original(',1))
    flags=['-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-fno-math-errno',
           '-ffunction-sections','-fdata-sections','-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host')]
    host_objects=[]
    for i,source in enumerate([*sources,oracle,stubs]):
        obj=folder/('host-%d.obj'%i)
        run([fb.CLANG,'--target=x86_64-pc-windows-msvc',*flags,'-fms-runtime-lib=static','-c',source,'-o',obj]);host_objects.append(obj)
    host=folder/'frustum-host.exe'
    run([bins/'link.exe','/nologo','/OPT:REF','/OUT:'+str(host),*host_objects,'libcmt.lib','libvcruntime.lib','libucrt.lib','oldnames.lib','kernel32.lib'])
    host_result=json.loads(run([host,mapped]));assert host_result['success']
    (folder/'host-result.json').write_text(json.dumps(host_result,indent=1)+'\n')
    objects=[]
    for i,source in enumerate([*sources,stubs]):
        obj=folder/('arm-%d.o'%i)
        run([fb.CLANG,*fb.COMPILE_FLAGS,*([] if source.name=='frustum_native.c' else ['-ffunction-sections','-fdata-sections']),*['-I'+str(ROOT/p) for p in fb.INCLUDES],'-c',source,'-o',obj]);objects.append(obj)
    arm=folder/'frustum-arm64'
    run([fb.CLANG,'--target=aarch64-linux-android30',*objects,renamed,'-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-lm','-o',arm])
    if args.compile_only:
        print(json.dumps(host_result,indent=1));sys.exit(0)
    adb=['adb','-s',args.serial]
    assert not run([*adb,'shell','pidof com.frostpunk.staticrecomp || true']).strip()
    native_override=run([*adb,'shell','getprop','debug.fp.native_frustum']).strip()
    assert native_override.startswith('0')==args.fallback, 'Wrong native frustum mode for this test'
    remote='/data/local/tmp/frostpunk-frustum-'+sha(arm)[:12]
    remote_data='/data/local/tmp/frostpunk-frustum-image-'+sha(mapped)[:12]
    run([*adb,'push',arm,remote]);run([*adb,'push',mapped,remote_data]);run([*adb,'shell','chmod','700',remote])
    try:
        device=json.loads(run([*adb,'shell',remote,remote_data]))
        benchmark=json.loads(run([*adb,'shell',remote,remote_data,'--benchmark'])) if not args.fallback else {'pairs':[]}
    finally:run([*adb,'shell','rm','-f',remote,remote_data])
    assert device['success'] and pins=={name:sha(Path(name)) for name in pins}
    medians={str(kind):{name:statistics.median(row[name] for row in benchmark['pairs'] if row['kind']==kind)
                        for name in ('original_ns','native_ns')} for kind in range(4)} if not args.fallback else {}
    report=dict(success=True,host=host_result,device=device,benchmark=benchmark,medians=medians,sources=pins,
                original_exe_sha256=sha(exe),mapped_image_sha256=sha(mapped),original_text_unchanged=True,original_object_sha256=sha(original),native_object_sha256=sha(objects[1]),arm_binary_sha256=sha(arm),native_override=native_override)
    (folder/('fallback-result.json' if args.fallback else 'result.json')).write_text(json.dumps(report,indent=1)+'\n')
    print(json.dumps(dict(host=host_result,device=device,medians=medians),indent=1),flush=True)


if __name__ == '__main__':
    main()
