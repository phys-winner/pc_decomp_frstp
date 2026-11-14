"""Build an isolated original-UI render-scale candidate; no production or device writes."""
import difflib
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

import pefile
from leaf_probe import ROOT, windows_toolchain
from prepare_raw_archives import entries, RECORD

OUT = ROOT / 'build/render-scale-ui-candidate'
EXE_SHA = '45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc'
ENTRY_SHA = 'e4d348c7af318bdcea2fe2bbfd4d5ed758885b798f7f5a5025605f09f675cad7'
KEY = 3500207806
FACTORS = [0x40000000, 0x3fd55555, 0x3fb6db6e, 0x3fa00000, 0x3f8e38e3, 0x3f800000]
GATES = [(0x141A17690, 0x141A17900), (0x141A17C60, 0x141A17E78),
         (0x140E816D0, 0x140E816F0), (0x140E81CBB, 0x140E81CDA),
         (0x140E64708, 0x140E64751), (0x140D87190, 0x140D87197),
         (0x140E4D3C0, 0x140E4D43A), (0x140E4F710, 0x140E4F79F)]


def sha(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Original source anchor differs: ' + old[:100])
    return text.replace(old, new, 1)


def function(text, name):
    start = text.index('void L_000' + name + '(CPU *c)\n')
    end = text.find('\nvoid ', start + 1)
    return text[start:end if end != -1 else len(text)]


def options(data, at):
    count, = struct.unpack_from('<I', data, at); at += 4
    if count > 32:
        raise ValueError('Unexpected original option count')
    result = []
    for _ in range(count):
        if data[at] != 1 or data[at + 5] != 1:
            raise ValueError('Unexpected original option record')
        value, = struct.unpack_from('<I', data, at + 1)
        end = data.index(0, at + 6)
        result.append((value, data[at + 6:end].decode('utf8'), data[end + 1]))
        at = end + 2
    return result, at


def resource(original):
    if hashlib.sha256(original).hexdigest() != ENTRY_SHA or len(original) != 4811:
        raise ValueError('Original SettingsConfig resource differs')
    if struct.unpack_from('<I', original, 0x160)[0] != 18:
        raise ValueError('Original settings count differs')
    old_options, end = options(original, 0x664)
    if end != 0x72a or [x[0] for x in old_options] != list(range(5)):
        raise ValueError('Original quality Combo layout differs')
    if original[0x5e0:0x5f7] != b'\1FrostpunkSettingCombo\0' or original[0x5f7:0x5ff] != struct.pack('<II', 9, 0):
        raise ValueError('Original Combo header differs')
    # Copy the native Graphics Combo base, including category GUID and UI flags.
    row = bytearray(original[0x5e0:0x5f7])
    row += struct.pack('<II', 20, 0) + b'\1Render scale\0' + original[0x640:0x664]
    row += struct.pack('<I', 6)
    for value in range(50, 101, 10):
        row += b'\1' + struct.pack('<I', value) + b'\1' + str(value).encode() + b'%\0\0'
    changed = bytearray(original[:0x5e0] + row + original[0x5e0:])
    struct.pack_into('<I', changed, 0x160, 19)
    restored = bytearray(changed[:0x5e0] + changed[0x5e0 + len(row):])
    struct.pack_into('<I', restored, 0x160, 18)
    if restored != original:
        raise ValueError('Changes outside the new row/count')
    start = len(original[0x5e0:0x5f7]) + 8 + len(b'\1Render scale\0') + len(original[0x640:0x664])
    parsed, parsed_end = options(row, start)
    if parsed_end != len(row) or parsed != [(v, str(v) + '%', 0) for v in range(50, 101, 10)]:
        raise ValueError('New Combo did not round-trip')
    return bytes(changed), bytes(row)


def generate():
    OUT.mkdir(parents=True, exist_ok=True)
    cfg = json.loads((ROOT / 'port.json').read_text())
    exe = Path(cfg['game_directory']) / 'Frostpunk.exe'
    if sha(exe) != EXE_SHA:
        raise ValueError('Original EXE differs')
    pe = pefile.PE(exe, fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
    meta = json.loads((ROOT / 'generated/frostpunk-x64/recomp_meta.json').read_text())
    sources = {}
    for unit in ['0809', '0959']:
        path = ROOT / ('generated/frostpunk-x64/recomp_funcs_' + unit + '.c')
        if sha(path) != meta['generated_files'][path.name]:
            raise ValueError('Sealed canonical source differs')
        sources[unit] = path.read_text()
    header = '#pragma once\n#include "cpu64.h"\n#include <stdio.h>\n#include <errno.h>\n'
    gate_rows = []
    for index, (va, end) in enumerate(GATES):
        raw = pe.get_data(va - base, end - va)
        if len(raw) != end - va:
            raise ValueError('Original byte gate truncated')
        header += 'static const unsigned char rs_gate_%u[]={%s};\n' % (index, ','.join('0x%02x' % b for b in raw))
        gate_rows.append(dict(va=hex(va), end=hex(end), bytes=raw.hex(), sha256=hashlib.sha256(raw).hexdigest()))
    header += 'static const uint32_t rs_gate_rvas[8]={' + ','.join('0x%xu' % (a-base) for a,z in GATES) + '};\n'
    header += '''static void rs_bad(const char *why) {
    fprintf(stderr,"STOP render scale candidate: %s\\n",why); fflush(stderr); exit(73);
}
static void rs_verify(void) { int saved=errno;
'''
    for index, (va, end) in enumerate(GATES):
        header += '    if(memcmp((const void*)(uintptr_t)GVA(0x%x),rs_gate_%u,sizeof(rs_gate_%u)))rs_bad("original bytes");\n' % (va, index, index)
    header += '    errno=saved;\n}\n'
    header += 'static const uint32_t rs_factors[6]={' + ','.join('0x%xu' % v for v in FACTORS) + '};\n'
    header += '''static uint32_t rs_percent(uint32_t bits) {
    for(unsigned i=0;i<6;++i)if(bits==rs_factors[i])return 50+10*i;
    rs_bad("unrepresented persisted factor");return 0;
}
static uint32_t rs_factor(uint32_t percent) {
    if(percent<50||percent>100||percent%10)rs_bad("invalid percentage");
    return rs_factors[(percent-50)/10];
}
'''
    (OUT / 'render_scale_candidate.h').write_text(header)
    changed = dict(sources)
    get_anchor = '    SET32(c->rcx, rd32((c->rbx + 0x20ull)));                     /* 000141A17C80: mov ecx, dword ptr [rbx + 0x20] */\n'
    get_extra = '''    if(R32(c->rcx)==20u) {
        rs_verify();
        SET32(c->rdx,rs_percent(rd32(c->rdi+0x1a4u)));
        goto L_000141A17E2F;
    }
'''
    set_anchor = '    flags_sub(c, R32(c->rbx), 0x12u, 4);                         /* 000141A176D7: cmp ebx, 0x12 */\n'
    set_extra = '''    if(R32(c->rbx)==19u) {
        rs_verify();
        uint32_t factor=rs_factor(rd32(c->rsp+0x50u));
        wr32(c->rdi+0xf4u,factor);
        wr8(c->rsi,1);
        goto L_000141A17706;
    }
'''
    changed['0959'] = once(changed['0959'], get_anchor, get_anchor + get_extra)
    changed['0959'] = once(changed['0959'], set_anchor, set_anchor + set_extra)
    packet_anchors = [
        '    if (!c->zf) goto L_000140E816E3;                             /* 000140E816D7: jne 0x140e816e3 */',
        '    if (!c->zf) goto L_000140E81CCA;                             /* 000140E81CBD: jne 0x140e81cca */']
    for old in packet_anchors:
        changed['0809'] = once(changed['0809'], old, '    rs_verify(); /* Explicit render scale applies independently of quality preset. */')
    patches = []
    for unit in sources:
        candidate = once(changed[unit], '#include "recomp_funcs.h"', '#include "recomp_funcs.h"\n#include "render_scale_candidate.h"')
        (OUT / ('recomp_funcs_' + unit + '.c')).write_text(candidate)
        patches.extend(difflib.unified_diff(sources[unit].splitlines(True), candidate.splitlines(True),
            fromfile='canonical/recomp_funcs_' + unit + '.c', tofile='candidate/recomp_funcs_' + unit + '.c'))
    (OUT / 'candidate.patch').write_text(''.join(patches))
    source_index = ROOT / 'build/raw-archives/common/common.idx'
    source_data = ROOT / 'build/raw-archives/common/common.dat'
    rows = entries(source_index.read_bytes(), source_data.stat().st_size)
    row = [r for r in rows if r[0] == KEY]
    if len(row) != 1 or row[0][1:3] != (4811, 4811) or row[0][4]:
        raise ValueError('Original SettingsConfig archive geometry differs')
    with source_data.open('rb') as f:
        f.seek(row[0][3]); original = f.read(row[0][1])
    data, inserted = resource(original)
    (OUT / 'settings-original.bin').write_bytes(original)
    (OUT / 'settings-render-scale.bin').write_bytes(data)
    (OUT / 'new-combo.bin').write_bytes(inserted)
    report = dict(success=True, production_modified=False, device_actions=False,
        original_exe_sha256=EXE_SHA, gates=gate_rows,
        resource=dict(archive='common', key=KEY, original_offset=row[0][3], original_bytes=4811,
            original_sha256=ENTRY_SHA, candidate_bytes=len(data), candidate_sha256=hashlib.sha256(data).hexdigest(),
            setting_id=20, insert_before_setting_id=9, after_setting_id=8,
            display_label='Render scale', percentages=list(range(50,101,10)), default_percent=80,
            unchanged_original_rows=18, only_count_and_insert_changed=True),
        factors=[dict(percent=50+10*i,bits=hex(v)) for i,v in enumerate(FACTORS)],
        scope='Isolated source/resource candidate. Native UI appearance, complete config save/load and RT recreation require hardware proof.',
        source_hashes={str(p.relative_to(ROOT)):sha(p) for p in [Path(__file__), ROOT/'port.json',
            ROOT/'generated/frostpunk-x64/recomp_meta.json',source_index] + [ROOT/('generated/frostpunk-x64/recomp_funcs_'+u+'.c') for u in sources]})
    (OUT / 'generation.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


def build(report):
    canonical = ROOT / 'generated/frostpunk-x64'
    source = {u:(canonical / ('recomp_funcs_'+u+'.c')).read_text() for u in ['0801','0805','0806','0807','0808','0809','0959']}
    modified = {u:(OUT / ('recomp_funcs_'+u+'.c')).read_text() for u in ['0809','0959']}
    bodies = []
    selected = [('0959','141A17690','baseline_set'),('0959','141A17C60','baseline_get'),
        ('0959','141A17F50','native_select'),('0959','141A17950','native_left'),
        ('0959','141A17AE0','native_right'),('0959','141A14F20','native_apply'),
        ('0959','141A14CE0','native_delegate'),('0959','141A146B0','native_wrapper'),
        ('0806','140E4C470','native_field_set'),('0806','140E4D3C0','native_read_float'),
        ('0806','140E4F710','native_write_float'),('0808','140E66520','native_quality'),
        ('0809','140E878F0','native_fullscreen'),('0809','140E86A10','native_get_fullscreen')]
    # The original field-address leaf has no independent extent in the sealed table.
    address_body='''static void native_field_address(CPU*c) {
    c->rip=0x140d87190ull;SET32(c->rax,rd32(c->rcx+0x28u));
    c->rip=0x140d87193ull;SET64(c->rax,c->rax+c->rdx);c->rsp+=8;
}
'''
    bodies.append(address_body)
    for unit, va, name in selected:
        bodies.append(function(source[unit],va).replace('void L_000'+va+'(', 'static void '+name+'(',1))
    extra=[]
    for va in re.findall(r'^void L_000([0-9A-F]+)\(CPU \*c\)',source['0959'],re.M):
        number=int(va,16)
        if ((0x141a17690<number<0x141a1794d) or
            (0x141a17950<number<0x141a17c60) or
            (0x141a17c60<number<0x141a17e78)) and va!='141A17AE0':
            name='native_fragment_'+va
            bodies.append(function(source['0959'],va).replace('void L_000'+va+'(', 'static void '+name+'(',1))
            extra.append('case UINT64_C(0x'+va+'): '+name+'(c);return;')
    (OUT/'render_scale_dispatch.inc').write_text('\n'.join(extra)+'\n')
    for va,name in [('141A17690','candidate_set'),('141A17C60','candidate_get')]:
        bodies.append(function(modified['0959'],va).replace('void L_000'+va+'(', 'static void '+name+'(',1))
    initial_begin='    flags_sub(c, rd32((c->rbx + 0x38ull)), 0x0u, 4);             /* 000140E816D0:'
    initial_end='    SET64(c->rdx, GVA(0x142980900));                             /* 000140E816F0:'
    refresh_begin='    flags_logicz(c, R32(c->rdx) & R32(c->rdx), 4);               /* 000140E81CBB:'
    refresh_end='    SET64(c->rdx, GVA(0x142980900));                             /* 000140E81CDA:'
    for name, text in [('baseline',source['0809']),('candidate',modified['0809'])]:
        for suffix,first,last in [('initial_packet',initial_begin,initial_end),('refresh_packet',refresh_begin,refresh_end)]:
            start=text.index(first);end=text.index(last,start)
            bodies.append('static void '+name+'_'+suffix+'(CPU*c) {\n'+text[start:end]+'}\n')
    (OUT / 'render_scale_functions.inc').write_text('\n'.join(bodies))
    cfg=json.loads((ROOT/'port.json').read_text())
    exe=Path(cfg['game_directory'])/'Frostpunk.exe';pe=pefile.PE(exe,fast_load=True)
    base=pe.OPTIONAL_HEADER.ImageBase
    spans=GATES+[(0x141D4E880,0x141D4E940),(0x141A17900,0x141A1794C),
        (0x141A17E78,0x141A17EB8),(0x1421F52B8,0x1421F52BC)]
    data=bytearray(struct.pack('<I',len(spans)))
    for a,z in spans:
        raw=pe.get_data(a-base,z-a);data+=struct.pack('<II',a-base,len(raw))+raw
    (OUT/'image-blocks.bin').write_bytes(data)
    bins,env=windows_toolchain();ndk=Path(cfg['android_sdk'])/'ndk'/cfg['android_ndk_version']/'toolchains/llvm/prebuilt/windows-x86_64/bin'
    clang=ndk/'clang.exe';commands=[]
    def run(args,expected=0):
        command=list(map(str,args));commands.append(command)
        result=subprocess.run(command,cwd=OUT,env=env,capture_output=True,text=True,timeout=120)
        if result.returncode!=expected:
            (OUT/'failure.json').write_text(json.dumps(dict(command=command,expected=expected,actual=result.returncode,stdout=result.stdout,stderr=result.stderr),indent=2)+'\n')
            raise RuntimeError(result.stdout[-5000:]+result.stderr[-10000:])
        return result
    includes=['-I'+str(ROOT/'native/runtime64'),'-I'+str(OUT)]
    fixture=ROOT/'tests/android_render_scale_fixture.c'
    obj=OUT/'render-scale-host.obj';host=OUT/'render-scale-host.exe'
    run([clang,'--target=x86_64-pc-windows-msvc','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
         '-D_CRT_SECURE_NO_WARNINGS',*includes,'-c',fixture,'-o',obj])
    run([bins/'link.exe','/nologo','/OUT:'+str(host),obj,'libcmt.lib','oldnames.lib','kernel32.lib'])
    result=json.loads(run([host,OUT/'image-blocks.bin']).stdout)
    negative=[]
    for mode,args in [('bad-factor',[])]+[('bad-percent',[str(p)])for p in [0,49,51,99,101,0xffffffff]]+[('bad-gate',[str(i)])for i in range(8)]:
        value=run([host,OUT/'image-blocks.bin',mode,*args],73)
        if 'STOP render scale candidate:' not in value.stderr:
            raise ValueError('Negative case did not fail at the intended boundary')
        negative.append(dict(mode=mode,args=args,exit=73,stderr=value.stderr.strip()))
    arm=OUT/'render-scale-arm64'
    run([clang,'--target=aarch64-linux-android30','-O2','-std=c11','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
         '-Wl,-z,max-page-size=16384',*includes,fixture,'-lm','-o',arm])
    pins={p.relative_to(ROOT).as_posix():sha(p)for p in [Path(__file__),fixture,ROOT/'native/runtime64/cpu64.h',
        ROOT/'native/runtime64/eh64.h',OUT/'render_scale_candidate.h',OUT/'render_scale_functions.inc',OUT/'image-blocks.bin',
        OUT/'settings-render-scale.bin',OUT/'candidate.patch']}
    report.update(host=result,negative=negative,source_hashes={**report['source_hashes'],**pins},commands=commands,
        arm=dict(compiled=True,executed=False,binary=arm.relative_to(ROOT).as_posix(),binary_sha256=sha(arm),
                 arguments=['image-blocks.bin'],dataset=dict(path=(OUT/'image-blocks.bin').relative_to(ROOT).as_posix(),sha256=sha(OUT/'image-blocks.bin'))))
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    return dict(success=True,host=result,negative_cases=len(negative),arm=report['arm'],receipt=str((OUT/'result.json').relative_to(ROOT)))


if __name__ == '__main__':
    print(json.dumps(build(generate()),indent=2))
