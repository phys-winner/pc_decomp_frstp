"""Check the restored switch entries against the existing exact generated blocks."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT,windows_toolchain


def main():
    binaries,env=windows_toolchain();out=ROOT/'build/windows-switch-aot-test';out.mkdir(parents=True,exist_ok=True)
    original=ROOT/'generated/frostpunk-x64/recomp_funcs_0813.c';text=original.read_text()
    bodies=[]
    for va,end in [('140EC9F6D','140ECA00A'),('140ECA00A',None)]:
        start=text.index('    SET64(c->rax, rd64((c->rdi + 0x330ull)));',text.index('/* 000'+va+':')-90)
        if end:stop=text.rindex('    SET64(c->rax, rd64((c->rdi + 0x330ull)));',start,text.index('/* 000'+end+':'))
        else:stop=text.index('L_000140ECA026:',start)
        body=text[start:stop]
        bodies.append('static void baseline_'+va+'(CPU *c) {\n    c->rip=0x'+va+'ull;\n'+body+
            '\nL_000140ECA026:\n    dispatch(c,0x140ECA026ull);\n}\n')
    baseline=out/'switch_original_baseline.h';baseline.write_text('\n'.join(bodies),encoding='utf-8')
    sources=[ROOT/name for name in ['tests/windows_switch_aot_fixture.c','native/windows/extra_aot.c',
        'native/windows/loader.c','native/windows/extra_aot_bodies.inc','native/windows/switch_aot_bodies.inc',
        'tests/windows_extra_aot_cases.h','tests/windows_switch_aot_cases.h','native/windows/extra_aot.h',
        'native/windows/loader.h','native/runtime64/cpu64.h','native/windows/switch_extension_bodies.inc',
        'tests/windows_switch_extension_cases.h']]+[original,baseline,Path(__file__).resolve()]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    exe=out/'fixture.exe'
    result=subprocess.run([str(binaries/'cl.exe'),'/nologo','/O2','/std:c11','/W4',
        '/I'+str(out),'/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'native/windows'),
        str(sources[0]),str(sources[1]),str(sources[2]),'/Fo'+str(out)+'/', '/Fe'+str(exe),
        '/link','/BASE:0x180000000','/DYNAMICBASE:NO'],env=env,capture_output=True,text=True,timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=30)
    assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
    proof=json.loads(result.stdout);assert proof['success']
    assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in hashes}
    proof.update(sources=hashes,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),game_boot_proven=False)
    (out/'result.json').write_text(json.dumps(proof,indent=2)+'\n')
    print(json.dumps({k:v for k,v in proof.items() if k!='sources'},indent=2))


if __name__=='__main__':main()
