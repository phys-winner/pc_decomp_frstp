"""Verify opt-in owned hardware D3D11 capture and always-on Present sampling."""
import json
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import sys
from leaf_probe import ROOT, windows_toolchain


def main():
    config=json.loads((ROOT/'port.json').read_text())
    binaries,env=windows_toolchain()
    debug='--debug' in sys.argv[1:]
    if debug:env['FROSTPUNK_TEST_D3D11_DEBUG']='1'
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    out=ROOT/'build/windows-performance-selftest';out.mkdir(parents=True,exist_ok=True)
    sources=[ROOT/'native/windows/performance.c',ROOT/'native/windows/performance.h',
        ROOT/'tests/windows_performance_fixture.c',Path(__file__).resolve()]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    objects=[]
    for name in ['native/windows/performance.c','tests/windows_performance_fixture.c']:
        source=ROOT/name;obj=out/(source.stem+'.obj');objects.append(obj)
        subprocess.run([str(clang),'--target=x86_64-pc-windows-msvc','-O2','-std=c11',
            '-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'native/windows'),
            '-c',str(source),'-o',str(obj)],env=env,check=True,cwd=ROOT)
    exe=out/'fixture.exe'
    subprocess.run([str(binaries/'link.exe'),'/nologo','/OUT:'+str(exe),*map(str,objects),
        'libcmt.lib','oldnames.lib','kernel32.lib','user32.lib','d3d11.lib','dxgi.lib'],env=env,check=True)
    runs=[]
    for setting in (None,'0','01','10','true','1'):
        with tempfile.TemporaryDirectory(prefix='frostpunk-own-backbuffer-') as folder:
            run_env=env.copy();run_env['FROSTPUNK_CAPTURE_DIRECTORY']=folder
            run_env.pop('FROSTPUNK_CAPTURE_BACKBUFFER',None)
            if setting is not None:run_env['FROSTPUNK_CAPTURE_BACKBUFFER']=setting
            process=subprocess.run([str(exe)],env=run_env,capture_output=True,text=True,timeout=30)
            assert process.returncode==0,(setting,process.returncode,process.stdout,process.stderr)
            images=list(Path(folder).glob('present-*.bmp'))
            enabled=setting=='1'
            assert len(images)==int(enabled),(setting,images)
            assert process.stderr.count('NATIVE_FIRST_SUCCESSFUL_PRESENT ')==1,process.stderr
            assert process.stderr.count('PRESENT_SAMPLE ')==1,process.stderr
            assert 'PRESENT_SAMPLE total=2 ' in process.stderr,process.stderr
            if enabled:
                image=images[0].read_bytes()
                assert image[:2]==b'BM'
                offset,=struct.unpack_from('<I',image,10)
                width,height=struct.unpack_from('<ii',image,18)
                assert (width,height)==(73,-41),(width,height)
                assert image[offset:]==bytes([0,0,255,255])*73*41
                assert 'all_black=0' in process.stderr and 'before_Present=1' in process.stderr
                if debug:
                    assert 'NATIVE_D3D11_INFO_QUEUE available=1' in process.stderr,process.stderr
                    assert 'NATIVE_D3D11_DEBUG severity=1' in process.stderr,process.stderr
                    assert 'Frostpunk owned debug queue fixture' in process.stderr,process.stderr
            else:
                assert 'NATIVE_BACKBUFFER_CAPTURE ' not in process.stderr,process.stderr
            runs.append(dict(capture_environment=setting,captures=len(images),
                successful_presents=2,output=process.stdout+process.stderr))
    report=dict(success=True,hardware_D3D11=True,exact_pixels=73*41,
        Present_TEST_ignored=True,failed_Present_ignored=True,capture_interval_enforced=True,
        capture_disabled_by_default=True,capture_requires_exact_1=True,
        Present_sampling_without_capture=True,full_CPU_preserved=True,
        desktop_read=False,game_boot_proven=False,output=runs[-1]['output'],runs=runs,
        debug_layer_validation_errors_observed=debug,
        source_sha256=hashes,executable_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    assert hashes=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    (out/'self-test.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
