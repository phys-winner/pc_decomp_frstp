"""Recompile accepted math fixtures against the frozen angle extension in a new output directory."""
import hashlib
import json
from pathlib import Path
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/android-angles-regressions'
def sha(path):
    with Path(path).open('rb')as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def main():
    OUT.mkdir(parents=True,exist_ok=True);bins,env=windows_toolchain();commands=[]
    source=ROOT/'android/host/win64_math.c';wide=ROOT/'android/host/win64_wide_stdio.c'
    expected={str(source.relative_to(ROOT)):'71b101ca5450f884ff8dbb9a490924968dd5dda7b6da5fe4cea6a530cb6e06c0',str(wide.relative_to(ROOT)):'891c975a134b1861fad8e665e468d3b84a4e63b27b0d4c8bb9760f456da5a892'}
    if any(sha(ROOT/p)!=h for p,h in expected.items()):raise ValueError('Frozen native source drift')
    jobs=[('pow','tests/win64_pow_fixture.c','android-pow-zero-test',[('--exact-zero','android-pow-zero-test'),('','android-pow-test')]),
          ('codec','tests/win64_codec_math_fixture.c','android-codec-math-test',[('','android-codec-math-test')]),
          ('ldexp','tests/win64_ldexp_fixture.c','android-ldexp-test',[('','android-ldexp-test')]),
          ('fmodf','tests/win64_fmodf_fixture.c','android-fmodf-test',[('','android-fmodf-test')]),
          ('expf','tests/win64_expf_fixture.c','android-expf-test',[('','android-expf-test')])]
    protected=set()
    for _,fixture,include,runs in jobs:
        protected.add(ROOT/fixture)
        for p in (ROOT/'build'/include).glob('*.inc'):protected.add(p)
        for _,directory in runs:
            old=ROOT/'build'/directory
            for p in old.iterdir():
                if p.is_file()and(p.suffix in ['.exe','.json']or p.name=='cases.bin'):protected.add(p)
    pins={str(p.relative_to(ROOT)):sha(p)for p in sorted(protected)};results=[]
    def run(args):
        command=list(map(str,args));commands.append(command);r=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if r.returncode:raise RuntimeError(subprocess.list2cmdline(command)+'\n'+r.stdout[-5000:]+r.stderr[-5000:])
        return r
    for name,fixture,include,runs in jobs:
        exe=OUT/(name+'-regression.exe')
        run([bins/'cl.exe','/nologo','/O2','/fp:strict','/std:c11','/I'+str(ROOT/'native/runtime64'),'/I'+str(ROOT/'android/host'),'/I'+str(ROOT/'build'/include),ROOT/fixture,source,'/Fo'+str(OUT)+'/', '/Fe'+str(exe)])
        for option,directory in runs:
            dataset=ROOT/'build'/directory/'cases.bin';r=run([exe,*([option]if option else[]),dataset]);result=json.loads(r.stdout.strip().splitlines()[-1])
            if not result.get('success'):raise ValueError('Accepted math regression failed')
            log=OUT/(name+'-'+directory+'.log');log.write_text(r.stdout+r.stderr)
            results.append({'name':name,'source_fixture':fixture,'dataset':str(dataset.relative_to(ROOT)),'dataset_sha256':sha(dataset),'result':result,'log':str(log.relative_to(ROOT)),'binary_sha256':sha(exe)})
    if pins!={p:sha(ROOT/p)for p in pins}or any(sha(ROOT/p)!=h for p,h in expected.items()):raise ValueError('Historical artifacts/native sources changed')
    report={'success':True,'frozen_native_sources':expected,'historical_files_unchanged':pins,'regressions':results,'commands':commands,'producer_sha256':sha(Path(__file__)),'device_executed':False,'production_modified':False}
    path=OUT/'result.json';path.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'success':True,'regressions':[{'name':r['name'],'dataset':r['dataset'],'result':r['result']}for r in results],'receipt':str(path),'receipt_sha256':sha(path)},indent=2))

if __name__=='__main__':main()
