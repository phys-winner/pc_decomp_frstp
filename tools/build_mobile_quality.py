"""Route mobile effects through the early shadow/SSAO and particle-fill gates."""
import hashlib
import json
import subprocess
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from fastbuild import BIN, INPUTS, ROOT
from build_port_settings_unit import FLAGS
import generate_android_direct_dispatch as direct
from switch_performance_overlay import texture as switch_texture,gfx as switch_gfx
from mobile_detail_overlay import apply as mobile_detail
from native_visibility_overlay import apply as native_visibility
from entity_walk_overlay import apply as entity_walk


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def particle_object(out):
    name = 'recomp_funcs_0820'
    previous = 'build/arm64/eh-overlay-objects/' + name + '.o'
    original = ROOT / previous
    assert sha(original) == 'f4a8a9035875b1b34b0499e6de15c20b31f5032d580a5b39acab2802a5433ded'
    source = ROOT / ('build/arm64/generated-overlay/' + name + '.c')
    assert sha(source) == '0ac688e95abda78daccbabe2dc2f47b733013e12b1c12c8dc57f5b0edb37b464'
    bindings = direct.binding_table((ROOT / 'generated/frostpunk-x64/recomp_dispatch.c').read_text(),
                                    (ROOT / 'generated/frostpunk-x64/recomp_funcs.h').read_text())
    text, _ = direct.rewrite_unit(source.read_text(), bindings)
    folder = out / 'particles'
    folder.mkdir(exist_ok=True)
    lifted, control = folder / (name + '.c'), folder / 'control.o'
    lifted.write_text(text, encoding='utf-8', newline='\n')
    subprocess.run([str(BIN / 'clang.exe'), *FLAGS, '-c', str(lifted), '-o', str(control)], check=True)
    assert sha(control) == sha(original), 'Particle control no longer matches production'
    anchor = '    SET32(c->r12, rd32((c->rdi + 0x56Cull)));'
    assert text.count(anchor) == 1
    # Both draw callers already skip the original zero-count return; simulation state stays intact.
    text = 'int fp_mobile_particle_fill_disabled(void);\n' + text.replace(
        anchor, anchor + '\n    if(fp_mobile_particle_fill_disabled())SET32(c->r12,0u);', 1)
    text = switch_texture(text)
    lifted.write_text(text, encoding='utf-8', newline='\n')
    target = folder / (name + '.o')
    subprocess.run([str(BIN / 'clang.exe'), *FLAGS, '-c', str(lifted), '-o', str(target)], check=True)
    replacement = target.relative_to(ROOT).as_posix()
    return previous, replacement, dict(symbol='L_000140F6D0D0', original_sha256=sha(original),
        source_sha256=sha(source), lifted_source_sha256=sha(lifted), replacement_sha256=sha(target),
        control_byte_identical=True, all_unit_entry_routes_preserved=True)


def gfx_object(out):
    name='recomp_funcs_0809'
    previous='build/arm64/eh-overlay-objects/'+name+'.o'
    original=ROOT/previous;source=ROOT/('build/arm64/generated-overlay/'+name+'.c')
    bindings=direct.binding_table((ROOT/'generated/frostpunk-x64/recomp_dispatch.c').read_text(),(ROOT/'generated/frostpunk-x64/recomp_funcs.h').read_text())
    text,_=direct.rewrite_unit(source.read_text(),bindings)
    folder=out/'switch-gfx';folder.mkdir(exist_ok=True);copy=folder/(name+'.c');control=folder/'control.o'
    copy.write_text(text,newline='\n');subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(control)],check=True)
    assert sha(control)==sha(original),'Graphics transaction control differs'
    copy.write_text(switch_gfx(text),newline='\n');target=folder/(name+'.o')
    subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(target)],check=True)
    return previous,target.relative_to(ROOT).as_posix(),dict(symbol='L_000140E81640',original_sha256=sha(original),replacement_sha256=sha(target),source_sha256=sha(copy),control_byte_identical=True)

def detail_object(out):
    from optimize_live_flags64 import optimize
    name='recomp_funcs_0822';previous='build/arm64/eh-overlay-objects/'+name+'.o'
    original=ROOT/previous;source=ROOT/('build/arm64/generated-overlay/'+name+'.c')
    bindings=direct.binding_table((ROOT/'generated/frostpunk-x64/recomp_dispatch.c').read_text(),(ROOT/'generated/frostpunk-x64/recomp_funcs.h').read_text())
    text,_=direct.rewrite_unit(source.read_text(),bindings)
    folder=out/'detail';folder.mkdir(exist_ok=True);copy=folder/(name+'.c');control=folder/'control.o'
    copy.write_text(text,newline='\n');subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(control)],check=True)
    assert sha(control)==sha(original),'Visibility detail control differs'
    text,_=optimize(native_visibility(mobile_detail(text)))
    copy.write_text(text,newline='\n');target=folder/(name+'.o')
    subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(target)],check=True)
    return previous,target.relative_to(ROOT).as_posix(),dict(symbol='L_000140F8F530',original_sha256=sha(original),replacement_sha256=sha(target),source_sha256=sha(copy),control_byte_identical=True)


def entity_walk_object(out):
    from optimize_live_flags64 import optimize
    name='recomp_funcs_0812';previous='build/arm64/eh-overlay-objects/'+name+'.o'
    original=ROOT/previous;source=ROOT/('build/arm64/generated-overlay/'+name+'.c')
    if not source.exists():source=ROOT/('generated/frostpunk-x64/'+name+'.c')
    bindings=direct.binding_table((ROOT/'generated/frostpunk-x64/recomp_dispatch.c').read_text(),(ROOT/'generated/frostpunk-x64/recomp_funcs.h').read_text())
    text,_=direct.rewrite_unit(source.read_text(),bindings)
    folder=out/'entity-walk';folder.mkdir(exist_ok=True);copy=folder/(name+'.c');control=folder/'control.o'
    copy.write_text(text,newline='\n');subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(control)],check=True)
    assert sha(control)==sha(original),'Entity walk control differs'
    text,_=optimize(text)
    transforms,_=direct.rewrite_unit((ROOT/'build/arm64/generated-overlay/recomp_funcs_0822.c').read_text(),bindings)
    transforms,_=optimize(transforms)
    visible=(ROOT/'generated/frostpunk-x64/recomp_funcs_0821.c').read_text()
    copy.write_text(entity_walk(text,transforms,visible),newline='\n');target=folder/(name+'.o')
    subprocess.run([str(BIN/'clang.exe'),*FLAGS,'-c',str(copy),'-o',str(target)],check=True)
    return previous,target.relative_to(ROOT).as_posix(),dict(symbol='L_000140EB45F0',original_sha256=sha(original),replacement_sha256=sha(target),source_sha256=sha(copy),control_byte_identical=True)


def main():
    before = INPUTS.read_bytes()
    data = json.loads(before)
    out = ROOT / 'build/fast/mobile-quality'
    out.mkdir(parents=True, exist_ok=True)
    routes = []
    for unit, symbol, original_sha in (
        ('0821', 'L_000140F752A0', '8e9ab790e1dba601340bce314ea64deb9e3d3b1b3cbb8f6f632f6434c007ecb6'),
        ('0832', 'L_0001410343F0', 'a25679f001c9cf096ee0f2a06054cdec1e129c266171209ac02dd800b9966cda'),
    ):
        name = 'recomp_funcs_' + unit + '.o'
        previous, replacement = 'build/arm64/eh-overlay-objects/' + name, 'build/fast/mobile-quality/' + name
        original, renamed = ROOT / previous, ROOT / replacement
        assert sha(original) == original_sha, 'Quality gate original object changed'
        source = ROOT / ('build/arm64/generated-overlay/recomp_funcs_' + unit + '.c')
        assert source.read_text().count(symbol) == 1, 'Same-unit gate call needs routing'
        with original.open('rb') as stream:
            elf = ELFFile(stream); text = elf.get_section_by_name('.text').data()
            refs = []
            for section in elf.iter_sections():
                if isinstance(section, RelocationSection):
                    table = elf.get_section(section['sh_link'])
                    refs.extend(r['r_offset'] for r in section.iter_relocations() if table.get_symbol(r['r_info_sym']).name == symbol)
            assert not refs, 'Same-unit gate reference needs routing'
        subprocess.run([str(BIN / 'llvm-objcopy.exe'), '--redefine-sym=' + symbol + '=fp_quality_original_' + symbol,
                        str(original), str(renamed)], check=True)
        with renamed.open('rb') as stream: assert ELFFile(stream).get_section_by_name('.text').data() == text
        assert sum(row in (previous, replacement) for row in data['objects']) == 1
        data['objects'] = [replacement if row == previous else row for row in data['objects']]
        routes.append(dict(symbol=symbol, original_sha256=original_sha, renamed_sha256=sha(renamed), same_unit_references=refs))
    previous, replacement, route = particle_object(out)
    assert sum(row in (previous, replacement) for row in data['objects']) == 1
    data['objects'] = [replacement if row == previous else row for row in data['objects']]
    routes.append(route)
    previous,replacement,route=gfx_object(out)
    assert sum(row in (previous,replacement) for row in data['objects'])==1
    data['objects']=[replacement if row==previous else row for row in data['objects']]
    routes.append(route)
    previous,replacement,route=detail_object(out)
    assert sum(row in (previous,replacement) for row in data['objects'])==1
    data['objects']=[replacement if row==previous else row for row in data['objects']]
    routes.append(route)
    previous,replacement,route=entity_walk_object(out)
    assert sum(row in (previous,replacement) for row in data['objects'])==1
    data['objects']=[replacement if row==previous else row for row in data['objects']]
    routes.append(route)
    native = 'android/host/mobile_quality.c'
    if native not in data['sources']: data['sources'].append(native)
    if 'android/host/switch_performance.c' not in data['sources']:data['sources'].append('android/host/switch_performance.c')
    if 'android/host/mobile_detail.c' not in data['sources']:data['sources'].append('android/host/mobile_detail.c')
    assert INPUTS.read_bytes() == before, 'Runtime inputs changed concurrently; rerun'
    INPUTS.write_text(json.dumps(data, indent=1) + '\n')
    receipt = dict(routes=routes, native_sha256=sha(ROOT / native), runtime_inputs_sha256=sha(INPUTS))
    (out / 'routing.json').write_text(json.dumps(receipt, indent=1) + '\n')
    print(json.dumps(receipt, indent=1))


if __name__ == '__main__': main()
