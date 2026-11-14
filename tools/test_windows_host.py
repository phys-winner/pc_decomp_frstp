"""Exercise the real Windows host with synthetic NX callbacks and stub AOT tables."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT, windows_toolchain


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    config=json.loads((ROOT/'port.json').read_text())
    binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out=ROOT/'build/windows-host-selftest';out.mkdir(parents=True,exist_ok=True)
    overlay=out/'include/cpu64.h';overlay.parent.mkdir(exist_ok=True)
    cpu=ROOT/'native/runtime64/cpu64.h'
    text=cpu.read_text().replace('#if defined(__aarch64__)',
        '#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)',1)
    overlay.write_text(text,encoding='utf-8',newline='\n')
    sources=[ROOT/name for name in ['native/windows/host.c','native/windows/loader.c',
        'native/runtime64/eh64.c','tests/windows_host_stub.c','native/windows/bridge.asm',
        'native/windows/performance.c','native/windows/extra_aot.c','native/windows/input.c',
        'native/windows/native_validation.c','native/windows/shader_lookup.c','native/windows/simd_integer.c',
        'native/windows/callback_stacks.c','native/windows/simd_float.c','native/windows/simd_float_ops.asm']]
    headers=[*sorted((ROOT/'native/windows').glob('*.h')),*sorted((ROOT/'native/windows').glob('*.inc')),
        *sorted((ROOT/'native/runtime64').glob('*.h')),overlay]
    before={str(p):digest(p) for p in [*sources,*headers,Path(__file__).resolve()]}
    commands=[]
    def run(command):
        commands.append([str(x) for x in command])
        p=subprocess.run(commands[-1],env=env,cwd=out,capture_output=True,
            text=True,encoding='utf-8',errors='replace',timeout=60)
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        return p.stdout+p.stderr
    objects=[]
    includes=[overlay.parent,ROOT/'native/runtime64',ROOT/'native/windows',
        ROOT/'generated/frostpunk-x64',ROOT/'generated/supplemental']
    for source in sources:
        obj=out/(source.stem+'.obj');objects.append(obj)
        if source.suffix=='.asm':
            run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(obj),source])
        else:
            run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11',
                '-fno-strict-aliasing','-fwrapv','-ffp-contract=off','-DFROSTPUNK_WINDOWS_HOST=1',
                '-include',overlay,*['-I'+str(p) for p in includes],'-c',source,'-o',obj])
    exe=out/'windows-host-selftest.exe'
    run([binaries/'link.exe','/nologo','/OUT:'+str(exe),'/BASE:0x180000000',
        '/DYNAMICBASE:NO','/STACK:16777216','/SUBSYSTEM:CONSOLE','/ENTRY:wmainCRTStartup',
        *objects,'libcmt.lib','oldnames.lib','kernel32.lib','user32.lib','advapi32.lib',
        'shell32.lib','ole32.lib','winmm.lib','version.lib','dbghelp.lib','d3d11.lib','dxgi.lib'])
    output=run([exe,'--self-test'])
    assert 'WINDOWS_HOST_SELF_TEST passed ' in output,output
    assert 'GUEST_DEBUG_A host diagnostic ANSI\n' in output,output
    assert 'GUEST_DEBUG_W host diagnostic Unicode \u0416\n' in output,output
    assert 'GUEST_EXIT api=exit code=17 return=0xe5ef0010\n' in output,output
    assert before=={p:digest(Path(p)) for p in before}
    report=dict(success=True,commands=commands,sources=before,output=output,
        executable_sha256=digest(exe),game_boot_proven=False)
    (out/'self-test.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['commands','sources']},indent=2))


if __name__=='__main__':main()
