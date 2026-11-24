"""Run bounded original-leaf/CPU adapters and actual bridge extent math; compile ARM CLI."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
from leaf_probe import ROOT,windows_toolchain
import generate_android_texture_format_sites as generator
from dxvk_astc_overlay import FORMATS


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def extract(text,signature):
    first=text.index(signature);brace=text.index('{',first);depth=1
    for last in range(brace+1,len(text)):
        depth+=(text[last]=='{')-(text[last]=='}')
        if not depth:return text[first:last+1]
    raise ValueError('Unclosed bridge helper')


def main():
    out=ROOT/'build/android-texture-formats';manifest=json.loads((out/'adapter-manifest.json').read_text())
    assert manifest['success']and manifest['producer']['sha256']==sha(ROOT/manifest['producer']['path'])
    for name,expected in manifest['outputs'].items():assert sha(ROOT/name)==expected
    header=(ROOT/'android/host/texture_formats.h').read_text()
    constants={k:int(v,16)for k,v in re.findall(r'#define (FP_ASTC\w+) UINT32_C\((0x[0-9a-f]+)\)',header)}
    for row in FORMATS:
        prefix='FP_ASTC'+str(row['block'])
        assert constants[prefix+'_D3DFMT']==row['D3DFMT']
        for suffix,key in [('_DXGI_TYPELESS','DXGI_typeless'),('_DXGI_UNORM','DXGI_unorm'),('_DXGI_SRGB','DXGI_srgb')]:assert constants[prefix+suffix]==row[key]
    config=json.loads((ROOT/'port.json').read_text());bins,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    files=['android/host/texture_formats.c','android/host/texture_formats.h','android/host/texture_format_sites.inc',
           'tests/android_texture_formats_fixture.c','tools/generate_android_texture_format_sites.py',
           'tools/test_android_texture_formats.py','android/dxvk/bridge.cpp','tools/dxvk_astc_overlay.py',
           'native/runtime64/cpu64.h','build/android-texture-formats/canonical_bodies.inc']
    inputs={name:sha(ROOT/name)for name in files};commands=[]
    def run(args,success=True):
        args=list(map(str,args));commands.append(args)
        p=subprocess.run(args,cwd=out,env=env,capture_output=True,text=True,timeout=120)
        if success and p.returncode:raise RuntimeError(p.stdout+p.stderr)
        return p
    sources=[ROOT/'android/host/texture_formats.c',ROOT/'tests/android_texture_formats_fixture.c']
    host=out/'texture-formats.exe'
    run([bins/'cl.exe','/nologo','/O2','/std:c11','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),
         '/I'+str(out),*sources,'/Fe'+str(host),'/link','/BASE:0x180000000','/DYNAMICBASE:NO'])
    dataset=out/'native-oracle.bin';result=json.loads(run([host,'--oracle',dataset]).stdout)
    assert result['success']and result['full_CPU_calls']==4608 and result['opcode_mutations']==1292
    replay=json.loads(run([host,'--dataset',dataset]).stdout);assert replay==result
    negative=['width0','height0','rowoverflow','sizeoverflow','badstack','stackoverflow','nullsize','nullrow','readonly','srgb2','mapstack','etc2dxvk']
    for name in negative:
        p=run([host,'--negative',name],False)
        assert p.returncode==73 and 'STOP 'in p.stderr,(name,p.returncode,p.stdout,p.stderr)
    arm=out/'texture-formats-arm64'
    run([clang,'--target=aarch64-linux-android30','-O2','-std=c11','-fPIE','-pie','-fno-strict-aliasing','-fwrapv',
         '-ffp-contract=off','-fno-math-errno','-Wl,-z,max-page-size=16384',
         '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(out),*sources,'-lm','-o',arm])
    # Extract the exact production Extent/extent/pitched_bytes helpers, rather
    # than mirroring their implementation in the test.
    bridge=(ROOT/'android/dxvk/bridge.cpp').read_text()
    extent_struct=bridge[bridge.index('struct Extent {'):bridge.index('\nExtent extent(')]
    preamble='#include <stdint.h>\n#include <stddef.h>\n#include <cstdio>\n#include <cstdlib>\n#include <cassert>\nusing DXGI_FORMAT=uint32_t;\n'
    preamble+='\n'.join(line for line in header.splitlines()if line.startswith('#define FP_ASTC'))+'\n'
    preamble+='[[noreturn]] void trap(const char* why){fprintf(stderr,"STOP extent %s\\n",why);_Exit(73);}\n'
    bridge_fixture=out/'bridge-extent.cpp'
    source=preamble+extract(bridge,'size_t multiply(')+'\n'+extent_struct+'\n'+extract(bridge,'Extent extent(')+'\n'+extract(bridge,'size_t pitched_bytes(')
    source+=r'''
int main(int argc,char**argv){
  if(argc==2){
    auto e=extent(FP_ASTC6_DXGI_UNORM,13,7,1);
    if(argv[1][0]=='u')extent(0x7f006603,1,1,1);
    else if(argv[1][0]=='p')pitched_bytes(e,47,0);
    else if(argv[1][0]=='o')pitched_bytes(e,SIZE_MAX,0);
    else if(argv[1][0]=='s')pitched_bytes(extent(FP_ASTC4_DXGI_UNORM,13,7,2),64,1);
    else abort();abort();
  }
  unsigned cases=0;
  uint32_t formats[]={FP_ASTC4_DXGI_TYPELESS,FP_ASTC4_DXGI_UNORM,FP_ASTC4_DXGI_SRGB,FP_ASTC6_DXGI_TYPELESS,FP_ASTC6_DXGI_UNORM,FP_ASTC6_DXGI_SRGB};
  for(unsigned f=0;f<6;f++)for(unsigned w=1;w<=513;w++)for(unsigned h:{1u,2u,4u,6u,7u,13u,256u,513u}){
    unsigned block=f<3?4:6;auto e=extent(formats[f],w,h,1);size_t row=(size_t(w)+block-1)/block*16,rows=(h+block-1)/block;
    assert(e.row_bytes==row&&e.rows==rows&&e.block==block&&e.block_bytes==16);
    assert(pitched_bytes(e,row,0)==row*rows);assert(pitched_bytes(e,row+16,0)==(row+16)*(rows-1)+row);cases++;
  }
  assert(extent(77,5,7,1).row_bytes==32&&extent(77,5,7,1).rows==2);
  assert(extent(87,5,7,1).row_bytes==20&&extent(87,5,7,1).rows==7);
  auto large=extent(FP_ASTC6_DXGI_UNORM,UINT32_MAX,UINT32_MAX,1);
  assert(large.row_bytes==UINT64_C(11453246128)&&large.rows==715827883);
  printf("{\"success\":true,\"actual_extent_cases\":%u,\"BC_BGRA_unchanged\":true}\n",cases);
}
'''
    # initializer-list range syntax requires its standard declaration.
    bridge_fixture.write_text('#include <initializer_list>\n'+source)
    extent_host=out/'bridge-extent.exe'
    run([bins/'cl.exe','/nologo','/O2','/std:c++17',bridge_fixture,'/Fe'+str(extent_host)])
    extent_result=json.loads(run([extent_host]).stdout)
    for name in ['unknown','pitch','overflow','slice']:
        p=run([extent_host,name],False);assert p.returncode==73 and 'STOP extent 'in p.stderr
    arm_cpp=clang.with_name('clang++.exe')
    run([arm_cpp,'--target=aarch64-linux-android30','-std=c++17','-fsyntax-only',bridge_fixture])
    assert inputs=={name:sha(ROOT/name)for name in inputs}
    report=dict(success=True,source_hashes=inputs,host_native_oracle=result,host_replay=replay,negative_cases=negative,
        bridge_extent=extent_result,bridge_negative_cases=4,
        arm=dict(binary=str(arm.relative_to(ROOT)),sha256=sha(arm),compiled=True,executed=False,
                 arguments=['--dataset','native-oracle.bin']),
        dataset=dict(path=str(dataset.relative_to(ROOT)),sha256=sha(dataset),bytes=dataset.stat().st_size),
        original_exe_started=False,original_integer_leaves_executed_on_native_host=True,canonical_modified=False,
        producer_SHA256=sha(__file__),commands=commands)
    (out/'test-result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(success=True,host=result,bridge=extent_result,negative_cases=len(negative)+4,arm=report['arm'],dataset=report['dataset']),indent=2))


if __name__=='__main__':main()
