"""Exact Android-only constant-main dispatch source map; canonical stays sealed."""
import hashlib
import json
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/android-direct-dispatch-candidate'
MANUAL={0x140e222e0,0x140d95650,0x140f499a0,0x1410596a9,0x1410597e9}
MAIN_BASE=0x140000000
MAIN_END=0x144539000
AUDIO_BEGIN=0x141babd20
AUDIO_END=0x141bc2d40
OLD_CALL=re.compile(r'\b(dispatch(?:_jmp)?)\(c,\s*(0x([0-9A-Fa-f]+)ull)\)')
NEW_CALL=re.compile(r'\bfp_dispatch_main\(c,\s*(0x([0-9A-Fa-f]+)ull),\s*(L_[0-9A-Fa-f]+)\)')
HEADER='#pragma once\n#include "cpu64.h"\n/* Only exact producer-verified constant main VA/symbol pairs may pass linked. */\n__attribute__((visibility("hidden"))) void fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU *));\n'


def digest(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def once(text,old,new):
    if text.count(old)!=1:raise ValueError('Direct-dispatch exact preimage differs: '+old[:100])
    return text.replace(old,new,1)
def eligible(va):return MAIN_BASE<=va<MAIN_END and not AUDIO_BEGIN<=va<=AUDIO_END and va not in MANUAL


def binding_table(source,header):
    rows=re.findall(r'\{\s*0x([0-9A-Fa-f]+)ull,\s*(L_[0-9A-Fa-f]+)\s*\}',source)
    bindings={int(va,16):symbol for va,symbol in rows}
    declarations=set(re.findall(r'\bvoid\s+(L_[0-9A-Fa-f]+)\(CPU \*c\);',header))
    if len(rows)!=399917 or len(bindings)!=len(rows) or list(bindings)!=sorted(bindings):
        raise ValueError('Canonical main dispatch table differs')
    if any(int(symbol[2:],16)!=va or symbol not in declarations for va,symbol in bindings.items()):
        raise ValueError('Main target/symbol/declaration pair differs')
    return bindings


def rewrite_unit(text,bindings,seen_targets=None):
    if 'fp_dispatch_main' in text:raise ValueError('Direct overlay already present')
    counts=dict(eligible=0,manual=0,openal=0,other=0)
    ordered=hashlib.sha256()
    def replace(match):
        kind,literal,number=match.groups();va=int(number,16)
        if va in MANUAL:counts['manual']+=1;return match[0]
        if AUDIO_BEGIN<=va<=AUDIO_END:counts['openal']+=1;return match[0]
        if not eligible(va) or va not in bindings:counts['other']+=1;return match[0]
        if kind!='dispatch':raise ValueError('Previously absent literal dispatch_jmp requires separate proof')
        symbol=bindings[va];counts['eligible']+=1
        if seen_targets is not None:seen_targets.add(va)
        ordered.update((literal+':'+symbol+'\n').encode())
        return 'fp_dispatch_main(c, '+literal+', '+symbol+')'
    changed=OLD_CALL.sub(replace,text)
    if counts['eligible']:changed=once(changed,'#include "recomp_funcs.h"','#include "recomp_funcs.h"\n#include "aot_dispatch.h"')
    verify_unit(text,changed,bindings,counts['eligible'])
    return changed,dict(counts,ordered_bindings_sha256=ordered.hexdigest())


def verify_unit(before,after,bindings,count):
    seen=0
    def restore(match):
        nonlocal seen
        literal,number,symbol=match.groups();va=int(number,16)
        if not eligible(va) or bindings.get(va)!=symbol:
            raise ValueError('Unqualified direct target/symbol pair')
        seen+=1;return 'dispatch(c, '+literal+')'
    restored=NEW_CALL.sub(restore,after)
    if count:restored=once(restored,'#include "recomp_funcs.h"\n#include "aot_dispatch.h"','#include "recomp_funcs.h"')
    if seen!=count or restored!=before:raise ValueError('Direct overlay edits outside exact constant calls')


def bootstrap_candidate(text):
    changes=[]
    old='void dispatch(CPU *c, uint64_t target) {'
    new='static void dispatch_resolved(CPU *c, uint64_t target, void (*linked)(CPU *)) {'
    changes.append((old,new));candidate=once(text,old,new)
    old='        function=main_static_lookup(target);\n        if(function)goto static_invoke;'
    # Five immutable adapters retain their original lookup and readiness rules,
    # even if an internal caller supplies a linked canonical function for one.
    new='''        function=linked;
        if(!function || target==UINT64_C(0x140e222e0) || target==UINT64_C(0x140d95650) ||
           target==UINT64_C(0x140f499a0) || target==UINT64_C(0x1410596a9) || target==UINT64_C(0x1410597e9))
            function=main_static_lookup(target);
        if(function)goto static_invoke;'''
    changes.append((old,new));candidate=once(candidate,old,new)
    old='void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }'
    new='''void dispatch(CPU *c, uint64_t target) { dispatch_resolved(c,target,NULL); }
__attribute__((visibility("hidden"))) void fp_dispatch_main(CPU *c, uint64_t target, void (*linked)(CPU *)) { dispatch_resolved(c,target,linked); }
void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }'''
    changes.append((old,new));candidate=once(candidate,old,new)
    restored=candidate
    for old,new in reversed(changes):restored=once(restored,new,old)
    if restored!=text:raise ValueError('Shared dispatcher roundtrip failed')
    return candidate


def generate():
    """Compose the already verified CPU/UI map, then replace constant calls."""
    import generate_android_aot_overlay as upstream
    config=json.loads((ROOT/'port.json').read_text());pins={}
    previous,_=upstream.verified_overlay(config,pins)
    if len(previous)!=53:raise ValueError('Expected final53 CPU/UI upstream source map')
    metadata_path=ROOT/'generated/frostpunk-x64/recomp_meta.json';meta=json.loads(metadata_path.read_text())
    table=ROOT/'generated/frostpunk-x64/recomp_dispatch.c';header=ROOT/'generated/frostpunk-x64/recomp_funcs.h'
    for path in (table,header):
        if digest(path)!=meta['generated_files'][path.name]:raise ValueError('Canonical dispatch binding input differs')
        pins[path]=digest(path)
    bindings=binding_table(table.read_text(),header.read_text())
    pins[metadata_path]=digest(metadata_path);pins[Path(__file__).resolve()]=digest(__file__)
    pins[ROOT/'tools/generate_android_aot_overlay.py']=digest(ROOT/'tools/generate_android_aot_overlay.py')
    for path in (ROOT/'research/android-aot-overlay.json',):pins[path]=digest(path)
    directory=OUT/'generated-overlay';directory.mkdir(parents=True,exist_ok=True)
    (OUT/'aot_dispatch.h').write_text(HEADER,newline='\n')
    units=[];totals={key:0 for key in ('eligible','manual','openal','other')};targets=set()
    canonicals=sorted((ROOT/'generated/frostpunk-x64').glob('recomp_funcs_*.c'))
    if len(canonicals)!=1000:raise ValueError('Canonical TU count differs')
    for canonical in canonicals:
        original_hash=meta['generated_files'][canonical.name]
        if digest(canonical)!=original_hash:raise ValueError('Sealed canonical body differs: '+canonical.name)
        source=previous.get(canonical.resolve(),canonical);raw=source.read_bytes();source_hash=hashlib.sha256(raw).hexdigest()
        if source!=canonical and pins.get(source.resolve())!=source_hash:raise ValueError('Upstream overlay body not pinned')
        changed,counts=rewrite_unit(raw.decode(),bindings,targets)
        if not counts['eligible']:raise ValueError('Expected at least one qualified call per main TU')
        output=directory/canonical.name;output.write_text(changed,encoding='utf-8',newline='\n')
        for key in totals:totals[key]+=counts[key]
        units.append(dict(canonical=dict(path=canonical.relative_to(ROOT).as_posix(),sha256=original_hash),
            input=dict(path=source.relative_to(ROOT).as_posix(),sha256=source_hash),
            overlay=dict(path=output.relative_to(ROOT).as_posix(),sha256=digest(output)),counts=counts))
    for path,expected in pins.items():
        if digest(path)!=expected:raise ValueError('Source drift during full direct source-map generation: '+str(path))
    result=dict(success=True,target='aarch64-linux-android30',original_exe_sha256=config['original_exe_sha256'],
        canonical_metadata_sha256=digest(metadata_path),generator=dict(path='tools/generate_android_direct_dispatch.py',sha256=digest(__file__)),
        inputs={path.relative_to(ROOT).as_posix():value for path,value in pins.items()},
        header=dict(path=(OUT/'aot_dispatch.h').relative_to(ROOT).as_posix(),sha256=digest(OUT/'aot_dispatch.h')),
        units=units,counts=totals,unique_target_count=len(targets),upstream_units=53,runtime_qualified=False,
        source_contract='Canonical→verified53 CPU/UI overlays→exact literal main call substitutions;5manual adapters/fullOpenAL/unknown/dynamic stay legacy. No second native mapping table.')
    (OUT/'source-map.json').write_text(json.dumps(result,indent=2)+'\n')
    return result


def verify_source_map(config,pins):
    """Structural proof only; the integrating builder must also require HA proof."""
    import generate_android_aot_overlay as upstream
    path=OUT/'source-map.json';manifest=json.loads(path.read_text());previous,_=upstream.verified_overlay(config,pins)
    if (manifest.get('success') is not True or manifest.get('target')!='aarch64-linux-android30' or
        manifest.get('original_exe_sha256')!=config['original_exe_sha256'] or len(manifest.get('units',[]))!=1000 or
        manifest.get('upstream_units')!=53 or manifest.get('runtime_qualified') is not False):raise ValueError('Unapproved isolated direct source map')
    for name,expected in manifest['inputs'].items():
        relative=Path(name);source=(ROOT/relative).resolve()
        if relative.is_absolute() or not source.is_relative_to(ROOT.resolve()) or digest(source)!=expected:raise ValueError('Direct map provenance drift: '+name)
        pins[source]=expected
    if manifest['generator']!=dict(path='tools/generate_android_direct_dispatch.py',sha256=digest(__file__)):raise ValueError('Direct generator identity differs')
    metadata_path=ROOT/'generated/frostpunk-x64/recomp_meta.json';meta=json.loads(metadata_path.read_text())
    if manifest['canonical_metadata_sha256']!=digest(metadata_path):raise ValueError('Canonical metadata drift')
    table=ROOT/'generated/frostpunk-x64/recomp_dispatch.c';header=ROOT/'generated/frostpunk-x64/recomp_funcs.h'
    required={'tools/generate_android_direct_dispatch.py','tools/generate_android_aot_overlay.py',
              'research/android-aot-overlay.json','generated/frostpunk-x64/recomp_meta.json',
              'generated/frostpunk-x64/recomp_dispatch.c','generated/frostpunk-x64/recomp_funcs.h'}
    if not required<=manifest['inputs'].keys():raise ValueError('Direct source-map input contract incomplete')
    for p in (table,header):
        if digest(p)!=meta['generated_files'][p.name]:raise ValueError('Canonical dispatch binding drift')
    bindings=binding_table(table.read_text(),header.read_text())
    private_header=ROOT/manifest['header']['path']
    if private_header.resolve()!=(OUT/'aot_dispatch.h').resolve() or private_header.read_bytes()!=HEADER.encode() or digest(private_header)!=manifest['header']['sha256']:raise ValueError('Private dispatch header differs')
    totals={key:0 for key in ('eligible','manual','openal','other')};targets=set();replacements={}
    expected_names={path.name for path in (ROOT/'generated/frostpunk-x64').glob('recomp_funcs_*.c')}
    for row in manifest['units']:
        canonical=ROOT/row['canonical']['path'];source=ROOT/row['input']['path'];output=ROOT/row['overlay']['path']
        if (canonical.parent!=ROOT/'generated/frostpunk-x64' or canonical.name not in expected_names or canonical.resolve() in replacements or
            row['canonical']['sha256']!=meta['generated_files'][canonical.name] or digest(canonical)!=row['canonical']['sha256'] or
            source.resolve()!=previous.get(canonical.resolve(),canonical).resolve() or digest(source)!=row['input']['sha256'] or
            output.resolve()!=(OUT/'generated-overlay'/canonical.name).resolve()):raise ValueError('Invalid direct source-map unit/path')
        changed,counts=rewrite_unit(source.read_bytes().decode(),bindings,targets)
        if counts!=row['counts'] or output.read_bytes()!=changed.encode() or digest(output)!=row['overlay']['sha256']:raise ValueError('Unapproved edits outside direct call pairs')
        for key in totals:totals[key]+=counts[key]
        for p,key in [(canonical,'canonical'),(source,'input'),(output,'overlay')]:pins[p.resolve()]=row[key]['sha256']
        replacements[canonical.resolve()]=output.resolve()
    if set(p.name for p in replacements)!=expected_names or totals!=manifest['counts'] or len(targets)!=manifest['unique_target_count']:raise ValueError('Direct overlay scope incomplete')
    pins[path]=digest(path);pins[private_header.resolve()]=digest(private_header)
    return replacements,manifest


if __name__=='__main__':
    report=generate()
    print(json.dumps(dict(success=True,units=len(report['units']),counts=report['counts'],unique_targets=report['unique_target_count'],
        source_map_sha256=digest(OUT/'source-map.json'),runtime_qualified=False)))
