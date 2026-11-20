"""Check original localization-call ABI and all gesture-help language resources; no device."""
import hashlib
import json
from pathlib import Path
import subprocess
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain
from port_settings_localization import LANGUAGES,translations,TABLE_LANGUAGES,parse_table
from prepare_raw_archives import entries

out=ROOT/'build/audit-20261108/help-check';out.mkdir(parents=True,exist_ok=True)
flags=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host')]
arm=out/'port-help-arm64'
subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=c11','-Wl,-z,max-page-size=16384',*flags,
                str(ROOT/'tests/port_help_fixture.c'),'-o',str(arm)],check=True)
bins,env=windows_toolchain();host=out/'port-help-host.exe'
run=subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11','/W3',*['/I'+f[2:] for f in flags],
                    str(ROOT/'tests/port_help_fixture.c'),'/Fo'+str(out/'port-help-host.obj'),'/Fe'+str(host)],env=env,capture_output=True,text=True)
assert not run.returncode,run.stdout+run.stderr
run=subprocess.run([str(host)],capture_output=True,text=True,check=True)
result=json.loads(run.stdout);assert result['success']
overlay_arm=out/'mobile-overlay-text-arm64';overlay_host=out/'mobile-overlay-text-host.exe'
subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=c11','-Wl,-z,max-page-size=16384',*flags,
                str(ROOT/'tests/mobile_overlay_text_fixture.c'),'-o',str(overlay_arm)],check=True)
run=subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11','/W3',*['/I'+f[2:] for f in flags],
                    str(ROOT/'tests/mobile_overlay_text_fixture.c'),'/Fo'+str(out/'mobile-overlay-text-host.obj'),'/Fe'+str(overlay_host)],env=env,capture_output=True,text=True)
assert not run.returncode,run.stdout+run.stderr
overlay=json.loads(subprocess.check_output([str(overlay_host)],text=True));assert overlay['success']
result['overlay']=overlay
data=translations();assert set(data)==LANGUAGES
assert all(all(0<len(row['UI/Android/'+key].encode('utf-16-le'))//2<8192 for key in ['GestureHelp','GestureHelpText','GestureHelpClose']) for row in data.values())
archive=ROOT/'build/raw-archives/localizations';raw=(archive/'localizations.dat').read_bytes();seen=set()
for key,size,_,offset,_ in entries((archive/'localizations.idx').read_bytes(),len(raw)):
    if key in TABLE_LANGUAGES:
        label=parse_table(raw[offset:offset+size])['UI/Menu/Buttons/Loading']
        assert 0<len(label.encode('utf-16-le'))//2<8192
        seen.add(key)
assert seen==set(TABLE_LANGUAGES)
result.update(languages=sorted(data),arm_compiled=True,arm_sha256=hashlib.sha256(arm.read_bytes()).hexdigest(),device_executed=False)
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
