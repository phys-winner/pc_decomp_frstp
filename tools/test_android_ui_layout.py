"""Run original UILayout flow/measure bodies with the mobile column policy, without ADB."""
import hashlib,json,re,subprocess
from pathlib import Path
import pefile
from fastbuild import ROOT,CLANG
from leaf_probe import windows_toolchain
from test_android_render_scale import function

out=ROOT/'build/mobile-20261108/input-photos/ui-layout-check';out.mkdir(parents=True,exist_ok=True)
source=ROOT/'generated/frostpunk-x64/recomp_funcs_0837.c';text=source.read_text()
available={int(x,16):x for x in re.findall(r'void L_000([0-9A-F]+)\(CPU \*c\)',text) if 0x141096F50<=int(x,16)<0x141098580}
extra_text=(ROOT/'generated/frostpunk-x64/recomp_funcs_0823.c').read_text()
anchor_text=(ROOT/'generated/frostpunk-x64/recomp_funcs_0822.c').read_text()
for group,start,end in ((anchor_text,0x140F964F0,0x140F967B0),(extra_text,0x140F99F00,0x140F9A100)):
    available.update({int(x,16):x for x in re.findall(r'void L_000([0-9A-F]+)\(CPU \*c\)',group) if start<=int(x,16)<end})
pending=[0x141097460,0x140F964F0];bodies={}
while pending:
    ea=pending.pop()
    if ea in bodies:continue
    body=function(anchor_text if ea<0x140F97000 else extra_text if ea<0x141000000 else text,available[ea]);bodies[ea]=body
    for target in re.findall(r'dispatch\(c, 0x([0-9A-Fa-f]+)ull\)',body):
        dep=int(target,16)
        if dep in available and dep not in bodies:pending.append(dep)
original='\n'.join('void L_000'+available[ea]+'(CPU*);' for ea in sorted(bodies))+'\n'+'\n'.join(bodies.values())
(out/'ui_layout_original.inc').write_text(original)
(out/'ui_layout_dispatch.inc').write_text('\n'.join('case UINT64_C(0x%X):L_000%s(c);return;'%(ea,available[ea]) for ea in sorted(bodies)))
pe=pefile.PE(str(ROOT/'build/mobile-20261108/input-photos/analysis/Frostpunk.exe'),fast_load=True)
constants=[]
addresses=set(int(x,16) for x in re.findall(r'GVA\(0x([0-9A-Fa-f]+)\)',original))
addresses.add(0x14109742C) # Original ten-entry alignment switch jump table.
for ea in sorted(addresses):
    if 0x140000000<ea<0x142B90000:
        raw=pe.get_data(ea-0x140000000,40 if ea==0x14109742C else 16)
        if not raw:continue
        constants.append('{const unsigned char value[]={'+','.join(str(x) for x in raw)+'};memcpy((void*)(uintptr_t)GVA(UINT64_C(0x%X)),value,sizeof(value));}'%ea)
(out/'ui_layout_constants.inc').write_text('\n'.join(constants))
flags=['-I'+str(ROOT/'native/runtime64'),'-I'+str(ROOT/'android/host'),'-I'+str(out)]
fixture=ROOT/'tests/android_ui_layout_fixture.c';arm=out/'ui-layout-arm64'
subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-O2','-std=c11','-Wno-parentheses-equality','-Wl,-z,max-page-size=16384',*flags,str(fixture),'-lm','-o',str(arm)],check=True)
bins,env=windows_toolchain();host=out/'ui-layout-host.exe'
run=subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11','/W3',*['/I'+f[2:] for f in flags],str(fixture),'/Fo'+str(out/'ui-layout-host.obj'),'/Fe'+str(host)],env=env,capture_output=True,text=True)
assert not run.returncode,run.stdout+run.stderr
run=subprocess.run([str(host)],capture_output=True,text=True);assert not run.returncode,f'exit={run.returncode:#x}\n'+run.stdout+run.stderr
result=json.loads(run.stdout);assert result['success']
result.update(original_functions=[available[ea] for ea in sorted(bodies)],source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),helper_sha256=hashlib.sha256((ROOT/'android/host/mobile_selection_columns.inc').read_bytes()).hexdigest(),arm_compiled=True,device_executed=False)
result.update(native_anchor_stretch_reproduced=True,native_anchor_and_position_setters=True,header_anchors_detached_and_restored=True,body_anchors_detached_and_restored=True,parent_width_331_to_673_to_331=True)
result['original_source_sha256']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (source,ROOT/'generated/frostpunk-x64/recomp_funcs_0822.c',ROOT/'generated/frostpunk-x64/recomp_funcs_0823.c')}
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
