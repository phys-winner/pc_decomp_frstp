"""Differentially test the complete observed six-entry scene decoder switch closure."""
import hashlib,json,subprocess
from pathlib import Path
from leaf_probe import ROOT,windows_toolchain
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    out=ROOT/'build/windows-scene-switch-test';out.mkdir(parents=True,exist_ok=True)
    names=['native/windows/extra_aot.c','native/windows/extra_aot.h','native/windows/extra_aot_bodies.inc','native/windows/switch_aot_bodies.inc',
        'native/windows/switch_extension_bodies.inc','native/windows/loader.c','native/windows/loader.h','native/runtime64/cpu64.h',
        'tests/windows_scene_switch_fixture.c','tests/windows_scene_switch_baseline.h','tests/windows_extra_aot_cases.h',
        'tests/windows_switch_aot_cases.h','tests/windows_switch_extension_cases.h','tools/generate_windows_extra_aot.py',
        'research/windows-extra-aot.json','generated/frostpunk-x64/recomp_funcs_0836.c','generated/frostpunk-x64/recomp_meta.json','tools/test_windows_scene_switch.py']
    hashes={name:sha(ROOT/name)for name in names};binaries,env=windows_toolchain();exe=out/'fixture.exe'
    command=[str(binaries/'cl.exe'),'/nologo','/O2','/std:c11','/W4','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'native/windows'),
        str(ROOT/'tests/windows_scene_switch_fixture.c'),str(ROOT/'native/windows/extra_aot.c'),str(ROOT/'native/windows/loader.c'),
        '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link','/BASE:0x180000000','/DYNAMICBASE:NO']
    p=subprocess.run(command,cwd=out,env=env,capture_output=True,text=True,timeout=60);assert p.returncode==0,p.stdout+p.stderr
    p=subprocess.run([str(exe)],cwd=out,env=env,capture_output=True,text=True,timeout=30);assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    result=json.loads(p.stdout);assert result['success']and result['full_CPU_stack_reader_object_cases']==8192 and result['original_byte_mutations_rejected']==1172
    assert hashes=={name:sha(ROOT/name)for name in names}
    report=dict(result=result,sources=hashes,executable_sha256=sha(exe),command=command,game_boot_proven=False)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
