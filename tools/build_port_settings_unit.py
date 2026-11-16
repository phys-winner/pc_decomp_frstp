"""General/Graphics menu rows 21-27 with live output resolution.

    python tools/build_port_settings_unit.py      # then: python tools/fastbuild.py runtime apk

Unit 0959 holds the settings setter (0x141A17690) and getter (0x141A17C60), which already carry the
render-scale row 20. The production source is regenerated from build/arm64/generated-overlay plus the
direct-dispatch rewrite; compiled unchanged it must equal the linked object byte for byte (checked
first). The port rows then call android/host/port_settings.c, and build/fast/runtime-inputs.json
links build/fast/port-objects/recomp_funcs_0959.o instead of the eh-overlay object.
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import generate_android_direct_dispatch as direct  # noqa: E402

UNIT = 'recomp_funcs_0959.c'
SOURCE = ROOT / 'build/arm64/generated-overlay' / UNIT
SOURCE_SHA = 'a4a80442b9a5f826885d5000bbfc861446006461ef9043458aeca189abee62f3'
ORIGINAL = ROOT / 'build/arm64/eh-overlay-objects/recomp_funcs_0959.o'
ORIGINAL_SHA = '8c9896fd44e228817ac2e3603a4e612ba6d9386b3e67a1aa73c9500377e32dbd'
OUT = ROOT / 'build/fast/port-objects'
INPUTS = ROOT / 'build/fast/runtime-inputs.json'
FLAGS = ['--target=aarch64-linux-android30', '-O2', '-std=c11', '-fPIC', '-fvisibility=hidden',
         '-fno-strict-aliasing', '-fwrapv', '-ffp-contract=off', '-fno-math-errno', '-g0', '-w',
         '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'generated/frostpunk-x64'),
         '-I' + str(ROOT / 'android/host'), '-I' + str(ROOT / 'build/android-direct-dispatch-candidate'),
         '-include', str(ROOT / 'android/host/eh_entry_nomask.h')]
EDITS = [
    # Getter: rows 21-25 select the stored option value (rcx = row id).
    ('''    if(R32(c->rcx)==20u) {
        rs_verify();
        SET32(c->rdx,rs_percent(rd32(c->rdi+0x1a4u)));
        goto L_000141A17E2F;
    }
''', '''    if(R32(c->rcx)>=21u&&R32(c->rcx)<=32u) {
        SET32(c->rdx,fp_port_setting_get(R32(c->rcx)));
        goto L_000141A17E2F;
    }
'''),
    # Setter: rbx = row id - 1, [rsp+0x50] = chosen option value; no graphics re-apply flag.
    ('''        wr8(c->rsi,1);
        goto L_000141A17706;
    }
''', '''    if(R32(c->rbx)>=20u&&R32(c->rbx)<=31u) {
        uint32_t fp_mobile_changed=R32(c->rbx)==21u&&rd32(c->rsp+0x50u)<=1u&&fp_port_setting_get(22u)!=rd32(c->rsp+0x50u);
        fp_port_setting_set(R32(c->rbx)+1u,rd32(c->rsp+0x50u));
        if(fp_mobile_changed){wr8(c->rdi+0x2cu,1);wr8(c->rsi,1);}
        if(R32(c->rbx)==28u&&rd32(c->rsp+0x50u)==1u)fp_port_gesture_help(c);
        if(R32(c->rbx)==20u) {
            fp_mobile_output_config(c->rdi,rd32(c->rsp+0x50u));
            wr8(c->rsi,1);
        }
        goto L_000141A17706;
    }
'''),
    ('#include "recomp_funcs.h"\n#include "aot_dispatch.h"\n', '''#include "recomp_funcs.h"
#include "aot_dispatch.h"
uint32_t fp_port_setting_get(uint32_t id);
void fp_port_setting_set(uint32_t id, uint32_t value);
void fp_mobile_output_config(uint64_t config, uint32_t percent);
uint32_t fp_port_frame_rates(uint32_t rates[32]);
int fp_output_resolution_label(uint32_t percent, char *text, size_t text_n);
#include <stdio.h>
#include <string.h>
#include "port_help.inc"
static void fp_frame_options(CPU *cpu,uint64_t row) {
    if(!row||rd32(row+0x20)!=28u)return;
    uint32_t rates[32],count=fp_port_frame_rates(rates);
    uint64_t options=rd64(row+0x28);uint32_t previous=rd32(row+0x30);
    if(!options||!previous||previous>33)return;
    unsigned same=previous==count+1;
    for(uint32_t i=0;same&&i<count;++i)if(rd32(options+(uint64_t)(i+1)*16)!=rates[i])same=0;
    if(same)return;
    CPU c=*cpu;c.rsp=(c.rsp-512)&~UINT64_C(15);
    for(uint32_t i=1;i<previous;++i){
        c.rcx=options+(uint64_t)i*16+8;push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x140E52950));
    }
    wr32(row+0x30,1);
    for(uint32_t i=0;i<count;++i){
        uint64_t string=c.rsp+0x60,text=c.rsp+0x80;
        wr64(string,0);snprintf((char*)(uintptr_t)text,32,"%u FPS",rates[i]);
        c.rcx=string;c.rdx=text;push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x140E538C0));
        c.rcx=row;c.rdx=rates[i];c.r8=string;push64(&c,UINT64_C(0xe5ee0010));dispatch(&c,UINT64_C(0x141A14E20));
    }
}
static int fp_label_matches(uint64_t text, const char *want) {
    const unsigned char *bytes;
    size_t i;
    if (!text || !want) return 0;
    /* Option slots may hold a guest string object. Read only the label length, never scan for NUL. */
    bytes = (const unsigned char *)(uintptr_t)text;
    for (i = 0; want[i]; ++i) {
        if (i >= 47 || bytes[i] != (unsigned char)want[i]) return 0;
    }
    return bytes[i] == 0;
}
static void fp_output_labels(CPU *cpu, uint64_t row) {
    /* The closed combo reads strings installed by the option append (the same path as "60 FPS").
     * Asset strings are left allocated: the append overwrites the slot and must see a null there. */
    static int done, waits;
    if (done || !row || rd32(row + 0x20) != 21u) return;
    uint64_t options = rd64(row + 0x28);
    uint32_t count = rd32(row + 0x30);
    if (!options || !count || count > 16) return;
    char want[16][48];
    uint32_t values[16];
    unsigned same = 1;
    for (uint32_t i = 0; i < count; ++i) {
        values[i] = rd32(options + (uint64_t)i * 16);
        uint64_t text = rd64(options + (uint64_t)i * 16 + 8);
        if (!fp_output_resolution_label(values[i], want[i], sizeof want[i])) {
            if (waits < 4) fprintf(stderr, "FP_OUTPUT_LABEL wait i=%u value=%u\\n", i, values[i]);
            ++waits;
            return;
        }
        if (!fp_label_matches(text, want[i])) same = 0;
    }
    if (same) { done = 1; return; }
    CPU c = *cpu;
    c.rsp = (c.rsp - 512) & ~UINT64_C(15);
    for (uint32_t i = 0; i < count; ++i) wr64(options + (uint64_t)i * 16 + 8, 0);
    wr32(row + 0x30, 0);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t string = c.rsp + 0x60, chars = c.rsp + 0x80;
        wr64(string, 0);
        snprintf((char *)(uintptr_t)chars, sizeof want[i], "%s", want[i]);
        c.rcx = string;
        c.rdx = chars;
        push64(&c, UINT64_C(0xe5ee0010));
        dispatch(&c, UINT64_C(0x140E538C0));
        if (!rd64(string)) {
            fprintf(stderr, "FP_OUTPUT_LABEL create_failed i=%u\\n", i);
            return;
        }
        c.rcx = row;
        c.rdx = values[i];
        c.r8 = string;
        push64(&c, UINT64_C(0xe5ee0010));
        dispatch(&c, UINT64_C(0x141A14E20));
    }
    done = rd32(row + 0x30) == count;
    fprintf(stderr, "FP_OUTPUT_LABEL wrote count=%u first=%s\\n", rd32(row + 0x30), want[0]);
}
'''),
]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compile_unit(text, folder):
    config = json.loads((ROOT / 'port.json').read_text())
    clang = Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
    folder.mkdir(parents=True, exist_ok=True)
    # The object records the source file name, so both builds use the production name.
    source = folder / UNIT
    source.write_text(text, encoding='utf-8', newline='\n')
    target = folder / 'recomp_funcs_0959.o'
    subprocess.run([str(clang), *FLAGS, '-c', str(source), '-o', str(target)], check=True)
    return target


def main():
    if sha(SOURCE) != SOURCE_SHA or sha(ORIGINAL) != ORIGINAL_SHA:
        raise SystemExit('unit 0959 inputs differ from the linked production object')
    table = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
    header = ROOT / 'generated/frostpunk-x64/recomp_funcs.h'
    bindings = direct.binding_table(table.read_text(), header.read_text())
    production, counts = direct.rewrite_unit(SOURCE.read_bytes().decode(), bindings)
    if sha(compile_unit(production, OUT / 'baseline')) != ORIGINAL_SHA:
        raise SystemExit('regenerated production source no longer compiles to the linked object')
    text = production
    for before, after in EDITS:
        if text.count(before) != 1:
            raise SystemExit('settings anchor drift: ' + before.splitlines()[0])
        text = text.replace(before, before + after if not before.startswith('#include') else after)
    getter = 'void L_000141A17C60(CPU *c)\n{\n'
    if text.count(getter) != 1:
        raise SystemExit('settings getter anchor drift')
    text = text.replace(getter, getter + '    fp_frame_options(c,c->rdx);\n    fp_output_labels(c,c->rdx);\n')
    obj = compile_unit(text, OUT)
    inputs = json.loads(INPUTS.read_text())
    old, new = 'build/arm64/eh-overlay-objects/recomp_funcs_0959.o', obj.relative_to(ROOT).as_posix()
    inputs['objects'] = [new if item in (old, 'build/fast/port-objects/recomp_funcs_0959.o') else item for item in inputs['objects']]
    if new not in inputs['objects']:
        raise SystemExit('runtime inputs do not link unit 0959')
    if 'android/host/port_settings.c' not in inputs['sources']:
        inputs['sources'].append('android/host/port_settings.c')
    INPUTS.write_text(json.dumps(inputs, indent=1) + '\n')
    print(json.dumps(dict(direct_calls=counts['eligible'], baseline_identical=True,
                          object=str(obj.relative_to(ROOT)), sha256=sha(obj)), indent=1))


if __name__ == '__main__':
    main()
