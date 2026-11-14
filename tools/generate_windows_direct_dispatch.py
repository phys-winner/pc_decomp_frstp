"""Generate the verified Windows constant-main overlay, composing the existing packed46 source map.

--host-only is cheap and reads no generated bodies. Full generation is explicit;
it scans 1000 main units but neither compiles nor edits canonical sources.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/windows-direct-dispatch-candidate'
import generate_android_direct_dispatch as direct

HEADER = '#pragma once\n#include "cpu64.h"\n/* Exact generated constant VA/function pairs only; dynamic calls use dispatch. */\nvoid fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU *));\n/* Windows startup rejects a nonzero image delta before entering generated code. */\n#undef GVA\n#define GVA(abs) ((uint64_t)(abs))\n'
ENTRY = '''
/* Internal source-map entry: linked is paired with target by the sealed producer.
 * Keep image/self/shader admission and the exact shared static call boundary. */
void fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU *)) {
    last_target=target;
    int self=self_target(target);
    uint64_t original=target-g_image_delta;
    if(!linked || self || g_image_delta || !direct_code_contains(target) ||
       original==UINT64_C(0x1410596a9) || original==UINT64_C(0x1410597e9)) {
        dispatch(c,target);return;
    }
    dispatch_static(c,target,linked,0);
}
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


ROUNDED_CVTT_SITES = (
    ('0965', 0x141AE17B9, '660f5bc0', 'cvtps2dq xmm0, xmm0', 'ps', 'xmm0', 0, 1),
    ('0965', 0x141AE1854, '660f5bc0', 'cvtps2dq xmm0, xmm0', 'ps', 'xmm0', 0, 1),
    ('0977', 0x141BABAE1, 'f20f2dc9', 'cvtsd2si ecx, xmm1', 'sd32', 'rcx', 1, 1),
    ('0977', 0x141BABB7D, 'f20f2dc1', 'cvtsd2si eax, xmm1', 'sd32', 'rax', 1, 1),
    ('0977', 0x141BABBEC, 'f20f2dc1', 'cvtsd2si eax, xmm1', 'sd32', 'rax', 1, 1),
    ('0977', 0x141BABC61, 'f20f2dc9', 'cvtsd2si ecx, xmm1', 'sd32', 'rcx', 1, 1),
    ('0986', 0x141C690B3, 'f2480f2dd1', 'cvtsd2si rdx, xmm1', 'sd64', 'rdx', 1, 1),
    ('payload_0001', 0x18001AFF2, 'f2480f2dc8', 'cvtsd2si rcx, xmm0', 'sd64', 'rcx', 0, 2),
)


def rounded_cvtt_unit(site):
    return ('payload/recomp_funcs_' + site[0][8:] + '.c' if site[0].startswith('payload_')
            else f'recomp_funcs_{site[0]}.c')


def rounded_cvtt_lines(site):
    unit, va, raw, instruction, kind, destination, source, occurrences = site
    if kind == 'ps':
        code = '{ XMM _s = c->xmm[0]; ' + ' '.join(
            f'c->xmm[0].i32[{i}] = sse_cvtt_i32(nearbyintf(_s.f32[{i}]));' for i in range(4)) + ' }'
        before = f'    {code} /* {va:012X}: {instruction} */'
    else:
        width = 32 if kind == 'sd32' else 64
        before = f'    SET{width}(c->{destination}, sse_cvtt_i{width}(nearbyint(c->xmm[{source}].f64[0])));    /* {va:012X}: {instruction} */'
    after = re.sub(r'\bsse_cvtt_i(32|64)\(', r'(sse_cvtt_i\1)(', before)
    return before, after, 4 if kind == 'ps' else 1


def rounded_cvtt_metadata():
    return dict(instructions=8, helper_calls=15, sites=[dict(
        unit=rounded_cvtt_unit(site), va=f'0x{site[1]:X}', bytes=site[2], instruction=site[3],
        occurrences=site[7], helper_calls=(4 if site[4] == 'ps' else 1)*site[7])
        for site in ROUNDED_CVTT_SITES])


def verify_rounded_cvtt_bytes(config):
    import capstone
    import pefile
    original = Path(config['game_directory']) / 'Frostpunk.exe'
    if digest(original) != config['original_exe_sha256']:
        raise ValueError('Original identity differs before rounded CVT byte check')
    payload = ROOT / 'generated/payload/input/SteamDRMP.dll'
    payload_meta = json.loads((ROOT / 'generated/payload/recomp/recomp_meta.json').read_text())
    if digest(payload) != payload_meta['payload_sha256']:
        raise ValueError('Payload identity differs before rounded CVT byte check')
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    with pefile.PE(str(original), fast_load=True) as main, pefile.PE(str(payload), fast_load=True) as extra:
        verify_e35a50_bytes(main)
        for unit, va, raw, expected, kind, destination, source, occurrences in ROUNDED_CVTT_SITES:
            pe = extra if unit.startswith('payload_') else main
            instruction = next(decoder.disasm(pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 15), va), None)
            if (instruction is None or instruction.bytes.hex() != raw or
                    instruction.mnemonic + ' ' + instruction.op_str != expected):
                raise ValueError('Rounded CVT original bytes differ at ' + hex(va))


def quarantine_rounded_cvtt(before, name):
    selected = [site for site in ROUNDED_CVTT_SITES if name == rounded_cvtt_unit(site)]
    expected = sum(rounded_cvtt_lines(site)[2]*site[7] for site in selected)
    actual = len(re.findall(r'\bsse_cvtt_i(?:32|64)\s*\(\s*nearbyintf?\s*\(', before))
    if actual != expected or re.search(r'\(sse_cvtt_i(?:32|64)\)\s*\(', before):
        raise ValueError('Rounded CVT source census differs: ' + name)
    after = before
    for site in selected:
        old, new, count = rounded_cvtt_lines(site)
        if after.count(old) != site[7]:raise ValueError('Rounded CVT exact source differs: ' + name)
        after = after.replace(old, new)
    restored = after
    for site in selected:
        old, new, count = rounded_cvtt_lines(site)
        if restored.count(new) != site[7]:raise ValueError('Rounded CVT restored count differs: ' + name)
        restored = restored.replace(new, old)
    if restored != before:
        raise ValueError('Rounded CVT quarantine changed unrelated source: ' + name)
    return after, expected


# One measured leaf only. The whole normalized body and original instruction
# bytes are sealed; this is not an arbitrary local-CPU transformation.
E35_NAME = 'recomp_funcs_0804.c'
E35_SYMBOL = 'L_000140E35A50'
E35_BODY_SHA256 = '64ee1cc091dcef6a3329dd08be7fd1f0317678055c031b9730cad3a925cdbce5'
E35_BYTES_SHA256 = '081fb116745d39d070b1e9fa56250f47ca1d6fba9d25b09ce0c291c20e103c31'
E35_STACK_GUARD = '''/* Guest buffers may alias each other. The native stack bounds must contain
 * actual RSP; this protects compiler working storage without escaping &working.
 * The generated RET increments RSP only. Conservatively exclude its slot from
 * native-stack overlap, while the CPU alias guard covers actual memory accesses. */
static __forceinline int fp_e35_stack_admit(const CPU *c,uint64_t start,uint64_t end,uint64_t native_sp) {
    if(start>=end || native_sp<start || native_sp>=end)return 0;
    if(c->rdx>UINT64_MAX-64 || c->rcx>UINT64_MAX-64 || c->rsp<0x108 || c->rsp>UINT64_MAX-8)return 0;
    return (c->rdx+64<=start || c->rdx>=end) &&
        (c->rcx+64<=start || c->rcx>=end) &&
        (c->rsp+8<=start || c->rsp-0x108>=end);
}
static __forceinline int fp_e35_cpu_disjoint(uint64_t start,uint64_t bytes,uint64_t cpu_start,uint64_t cpu_end) {
    if(start>UINT64_MAX-bytes)return 0;
    return start+bytes<=cpu_start || start>=cpu_end;
}
static __forceinline int fp_e35_admit(const CPU *c) {
    uint64_t address=(uintptr_t)c;
    if(!c || address>UINT64_MAX-sizeof(*c))return 0;
    if((_mm_getcsr()&0x1f80)!=0x1f80)return 0;
    if(c->rsp<0x108 || c->rsp>UINT64_MAX-8)return 0;
    uint64_t end=address+sizeof(*c);
    return fp_e35_cpu_disjoint(c->rdx,64,address,end) && fp_e35_cpu_disjoint(c->rcx,64,address,end) &&
        fp_e35_cpu_disjoint(c->rsp-0x108,256,address,end);
}
'''
E35_LOCAL = '''/* Normal return commits the working CPU to the original caller identity.
 * AV diagnostic snapshots may differ; production VEH does not recover guest AVs.
 * Unmasked host SSE and CPU/native-stack aliases retain the exact legacy body. */
static __declspec(noinline) void fp_e35_local(CPU *original) {
    uint64_t native_sp;
    __asm__ volatile("movq %%rsp,%0" : "=r"(native_sp));
    if(!fp_e35_stack_admit(original,__readgsqword(0x10),__readgsqword(8),native_sp)) {
        fp_e35_legacy(original);return;
    }
    CPU working=*original;
    fp_e35_ordered_body(&working);
    *original=working;
}
void L_000140E35A50(CPU *c) {
    uint64_t native_sp;
    __asm__ volatile("movq %%rsp,%0" : "=r"(native_sp));
    if(fp_e35_admit(c) && fp_e35_stack_admit(c,__readgsqword(0x10),__readgsqword(8),native_sp))
        fp_e35_local(c);
    else fp_e35_legacy(c);
}
'''


def e35a50_metadata():
    return dict(unit=E35_NAME, symbol=E35_SYMBOL, body_sha256=E35_BODY_SHA256,
        original_sha256=E35_BYTES_SHA256, original_bytes=707, original_instructions=175,
        ordered_operations=dict(mulps=35, subps=13, addps=8), original_bytes_executed=0,
        fallback='exact legacy body', fault_snapshot_equivalent=False)


def e35a50_parts(before):
    matches=list(re.finditer(r'(?m)^void L_000140E35A50\(CPU \*c\)\r?\n\{\r?\n.*?^\}\r?\n',before,re.S))
    if len(matches)!=1:raise ValueError('E35 exact function count differs')
    raw=matches[0][0];body=raw.replace('\r\n','\n')
    if hashlib.sha256(body.encode()).hexdigest()!=E35_BODY_SHA256:
        raise ValueError('E35 exact body fingerprint differs')
    helpers='#include <xmmintrin.h>\n'
    for op in ['mulps','subps','addps']:
        helpers+=f'''static __forceinline XMM fp_e35_{op}(XMM a,XMM b) {{
    __m128 d,s;memcpy(&d,&a,16);memcpy(&s,&b,16);
    __asm__ volatile("{op} %1,%0" : "+x"(d) : "x"(s));
    memcpy(&a,&d,16);return a;
}}
'''
    ordered=[];counts=dict(mulps=0,subps=0,addps=0)
    for line in body.splitlines(True):
        match=re.search(r'/\* ([0-9A-F]+): (mulps|subps|addps) xmm(\d+), (.*?) \*/',line)
        if not match:ordered.append(line);continue
        address,op,destination,operand=match.groups();d=int(destination)
        expression=re.match(r'    \{ XMM _s = (.*?); ',line)[1]
        operator={'mulps':'*','subps':'-','addps':'+'}[op]
        old='    { XMM _s = '+expression+'; '+''.join(
            f'c->xmm[{d}].f32[{i}] = c->xmm[{d}].f32[{i}] {operator} _s.f32[{i}]; ' for i in range(4))+'} '
        if not line.startswith(old):raise ValueError('E35 ordered instruction shape differs')
        new=f'    {{ XMM _s = {expression}; c->xmm[{d}] = fp_e35_{op}(c->xmm[{d}],_s); }} '
        ordered.append(direct.once(line,old,new));counts[op]+=1
    if counts!=e35a50_metadata()['ordered_operations']:raise ValueError('E35 operation census differs')
    ordered=''.join(ordered)
    legacy=direct.once(body,'void L_000140E35A50(CPU *c)','static __declspec(noinline) void fp_e35_legacy(CPU *c)')
    local=direct.once(ordered,'void L_000140E35A50(CPU *c)','static __forceinline void fp_e35_ordered_body(CPU *c)')
    fragment=helpers+E35_STACK_GUARD+legacy+local+E35_LOCAL
    return dict(raw=raw,body=body,ordered=ordered,helpers=helpers,fragment=fragment)


def rewrite_e35a50(before,name):
    if name!=E35_NAME:
        if re.search(r'\bvoid\s+L_000140E35A50\(',before):raise ValueError('E35 appeared in another unit')
        return before
    if 'fp_e35_' in before:raise ValueError('E35 overlay already present')
    parts=e35a50_parts(before);newline='\r\n' if '\r\n' in parts['raw'] else '\n'
    fragment=parts['fragment'].replace('\n',newline)
    after=direct.once(before,parts['raw'],fragment)
    if direct.once(after,fragment,parts['raw'])!=before:raise ValueError('E35 unrelated source drift')
    return after


def verify_e35a50_bytes(pe):
    code=pe.get_data(0x140e35a50-pe.OPTIONAL_HEADER.ImageBase,707)
    if len(code)!=707 or hashlib.sha256(code).hexdigest()!=E35_BYTES_SHA256:
        raise ValueError('E35 original bytes differ')
    return code


def rewrite_windows_unit(before, name, bindings, targets=None):
    quarantined, rounded_calls = quarantine_rounded_cvtt(before, name)
    after, counts = direct.rewrite_unit(quarantined, bindings, targets)
    return rewrite_e35a50(after, name), counts, rounded_calls


def generate_payload_quarantine(pins, directory):
    # One exact auxiliary TU. No main-entry header or direct-call rewrite here.
    folder = ROOT / 'generated/payload/recomp'
    metadata = folder / 'recomp_meta.json';meta = json.loads(metadata.read_text())
    source = folder / 'recomp_funcs_0001.c';header = folder / 'recomp_funcs.h'
    for path in [source, header]:
        if digest(path) != meta['generated_files'][path.name]:raise ValueError('Payload canonical source differs')
        pins[path.resolve()] = digest(path)
    pins[metadata.resolve()] = digest(metadata)
    payload = ROOT / 'generated/payload/input/SteamDRMP.dll';pins[payload.resolve()] = meta['payload_sha256']
    after, calls = quarantine_rounded_cvtt(source.read_text(), 'payload/recomp_funcs_0001.c')
    if calls != 2:raise ValueError('Payload rounded CVT coverage differs')
    output = directory / 'payload' / source.name;output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(after, encoding='utf-8', newline='\n')
    local_header = output.parent / header.name;local_header.write_bytes(header.read_bytes())
    item = lambda path:dict(path=path.relative_to(ROOT).as_posix(), sha256=digest(path))
    return dict(canonical=item(source), overlay=item(output), rounded_cvtt_calls=calls,
                header=dict(canonical=item(header), overlay=item(local_header)))


def verify_fixed_image_contract(text):
    # The private main header may fold GVA only while Windows rejects relocation.
    startup = '''    direct_image_reset();
    if(!fp_load_image(game_exe,&game))return 74;
    g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)game.preferred_base;
    if(g_image_delta)stop("preferred guest address occupied; full relocation not validated");
    if(!fp_extra_aot_init(&game))stop("extra AOT original opcode mismatch");
    if(!fp_shader_lookup_init(&game))stop("shader lookup original opcode mismatch");
    direct_image_init();
'''
    expected = [
        'g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)site;',
        'active_cpu=old_cpu;g_image_delta=old_delta;return enabled;',
        r'g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)va;memcpy(game.base,"\x66\x0f\x38\x25\xd1",5);',
        'active_cpu=old_cpu;g_image_delta=old_delta;return tested;',
        'g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)site->va;memcpy(game.base,site->bytes,site->size);',
        'active_cpu=old_cpu;g_image_delta=old_delta;return tested;',
        'g_image_delta=(int64_t)(uintptr_t)game.base-(int64_t)game.preferred_base;',
    ]
    writes = re.findall(r'^.*(?:\bg_image_delta\s*[+\-*/%&|^<>]*=(?!=)|(?:\+\+|--)\s*g_image_delta|\bg_image_delta\s*(?:\+\+|--)).*$', text, re.M)
    if text.count(startup) != 1 or [line.strip() for line in writes] != expected:
        raise ValueError('Windows fixed-image startup or image-delta writes changed')


def host_candidate():
    source = ROOT / 'native/windows/host.c'
    raw = source.read_bytes();text = raw.decode().replace('\r\n', '\n')
    newline = '\r\n' if b'\r\n' in raw else '\n'
    verify_fixed_image_contract(text)
    if text.replace('\n', newline).encode() != raw:raise ValueError('Mixed host newlines')
    required = 'static __forceinline void dispatch_static(CPU *c,uint64_t target,void (*fn)(CPU *),int self) {'
    if text.count(required) != 1:raise ValueError('Requires current split static dispatcher')
    if text.count('static __forceinline int direct_code_contains(uint64_t target) {') != 1:
        raise ValueError('Requires validated executable-span admission')
    if text.count('    direct_image_init();\n    fp_windows_input_init') != 1:
        raise ValueError('Requires executable-span initialization before guest entry')
    anchor = 'void dispatch_jmp(CPU *c,uint64_t target) {dispatch(c,target);}'
    candidate = direct.once(text, anchor, ENTRY + '\n' + anchor)
    if direct.once(candidate, ENTRY + '\n' + anchor, anchor) != text:
        raise ValueError('Host edit roundtrip failed')
    (OUT / 'host.before.c').write_bytes(raw)
    (OUT / 'host.direct.c').write_bytes(candidate.replace('\n', newline).encode())
    (OUT / 'host.patch').write_text(''.join(difflib.unified_diff(text.splitlines(True), candidate.splitlines(True),
        fromfile='a/native/windows/host.c', tofile='b/native/windows/host.c')), encoding='utf-8', newline='\n')
    info = dict(success=True, host_before_sha256=hashlib.sha256(raw).hexdigest(),
                host_candidate_sha256=digest(OUT / 'host.direct.c'),
                generator_sha256=digest(__file__), canonical_modified=False,
                generated_body_scan=False, compiled=False, runtime_qualified=False)
    (OUT / 'host-draft.json').write_text(json.dumps(info, indent=2) + '\n')
    return info


def generate():
    # This verifies the existing real Windows packed-comparison oracle and all
    # upstream inputs. Do not substitute Android's different 53-unit overlays.
    from build_windows import packed_compare_overlay
    config_path = ROOT / 'port.json';config = json.loads(config_path.read_text())
    pins = {};previous, upstream = packed_compare_overlay(config, pins)
    verify_rounded_cvtt_bytes(config)
    if upstream['scope'] != 'all' or len(previous) != 46:
        raise ValueError('Expected proven Windows packed46 upstream')
    meta_path = ROOT / 'generated/frostpunk-x64/recomp_meta.json';meta = json.loads(meta_path.read_text())
    table = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
    declarations = ROOT / 'generated/frostpunk-x64/recomp_funcs.h'
    for p in [table, declarations]:
        if digest(p) != meta['generated_files'][p.name]:raise ValueError('Canonical binding input differs')
        pins[p.resolve()] = digest(p)
    if meta['original_exe_sha256'] != config['original_exe_sha256']:
        raise ValueError('Original image identity differs')
    bindings = direct.binding_table(table.read_text(), declarations.read_text())
    # Exact source restoration rejects altered pairs, headers, dynamic calls,
    # any new literal dispatch_jmp, and unrelated edits before compilation.
    sample = '#include "recomp_funcs.h"\nvoid sample(CPU*c){dispatch(c, 0x140E33250ull);dispatch(c, c->rax);dispatch(c, 0x140D95650ull);dispatch(c, 0x141BABD20ull);dispatch(c, 0x140000001ull);}\n'
    rewritten, counts = direct.rewrite_unit(sample, bindings)
    if [counts[k] for k in ['eligible', 'manual', 'openal', 'other']] != [1, 1, 1, 1]:
        raise ValueError('Generator exclusion fixture differs')
    rejected = 0
    for bad in [rewritten.replace('L_000140E33250', 'L_000140E33410'),
                rewritten.replace('0x140E33250ull', '0x140E33251ull'),
                rewritten + '\n/* unapproved drift */',
                rewritten.replace('dispatch(c, c->rax)', 'dispatch(c, c->rbx)')]:
        try:direct.verify_unit(sample, bad, bindings, 1)
        except ValueError:rejected += 1
        else:raise ValueError('Generator accepted mismatched source')
    for p in [meta_path, config_path, Path(__file__), Path(direct.__file__), ROOT / 'tools/build_windows.py']:
        pins[p.resolve()] = digest(p)
    info = host_candidate();pins[(ROOT / 'native/windows/host.c').resolve()] = info['host_before_sha256']
    directory = OUT / 'generated-overlay';directory.mkdir(exist_ok=True)
    header = directory / 'aot_dispatch.h';header.write_text(HEADER, encoding='utf-8', newline='\n')
    units = [];targets = set();totals = dict(eligible=0, manual=0, openal=0, other=0);rounded_total=0
    canonicals = sorted((ROOT / 'generated/frostpunk-x64').glob('recomp_funcs_*.c'))
    if len(canonicals) != 1000:raise ValueError('Canonical main TU count differs')
    for canonical in canonicals:
        expected = meta['generated_files'][canonical.name]
        if digest(canonical) != expected:raise ValueError('Canonical source changed: ' + canonical.name)
        source = previous.get(canonical.resolve(), canonical)
        source_hash = digest(source)
        if source != canonical and pins.get(source.resolve()) != source_hash:
            raise ValueError('Upstream source was not oracle pinned')
        before = source.read_bytes().decode();after, counts, rounded_calls = rewrite_windows_unit(before, canonical.name, bindings, targets)
        rounded_total += rounded_calls
        if not counts['eligible']:raise ValueError('Expected at least one known-main call per TU')
        output = directory / canonical.name;output.write_bytes(after.encode())
        for k in totals:totals[k] += counts[k]
        units.append(dict(canonical=dict(path=canonical.relative_to(ROOT).as_posix(), sha256=expected),
            input=dict(path=source.relative_to(ROOT).as_posix(), sha256=source_hash),
            overlay=dict(path=output.relative_to(ROOT).as_posix(), sha256=digest(output)), counts=counts, rounded_cvtt_calls=rounded_calls))
        pins[canonical.resolve()] = expected;pins[source.resolve()] = source_hash
    if rounded_total != 13:raise ValueError('Main rounded CVT quarantine coverage differs')
    auxiliary = generate_payload_quarantine(pins, directory)
    for p, expected in pins.items():
        if digest(p) != expected:raise ValueError('Source drift during generation: ' + str(p))
    manifest = dict(success=True, target='x86_64-pc-windows-msvc', runtime_qualified=False,
        source_map_only=True, original_exe_sha256=config['original_exe_sha256'],
        canonical_metadata_sha256=digest(meta_path), host=info, upstream=upstream,
        inputs={p.relative_to(ROOT).as_posix():v for p,v in pins.items()},
        header=dict(path=header.relative_to(ROOT).as_posix(), sha256=digest(header)), units=units,
        counts=totals, unique_target_count=len(targets), negative_source_mutations_rejected=rejected,
        rounded_cvtt_quarantine=rounded_cvtt_metadata(), auxiliary_units=[auxiliary], e35a50=e35a50_metadata(),
        conservative_exclusions=dict(manual=[hex(x) for x in sorted(direct.MANUAL)],
                                     OpenAL=[hex(direct.AUDIO_BEGIN), hex(direct.AUDIO_END)]))
    (OUT / 'source-map.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return {k:v for k,v in manifest.items() if k in ['success', 'target', 'runtime_qualified', 'counts', 'unique_target_count', 'negative_source_mutations_rejected']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-only', action='store_true')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    print(json.dumps(host_candidate() if args.host_only else generate(), indent=2))
