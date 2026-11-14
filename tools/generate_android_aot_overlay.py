"""Build a byte-checked Android source map beside the sealed canonical C lift."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import capstone
import pefile
import android_render_scale_contract as render_scale

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = Path('research/android-aot-overlay.json')
TRAP_UNITS = {'0965','0974','0975','0977','0980','0982','0985','0986'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reserved_units():
    packed = json.loads((ROOT/'research/windows-packed-compare-overlay.json').read_text())
    if packed.get('success') is not True or packed.get('scope') != 'all' or len(packed['units']) != 46:
        raise ValueError('Missing approved 46-unit packed compare scope')
    names = {Path(row['canonical']['path']).name for row in packed['units']}
    names |= {'recomp_funcs_'+number+'.c' for number in TRAP_UNITS}
    if len(names) != 51 or any(not re.fullmatch(r'recomp_funcs_[0-9]{4}\.c',name) for name in names):
        raise ValueError('Unexpected Android overlay reservation scope')
    return names | render_scale.UNITS


def checked(path, expected, pins):
    relative = Path(path)
    resolved = (ROOT/relative).resolve()
    if relative.is_absolute() or not resolved.is_relative_to(ROOT.resolve()):
        raise ValueError('Android overlay provenance path escapes workspace')
    if digest(resolved) != expected:
        raise ValueError('Android overlay provenance mismatch: '+str(path))
    pins[resolved] = expected
    return resolved


def generate():
    config = json.loads((ROOT/'port.json').read_text())
    original = Path(config['game_directory'])/'Frostpunk.exe'
    if digest(original) != config['original_exe_sha256']:
        raise ValueError('Original executable changed')
    pe = pefile.PE(str(original),fast_load=True)
    decoder = capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
    pins = {}
    packed_path = ROOT/'research/windows-packed-compare-overlay.json'
    cpu_path = ROOT/'research/android-aot-cpu-sites.json'
    packed = json.loads(packed_path.read_text());cpu = json.loads(cpu_path.read_text())
    if (packed.get('success') is not True or packed.get('scope') != 'all' or
            cpu.get('success') is not True or cpu.get('count') != 84 or cpu.get('occurrences') != 90 or
            any(row['original_exe_sha256'] != config['original_exe_sha256'] for row in (packed,cpu))):
        raise ValueError('Unapproved Android CPU repair scope')
    for path in (packed_path,cpu_path,ROOT/'generated/frostpunk-x64/recomp_meta.json'):
        pins[path] = digest(path)
    metadata = json.loads((ROOT/'generated/frostpunk-x64/recomp_meta.json').read_text())
    for record in (packed['generator'],cpu['producer']):checked(record['path'],record['sha256'],pins)
    for table in (cpu['reference_inputs'],cpu['outputs'],cpu['canonical_sources']):
        for name,expected in table.items():checked(name,expected,pins)
    for unit in packed['units']:checked(unit['canonical']['path'],unit['canonical']['sha256'],pins)
    header = ROOT/'android/host/aot_cpu.h';pins[header] = digest(header)
    lines_by_path = {};sites = [];occurrences = 0
    for kind, rows in (('packed',packed['sites']),('instruction',cpu['sites'])):
        for row in rows:
            va = int(row['va'],16);raw = bytes.fromhex(row['bytes'])
            instruction = next(decoder.disasm(pe.get_data(va-pe.OPTIONAL_HEADER.ImageBase,len(raw)),va),None)
            if (instruction is None or bytes(instruction.bytes) != raw or
                    instruction.mnemonic+' '+instruction.op_str != row['instruction']):
                raise ValueError('Original opcode/operand mismatch at '+hex(va))
            if kind == 'packed':
                replacement = row['new_code'].replace('fp_simd_packed_compare','fp_aot_packed_compare').replace('FP_CMP','FP_AOT_CMP')
            else:
                replacement = '{ if(!fp_aot_cpu_instruction(c,0x%Xull,"%s"))frostpunk_unsupported(0x%Xull,"%s"); }' % (va,row['instruction'],va,row['instruction'])
            locations = row['canonical_locations']
            for location in locations:
                name = location['path'];path = ROOT/name
                if path.name not in reserved_units() or path.parent != ROOT/'generated/frostpunk-x64':
                    raise ValueError('Unapproved source-map unit')
                if name not in lines_by_path:
                    if digest(path) != metadata['generated_files'][path.name]:raise ValueError('Sealed canonical source changed')
                    lines_by_path[name] = path.read_text().splitlines()
                lines = lines_by_path[name];index = location['line']-1;old,comment = lines[index].split(' /*',1)
                if kind == 'packed':
                    if old.strip() != row['old_code']:raise ValueError('Packed compare preimage mismatch')
                else:
                    match = re.fullmatch(r'RECOMP_TODO\(0x([0-9a-fA-F]+), "([^"]+)"\);',old.strip())
                    if not match or int(match[1],16) != va or match[2] != row['instruction']:
                        raise ValueError('Instruction trap preimage mismatch')
                lines[index] = '    '+replacement+' /*'+comment;occurrences += 1
            sites.append(dict(kind=kind,va=hex(va),bytes=raw.hex(),instruction=row['instruction'],
                              canonical_locations=locations,replacement=replacement))
    if len(sites) != 296 or occurrences != 378 or {Path(n).name for n in lines_by_path} != reserved_units()-render_scale.UNITS:
        raise ValueError('Android repair site/unit/occurrence set is incomplete')
    ui=render_scale.verified(config,pins)
    helper=(ROOT/render_scale.HEADER).read_text()
    for name in sorted(render_scale.UNITS):
        path=ROOT/'generated/frostpunk-x64'/name
        if digest(path)!=metadata['generated_files'].get(name):raise ValueError('Render scale canonical source changed')
        relative=path.relative_to(ROOT).as_posix()
        text='\n'.join(lines_by_path[relative])+'\n' if relative in lines_by_path else path.read_text()
        lines_by_path[relative]=render_scale.apply(name,text,helper).splitlines()
        pins[path]=digest(path)
    output = ROOT/'build/arm64/generated-overlay';output.mkdir(parents=True,exist_ok=True);units = []
    for name,lines in sorted(lines_by_path.items()):
        lines.insert(3,'#include "aot_cpu.h"')
        target = output/Path(name).name;target.write_text('\n'.join(lines)+'\n',encoding='utf-8',newline='\n')
        units.append(dict(canonical=dict(path=name,sha256=digest(ROOT/name)),
                          overlay=dict(path=target.relative_to(ROOT).as_posix(),sha256=digest(target))))
    for path,expected in pins.items():
        if digest(path) != expected:raise ValueError('Overlay input changed during generation')
    result = dict(success=True,target='aarch64-linux-android30',original_exe_sha256=config['original_exe_sha256'],
        canonical_metadata_sha256=pins[ROOT/'generated/frostpunk-x64/recomp_meta.json'],
        producer=dict(path='tools/generate_android_aot_overlay.py',sha256=digest(Path(__file__))),
        inputs={p.relative_to(ROOT).as_posix():v for p,v in pins.items()},units=units,sites=sites,
        occurrences=occurrences,runtime_fixture_verified=False,render_scale=ui,
        scope='212 packed compare sites, 84 exact instruction traps and four qualified native render-scale UI edits; XGETBV unchanged')
    (ROOT/MANIFEST).write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


def verified_overlay(config, pins):
    path = ROOT/MANIFEST;manifest = json.loads(path.read_text())
    if (manifest.get('success') is not True or manifest.get('target') != 'aarch64-linux-android30' or
            manifest.get('original_exe_sha256') != config['original_exe_sha256'] or
            len(manifest.get('units',[])) != len(reserved_units()) or len(manifest.get('sites',[])) != 296 or manifest.get('occurrences') != 378 or
            len({s['va'] for s in manifest.get('sites',[])}) != 296):
        raise ValueError('Unapproved Android overlay manifest')
    required={'research/windows-packed-compare-overlay.json','research/android-aot-cpu-sites.json',
              'generated/frostpunk-x64/recomp_meta.json','android/host/aot_cpu.h'}
    if not required<=manifest['inputs'].keys():raise ValueError('Android overlay input proof is incomplete')
    checked(manifest['producer']['path'],manifest['producer']['sha256'],pins)
    for name,expected in manifest['inputs'].items():checked(name,expected,pins)
    metadata=json.loads((ROOT/'generated/frostpunk-x64/recomp_meta.json').read_text())
    if manifest['canonical_metadata_sha256']!=digest(ROOT/'generated/frostpunk-x64/recomp_meta.json'):
        raise ValueError('Android overlay canonical metadata changed')
    # Reconstruct only the exact approved line edits: changing artifact+manifest
    # hashes together must not authorize arbitrary C outside the repair sites.
    expected_sites=[];materialized={}
    for kind,name in (('packed','research/windows-packed-compare-overlay.json'),('instruction','research/android-aot-cpu-sites.json')):
        upstream=json.loads((ROOT/name).read_text())
        if (upstream.get('success') is not True or upstream.get('original_exe_sha256')!=config['original_exe_sha256'] or
                len(upstream.get('sites',[]))!=(212 if kind=='packed' else 84)):
            raise ValueError('Unapproved Android overlay upstream repair proof')
        for row in upstream['sites']:
            va=int(row['va'],16)
            replacement=(row['new_code'].replace('fp_simd_packed_compare','fp_aot_packed_compare').replace('FP_CMP','FP_AOT_CMP') if kind=='packed' else
                '{ if(!fp_aot_cpu_instruction(c,0x%Xull,"%s"))frostpunk_unsupported(0x%Xull,"%s"); }'%(va,row['instruction'],va,row['instruction']))
            expected_sites.append(dict(kind=kind,va=hex(va),bytes=row['bytes'].lower(),instruction=row['instruction'],
                canonical_locations=row['canonical_locations'],replacement=replacement))
            for location in row['canonical_locations']:
                relative=location['path'];source=ROOT/relative
                if relative not in materialized:materialized[relative]=source.read_text().splitlines()
                lines=materialized[relative];index=location['line']-1;old,comment=lines[index].split(' /*',1)
                if kind=='packed' and old.strip()!=row['old_code']:raise ValueError('Packed overlay preimage changed')
                if kind=='instruction':
                    match=re.fullmatch(r'RECOMP_TODO\(0x([0-9a-fA-F]+), "([^"]+)"\);',old.strip())
                    if not match or int(match[1],16)!=va or match[2]!=row['instruction']:raise ValueError('CPU overlay preimage changed')
                lines[index]='    '+replacement+' /*'+comment
    if manifest['sites']!=expected_sites:raise ValueError('Android overlay exact site map changed')
    ui_names=reserved_units() & render_scale.UNITS
    if ui_names:
        ui=render_scale.verified(config,pins)
        if ui_names!=render_scale.UNITS or manifest.get('render_scale')!=ui:
            raise ValueError('Render scale overlay qualification changed')
        helper=(ROOT/render_scale.HEADER).read_text()
        for name in sorted(ui_names):
            source=ROOT/'generated/frostpunk-x64'/name;relative=source.relative_to(ROOT).as_posix()
            text='\n'.join(materialized[relative])+'\n' if relative in materialized else source.read_text()
            materialized[relative]=render_scale.apply(name,text,helper).splitlines()
    replacements = {}
    for row in manifest['units']:
        canonical = checked(row['canonical']['path'],row['canonical']['sha256'],pins)
        overlay = checked(row['overlay']['path'],row['overlay']['sha256'],pins)
        if (canonical.parent != (ROOT/'generated/frostpunk-x64').resolve() or
                overlay.parent != (ROOT/'build/arm64/generated-overlay').resolve() or
                canonical.name != overlay.name or canonical in replacements):
            raise ValueError('Unapproved or duplicate Android overlay source-map unit')
        if row['canonical']['sha256']!=metadata['generated_files'].get(canonical.name):
            raise ValueError('Android canonical seal changed')
        lines=materialized[canonical.relative_to(ROOT).as_posix()];lines.insert(3,'#include "aot_cpu.h"')
        if overlay.read_bytes()!=('\n'.join(lines)+'\n').encode('utf-8'):
            raise ValueError('Android overlay has edits outside the approved source map')
        replacements[canonical] = overlay
    if {p.name for p in replacements} != reserved_units():raise ValueError('Android source-map scope changed')
    pins[path] = digest(path)
    return replacements,manifest


if __name__ == '__main__':
    argparse.ArgumentParser(description=__doc__).parse_args()
    result=generate();print(json.dumps(dict(success=result['success'],units=len(result['units']),sites=len(result['sites']),occurrences=result['occurrences'])))
