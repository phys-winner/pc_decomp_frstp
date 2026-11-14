"""Run the production IME queue with the real translated original editor storage handlers."""
import hashlib
import json
import re
import subprocess
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain
from test_android_render_scale import function

out=ROOT/'build/audit-20261108/text-editor-check';out.mkdir(parents=True,exist_ok=True)
(out/'result.json').write_text(json.dumps(dict(success=False,status='not_completed'))+'\n')
source=ROOT/'generated/frostpunk-x64/recomp_funcs_0825.c';text=source.read_text()
names=['140FC29C0','140FC2E30','140FC30A0','140FC3A50','140FC3C70']
names+=re.findall(r'void L_000(140FC2[A-B][0-9A-F]{2})\(CPU \*c\)',text)
names=[x for x in names if int(x,16)<0x140FC2BA0 or x in ['140FC2E30','140FC30A0','140FC3A50','140FC3C70']]
names=sorted(set(names))
functions='\n'.join(function(text,name) for name in names)
(out/'text_editor_original.inc').write_text('\n'.join('void L_000'+name+'(CPU*);' for name in names)+'\n'+functions)
(out/'text_editor_dispatch.inc').write_text('\n'.join('case UINT64_C(0x'+name+'):L_000'+name+'(c);return;' for name in names))
queue=ROOT/'android/host/text_input.c'
production=queue.read_text();assert production.count('#include <pthread.h>')==1
(out/'text_input_production.inc').write_text(production.replace('#include <pthread.h>','/* The single-thread oracle supplies its mutex boundary. */'))
flags=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(out)]
arm=out/'text-editor-arm64'
subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=c11','-Wl,-z,max-page-size=16384',*flags,
                '-Wno-parentheses-equality',str(ROOT/'tests/android_text_editor_fixture.c'),'-lm','-o',str(arm)],check=True)
bins,env=windows_toolchain();host=out/'text-editor-host.exe'
run=subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11','/W3',*['/I'+f[2:] for f in flags],
                    str(ROOT/'tests/android_text_editor_fixture.c'),'/Fo'+str(out/'text-editor-host.obj'),'/Fe'+str(host)],env=env,capture_output=True,text=True)
assert not run.returncode,run.stdout+run.stderr
run=subprocess.run([str(host)],capture_output=True,text=True);assert not run.returncode,run.stdout+run.stderr
result=json.loads(run.stdout);assert result['success']
assert queue.read_text()==production,'Production text queue changed during this fixture run'
result.update(original_functions=names,original_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
              queue_source_sha256=hashlib.sha256(queue.read_bytes()).hexdigest(),arm_compiled=True,device_executed=False)
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
