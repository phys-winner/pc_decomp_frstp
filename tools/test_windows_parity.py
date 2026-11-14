"""Check parity and full flag callers against hardware PF and the preserved fallback."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT, windows_toolchain

CANDIDATE_SHA256='28bed585eb683f8ba789515efeb939d294c17260b4798a839d8ab84a7d16ed22'


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',action='store_true',help='Use the pinned isolated parity header before promotion.')
    args=parser.parse_args()
    header=ROOT/('build/windows-parity-candidate/after/cpu64.h' if args.candidate else 'native/runtime64/cpu64.h')
    if args.candidate:assert digest(header)==CANDIDATE_SHA256,'Isolated parity header changed'
    out=ROOT/'build/windows-parity-test'/('candidate' if args.candidate else 'production');out.mkdir(parents=True,exist_ok=True)
    include=out/'include';include.mkdir(exist_ok=True);overlay=include/'cpu64.h';overlay.write_bytes(header.read_bytes())
    config=json.loads((ROOT/'port.json').read_text());binaries,env=windows_toolchain()
    clang=Path(config['android_sdk'])/'ndk'/config['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    fixture=ROOT/'tests/windows_parity_fixture.c'
    inputs=[header,overlay,fixture,Path(__file__).resolve(),ROOT/'port.json',ROOT/'tools/leaf_probe.py',
        *[ROOT/'native/runtime64'/name for name in ['eh64.h','sse_scalar.h','bswap.h']]]
    hashes={str(p):digest(p) for p in inputs};commands=[]
    def run(command):
        commands.append(list(map(str,command)))
        p=subprocess.run(commands[-1],env=env,cwd=out,capture_output=True,timeout=30)
        assert p.returncode==0,p.stdout+p.stderr
        return p.stdout
    flags=['--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
        '-DFROSTPUNK_WINDOWS_HOST=1','-I'+str(include),'-I'+str(ROOT/'native/runtime64')]
    objects=[]
    for name,defines in [('fallback',['-DFP_PARITY_WRAPPER=1','-DFP_PARITY_FALLBACK=1']),
                         ('current',['-DFP_PARITY_WRAPPER=1']),('fixture',[])]:
        obj=out/(name+'.obj');objects.append(obj);run([clang,*flags,*defines,'-c',fixture,'-o',obj])
    exe=out/'fixture.exe';run([binaries/'link.exe','/nologo','/OUT:'+str(exe),*objects,'libcmt.lib','oldnames.lib','kernel32.lib'])
    result=json.loads(run([exe]))
    assert result==dict(success=True,native_PF_values=256,full_CPU_flag_cases=55680,helper_operations=12,
                        operand_widths=4,both_input_carries=True,LastError_MXCSR=True),result
    assert hashes=={p:digest(Path(p)) for p in hashes},'Fixture inputs changed'
    report=dict(success=True,result=result,selected_header=str(header),selected_header_sha256=digest(header),
        source_hashes=hashes,compiler=dict(path=str(clang),sha256=digest(clang)),commands=commands,
        executable_sha256=digest(exe),game_boot_proven=False)
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k not in ['source_hashes','commands']},indent=2))


if __name__=='__main__':main()
