"""Wrap original AOT entries without regenerating or discarding their EH repairs."""
import hashlib
import json
import subprocess
from pathlib import Path
import pefile
from lift64_checked import CheckedLifter

from fastbuild import BIN, CLANG, ROOT, FAST


def main():
    out=FAST/'mobile-objects';out.mkdir(exist_ok=True)
    inputs=FAST/'runtime-inputs.json';data=json.loads(inputs.read_text())
    hooks={'0859':['L_000141250610','L_00014124C7B0'],'0813':['L_000140EC6200','L_000140EC7240'],
           '0808':['L_000140E74B30'],
           '0803':['L_000140E26E40','L_000140E26F30','L_000140E26F90'],'0823':['L_000140F99F00','L_000140F9D240'],
           '0958':['L_000141A04EB0'],'0957':['L_0001419EF190'],'0807':[],
           '0793':['L_000140D8DB10','L_000140D8E470'],
           '0961':['L_000141A5EE10','L_000141A62E90']}
    for unit,names in hooks.items():
        original=ROOT/f'build/arm64/eh-overlay-objects/recomp_funcs_{unit}.o'
        target=out/original.name
        if unit=='0807':
            from build_port_settings_unit import FLAGS
            text=(ROOT/'build/android-resolution-config-integration/recomp_funcs_0807.c').read_text()
            baseline=out/'defaults-baseline';baseline.mkdir(exist_ok=True)
            source=baseline/'recomp_funcs_0807.c';source.write_text(text,encoding='utf-8',newline='\n')
            check=source.with_suffix('.o')
            subprocess.run([str(CLANG),*FLAGS,'-c',str(source),'-o',str(check)],check=True)
            if hashlib.sha256(check.read_bytes()).digest()!=hashlib.sha256(original.read_bytes()).digest():
                raise ValueError('RendererConfig baseline no longer reproduces its linked object')
            # Same-unit direct calls bypass a renamed constructor wrapper; change its actual default.
            before='wr32((c->rbx + 0xF4ull), (uint32_t)(0x3FA00000u));'
            assert text.count(before)==1
            source=out/'recomp_funcs_0807.c'
            source.write_text(text.replace(before,before.replace('0x3FA00000u','0x3F800000u')),encoding='utf-8',newline='\n')
            subprocess.run([str(CLANG),*FLAGS,'-c',str(source),'-o',str(target)],check=True)
        else:
            subprocess.run([str(BIN/'llvm-objcopy.exe'),*[f'--redefine-sym={n}=fp_original_{n}' for n in names],str(original),str(target)],check=True)
        previous=f'build/arm64/eh-overlay-objects/{original.name}'
        replacement=target.relative_to(ROOT).as_posix()
        assert previous in data['objects'] or replacement in data['objects']
        data['objects']=[replacement if row==previous else row for row in data['objects']]
    source=out/'wrappers.c'
    source.write_text('''#include "mobile_controls.h"
#include "win64_window.h"
#include "text_input.h"
#include "temperature_ui.h"
#include "../dxvk/bridge.h"
void L_000140D8DB10(CPU* c){win64_d3d11_save_photo(c);c->rip=pop64(c);}
void L_000140D8E470(CPU* c){
    CPU request=*c;request.rsp=(request.rsp-512)&~UINT64_C(15);
    request.rcx=rd64(c->rdx+0x28);
    push64(&request,UINT64_C(0xe5ee0010));
    dispatch(&request,rd64(rd64(request.rcx)+0x88)-g_image_delta);
    request.rcx=rd64(GVA(0x14400F178));request.rdx=request.rax;request.r9=c->r8;
    win64_d3d11_save_photo(&request);c->rax=request.rax;c->rip=pop64(c);
}
void fp_original_L_000141250610(CPU*);
void fp_original_L_00014124C7B0(CPU*);
void fp_original_L_000140EC6200(CPU*);
void fp_original_L_000140EC7240(CPU*);
void fp_original_L_000140E74B30(CPU*);
void fp_original_L_000140F99F00(CPU*);
void fp_original_L_000140F9D240(CPU*);
void fp_original_L_000141A04EB0(CPU*);
void fp_original_L_0001419EF190(CPU*);
void fp_original_L_000141A5EE10(CPU*);
void fp_original_L_000141A62E90(CPU*);
void L_000141A5EE10(CPU* c){fp_mobile_law_camera_init(c,fp_original_L_000141A5EE10);}
void L_000141A62E90(CPU* c){fp_mobile_law_camera(c,fp_original_L_000141A62E90);}
void L_000141A04EB0(CPU* c){fp_mobile_painters_created(c,fp_original_L_000141A04EB0);}
void L_0001419EF190(CPU* c){fp_mobile_panels_created(c,fp_original_L_0001419EF190);}
void L_000141250610(CPU* c){fp_mobile_camera_before(c);fp_original_L_000141250610(c);}
void L_00014124C7B0(CPU* c){fp_mobile_camera_destroy();fp_original_L_00014124C7B0(c);}
void L_000140EC6200(CPU* c){fp_mobile_ui_scale();fp_original_L_000140EC6200(c);}
void L_000140EC7240(CPU* c){fp_temperature_ui_before(c);fp_mobile_ui_before(c);fp_original_L_000140EC7240(c);fp_mobile_text_process(c);}

void L_000140E74B30(CPU* c){
    uint64_t game=c->rcx,epoch=win64_window_ui_begin();uint32_t frame=rd32(game+0x444);
    fp_original_L_000140E74B30(c);
    /* Loading/synchronization returns skip native UI input handling and its frame increment. */
    if(rd32(game+0x444)!=frame)win64_window_ui_end(epoch);
}
void L_000140F99F00(CPU* c){fp_mobile_ui_position(c,fp_original_L_000140F99F00);}
void L_000140F9D240(CPU* c){fp_mobile_ui_template(c,fp_original_L_000140F9D240);}
''')
    relative=source.relative_to(ROOT).as_posix()
    config=json.loads((ROOT/'port.json').read_text())
    original=Path(config['game_directory'])/'Frostpunk.exe'
    assert hashlib.sha256(original.read_bytes()).hexdigest()==config['original_exe_sha256']
    memory=pefile.PE(str(original),fast_load=True).get_memory_mapped_image();base=0x140000000
    lifter=CheckedLifter(image_size=len(memory),image_base=base,read_va=lambda a,n:memory[a-base:a-base+n])
    ring=['/* Original command-ring entries with ARM acquire/release and MSVC fences. */',
          '#include "cpu64.h"',
          'static uint32_t fp_ring_load32(uint64_t p){return __atomic_load_n((uint32_t*)(uintptr_t)p,__ATOMIC_ACQUIRE);}',
          'static void fp_ring_store32(uint64_t p,uint32_t v){__atomic_store_n((uint32_t*)(uintptr_t)p,v,__ATOMIC_RELEASE);}']
    plain=['#include "cpu64.h"']
    for address,size in ((0x140e26e40,0x4b),(0x140e26f30,0x4d),(0x140e26f90,0x5d)):
        raw=memory[address-base:address-base+size];assert raw[-1]==0xc3 and memory[address-base+size]==0xcc
        body=lifter.lift_function(raw,address,name=f'L_{address:012X}')
        assert body.count('__atomic_thread_fence(__ATOMIC_SEQ_CST)')==1
        # The only TODO is the unreachable high-half divide guard after XOR EDX,EDX.
        assert body.count('RECOMP_TODO(')==(1 if address==0x140e26f90 else 0)
        ring.append(body.replace('rd32(','fp_ring_load32(').replace('wr32(','fp_ring_store32('))
        plain.append(body.replace(f'L_{address:012X}(',f'plain_{address:X}(')
                     .replace('__atomic_thread_fence(__ATOMIC_SEQ_CST);',''))
    (ROOT/'android/host/command_ring.c').write_text('\n'.join(ring)+'\n')
    (out/'command-ring-plain.c').write_text('\n'.join(plain)+'\n')
    for name in ('android/host/mobile_controls.c','android/host/temperature_ui.c','android/host/text_input.c','android/host/command_ring.c',
                 'android/host/zip_files.c','reference/launcher-zipmount/zipmount.c',relative):
        if name not in data['sources']:data['sources'].append(name)
    inputs.write_text(json.dumps(data,indent=1)+'\n')
    print(json.dumps(dict(wrapped=hooks,preserved_original_eh=True,renderer_baseline_identical=True,renderer_default_percent=100)))


if __name__=='__main__':main()
