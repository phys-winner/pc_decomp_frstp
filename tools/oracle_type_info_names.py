"""Record native VCRUNTIME names and cache/list ABI for every original call-site type."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from leaf_probe import ROOT,windows_toolchain

OUT=ROOT/'build/audit-20261108/type-info-name'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def main():
    import pefile
    OUT.mkdir(parents=True,exist_ok=True)
    cfg=json.loads((ROOT/'port.json').read_text());original=Path(cfg['game_directory'])/'Frostpunk.exe'
    assert sha(original)==cfg['original_exe_sha256']
    pe=pefile.PE(str(original),fast_load=True);data=pe.get_memory_mapped_image();base=pe.OPTIONAL_HEADER.ImageBase
    rows=[]
    for va in range(0x1428b18b8,0x1428b1c79,0x60):
        at=va-base;assert data[at-8:at]==struct.pack('<Q',0x14217c8d0) and data[at:at+8]==bytes(8)
        name=data[at+8:at+520].split(b'\0',1)[0].decode('ascii')
        assert name.startswith('.?AU?$DrawQueue@U') and name.endswith('@ShapeRenderNode@@@ShapeRenderNode@@')
        rows.append((va-base,name))
    assert len(rows)==11
    (OUT/'type_info_input.inc').write_text('static const struct {uint32_t rva;const char*decorated;} input_cases[]={\n'+''.join('{0x%x,%s},\n'%(rva,json.dumps(name)) for rva,name in rows)+'};\n')
    bins,env=windows_toolchain();oracle=OUT/'type-info-name-oracle.exe'
    result=subprocess.run([str(bins/'cl.exe'),'/nologo','/O2','/std:c11','/I'+str(OUT),str(ROOT/'tests/type_info_name_oracle.c'),
                           '/Fo'+str(OUT/'type-info-name-oracle.obj'),'/Fe'+str(oracle)],capture_output=True,text=True,env=env)
    assert not result.returncode,result.stdout+result.stderr
    run=subprocess.run([str(oracle),str(OUT/'cases.bin')],capture_output=True,text=True,check=True)
    proof=json.loads(run.stdout);assert proof['success'] and proof['cases']==11
    packed=(OUT/'cases.bin').read_bytes();count,=struct.unpack_from('<I',packed);at=4;result_rows=[]
    for _ in range(count):
        rva,decorated_count,name_count,root_low=struct.unpack_from('<IIIQ',packed,at);at+=20
        decorated=packed[at:at+decorated_count];at+=decorated_count
        decoded=packed[at:at+name_count];at+=name_count
        assert decorated[-1:]==decoded[-1:]==b'\0'
        result_rows.append(dict(rva=rva,decorated=decorated[:-1].decode('ascii'),name=decoded[:-1].decode('ascii'),root_low=root_low))
    assert at==len(packed)
    include='/* Native System32 VCRUNTIME oracle; exact original diagnostic-call closure. */\n'
    include+='static const struct {const char *decorated,*name;} rtti_diagnostic_names[]={\n'
    include+=''.join('    {%s,%s},\n'%(json.dumps(row['decorated']),json.dumps(row['name'])) for row in result_rows)+'};\n'
    target=ROOT/'android/host/win64_rtti_names.inc';target.write_text(include,encoding='utf-8',newline='\n')
    msvc=Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231')
    sources=[msvc/'crt/src/vcruntime/std_type_info.cpp',msvc/'include/vcruntime_typeinfo.h',Path('C:/Windows/System32/vcruntime140.dll'),Path('C:/Windows/System32/ntdll.dll')]
    report=dict(success=True,original_exe_sha256=cfg['original_exe_sha256'],oracle=proof,cases=result_rows,
                generated_include_sha256=sha(target),dataset_sha256=sha(OUT/'cases.bin'),native_inputs={str(p):sha(p) for p in sources},
                original_callers=['0x141035430','0x141037810','0x141037A00'],fault_return='0x141037C38',
                scope='All11 actual uncached type-name inputs; names and SList/cache ABI measured natively, no original instructions executed',device_actions=False)
    (OUT/'oracle.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(dict(success=True,cases=11,include_sha256=sha(target),native=proof)))
if __name__=='__main__':main()
