"""Run the production persistent profile migration against disposable host folders."""
import json
from pathlib import Path
import subprocess
from fastbuild import ROOT, JAVA

out=ROOT/'build/audit-20261108/profile-check'
out.mkdir(parents=True,exist_ok=True)
subprocess.run([str(JAVA.parent/'javac.exe'),'--release','8','-encoding','UTF-8','-d',str(out),
                str(ROOT/'android/app/PortProfile.java'),str(ROOT/'tests/PortProfileCheck.java')],check=True)
run=subprocess.run([str(JAVA),'-cp',str(out),'com.frostpunk.staticrecomp.probe.PortProfileCheck'],capture_output=True,text=True,check=True)
result=json.loads(run.stdout);assert result['success']
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
