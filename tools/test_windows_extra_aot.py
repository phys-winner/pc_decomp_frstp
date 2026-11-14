"""Check the exact extra AOT thunk against native ADD flags and a captured tail."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT,windows_toolchain


def main():
    binaries,env=windows_toolchain();out=ROOT/'build/windows-extra-aot';out.mkdir(parents=True,exist_ok=True)
    sources=[ROOT/name for name in ['native/windows/extra_aot.c','native/windows/extra_aot.h',
        'tests/windows_extra_aot_fixture.c','tests/windows_extra_aot_fixture.asm',
        'native/windows/loader.c','native/windows/loader.h','native/runtime64/cpu64.h',
        'tools/test_windows_extra_aot.py','native/windows/extra_aot_bodies.inc',
        'tests/windows_extra_aot_cases.h','tools/generate_windows_extra_aot.py',
        'research/windows-extra-aot.json','native/windows/switch_aot_bodies.inc',
        'tests/windows_switch_aot_cases.h','tools/generate_windows_switch_aot.py',
        'native/windows/switch_extension_bodies.inc','tests/windows_switch_extension_cases.h',
        'tools/generate_windows_switch_extension.py','research/windows-switch-extension.json']]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources};commands=[]
    def run(args):
        commands.append([str(x) for x in args]);p=subprocess.run(commands[-1],env=env,cwd=out,
            capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
        if p.returncode:raise RuntimeError(p.stdout+p.stderr)
        return p.stdout
    asm=out/'oracle.obj';run([binaries/'ml64.exe','/nologo','/c','/Fo'+str(asm),sources[3]])
    exe=out/'extra-aot.exe'
    run([binaries/'cl.exe','/nologo','/O2','/std:c11','/W4','/I'+str(ROOT/'native/runtime64'),
        '/I'+str(ROOT/'native/windows'),sources[0],sources[2],sources[4],asm,'/Fe'+str(exe),
        '/link','/BASE:0x180000000','/DYNAMICBASE:NO'])
    result=json.loads(run([exe]));assert result['success']
    assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in hashes}
    report=dict(result=result,sources=hashes,commands=commands,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    (out/'self-test.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__=='__main__':main()
