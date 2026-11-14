"""Stress actual host callbacks and ExitThread while reclaiming only signaled owners."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT,windows_toolchain


def main():
    config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out=ROOT/'build/windows-callback-churn-test';out.mkdir(parents=True,exist_ok=True)
    overlay=out/'include/cpu64.h';overlay.parent.mkdir(exist_ok=True)
    overlay.write_text((ROOT/'native/runtime64/cpu64.h').read_text().replace('#if defined(__aarch64__)',
        '#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)',1),encoding='utf-8')
    sources=[ROOT/'tests/windows_callback_churn_fixture.c',ROOT/'tests/windows_callback_churn_fixture.asm',
        ROOT/'native/runtime64/eh64.c',*[p for p in sorted((ROOT/'native/windows').glob('*.c')) if p.name!='host.c'],
        *sorted((ROOT/'native/windows').glob('*.asm'))]
    watched=[*sources,ROOT/'native/windows/host.c',*sorted((ROOT/'native/windows').glob('*.h')),
        *sorted((ROOT/'native/windows').glob('*.inc')),*sorted((ROOT/'native/runtime64').glob('*.h')),overlay,Path(__file__).resolve()]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in watched}
    def run(command):
        result=subprocess.run(list(map(str,command)),env=env,cwd=out,capture_output=True,text=True,
            encoding='utf-8',errors='replace',timeout=60)
        assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
        return result.stdout,result.stderr
    includes=[overlay.parent,ROOT/'native/runtime64',ROOT/'native/windows',ROOT/'generated/frostpunk-x64',ROOT/'generated/supplemental']
    objects=[]
    for source in sources:
        obj=out/(source.stem+source.suffix+'.obj');objects.append(obj)
        if source.suffix=='.asm':run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(obj),source])
        else:run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv',
            '-ffp-contract=off','-DFROSTPUNK_WINDOWS_HOST=1','-include',overlay,*['-I'+str(p) for p in includes],'-c',source,'-o',obj])
    exe=out/'fixture.exe'
    run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000','/DYNAMICBASE:NO','/STACK:16777216',
        '/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',*objects,'libcmt.lib','oldnames.lib','kernel32.lib','user32.lib',
        'advapi32.lib','shell32.lib','ole32.lib','winmm.lib','version.lib','dbghelp.lib','d3d11.lib','dxgi.lib'])
    stdout,stderr=run([exe]);proof=json.loads(stdout);assert proof['success']
    assert proof['native_reap_per_thread_4096'] and proof['native_reap_unsigned_wrap']
    assert 'CALLBACK_STACK_ALLOCATION_FAILED error='+str(proof['forced_allocation_error']) in stderr
    assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in hashes}
    proof.update(source_sha256=hashes,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),diagnostic=stderr,game_boot_proven=False)
    (out/'result.json').write_text(json.dumps(proof,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in proof.items() if k!='source_sha256'},indent=2))


if __name__=='__main__':main()
