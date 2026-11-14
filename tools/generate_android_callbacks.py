"""Recover referenced, CC-delimited callbacks omitted by the unwind catalogue.

Only original instructions are lifted; an unknown target is never a no-op.
Observed non-CC starts require exact predecessor and reference byte witnesses.
The generated table verifies the original bytes before enabling these entries.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

from lift64_checked import CheckedLifter

ROOT = Path(__file__).resolve().parents[1]


def main():
    config = json.loads((ROOT / 'port.json').read_text())
    original = Path(config['game_directory']) / 'Frostpunk.exe'
    assert hashlib.sha256(original.read_bytes()).hexdigest() == config['original_exe_sha256']
    pe = pefile.PE(str(original), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    memory = pe.get_memory_mapped_image()
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    low, high = base + text.VirtualAddress, base + text.VirtualAddress + text.SizeOfRawData
    known = {int(line.split()[0], 16) for line in (ROOT / 'config/Frostpunk.functions.txt').read_text().splitlines()
             if line and not line.startswith('#')}
    known.update(int(value, 16) for value in re.findall(r'\{(?:UINT64_C\()?0x([0-9a-fA-F]+)',
                 (ROOT / 'generated/supplemental/recomp_supplemental.c').read_text()))
    references = {}
    for source in sorted((ROOT / 'generated/frostpunk-x64').glob('recomp_funcs_*.c')):
        for match in re.finditer(r'GVA\(0x([0-9A-F]+)\).*?/\* ([0-9A-F]+): lea ', source.read_text()):
            target, site = (int(value, 16) for value in match.groups())
            if low <= target < high and target not in known:
                references.setdefault(target, []).append(site)
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    # Import thunks can be in the canonical dispatcher without a pdata entry.
    # They are allowed only as the exact tail of an explicitly witnessed callback.
    canonical = {int(value, 16) for value in re.findall(r'\{\s*0x([0-9a-fA-F]+)ull,',
                 (ROOT / 'generated/frostpunk-x64/recomp_dispatch.c').read_text())}
    observed_path = ROOT / 'config/Frostpunk.android-observed-callbacks.json'
    observed = json.loads(observed_path.read_text())
    assert observed['original_exe_sha256'] == config['original_exe_sha256']
    witnessed = {}
    for row in observed['callbacks']:
        target = int(row['address'], 16)
        raw = bytes.fromhex(row['original_bytes'])
        previous = int(row['predecessor_address'], 16)
        previous_raw = bytes.fromhex(row['predecessor_bytes'])
        reference = int(row['reference_address'], 16)
        reference_raw = bytes.fromhex(row['reference_bytes'])
        assert target not in witnessed and target in references and reference in references[target]
        assert memory[target-base:target-base+len(raw)] == raw and memory[target-base+len(raw)] == 0xcc
        assert previous+len(previous_raw) == target
        assert memory[previous-base:previous-base+len(previous_raw)] == previous_raw
        previous_decoded = list(decoder.disasm(previous_raw, previous))
        assert sum(i.size for i in previous_decoded) == len(previous_raw)
        if row.get('predecessor_terminal') == 'ret':
            assert [(i.mnemonic, i.op_str) for i in previous_decoded] == [('add', 'rsp, 0x38'), ('ret', '')]
        else:
            assert len(previous_decoded) == 1 and previous_decoded[0].mnemonic == 'jmp'
            assert previous_decoded[0].op_str == row['predecessor_target']
        assert memory[reference-base:reference-base+len(reference_raw)] == reference_raw
        reference_decoded = list(decoder.disasm(reference_raw, reference))
        assert len(reference_decoded) == 1 and reference_decoded[0].mnemonic == 'lea'
        assert reference+len(reference_raw)+int.from_bytes(reference_raw[3:], 'little', signed=True) == target
        decoded = list(decoder.disasm(raw, target))
        assert sum(i.size for i in decoded) == len(raw) and decoded[-1].mnemonic == 'jmp'
        if row.get('kind', 'lea_rcx_tail') == 'lea_rcx_tail':
            assert len(decoded) == 2 and decoded[0].mnemonic == 'lea' and decoded[0].op_str.startswith('rcx, [rip ')
        else:
            assert row['kind'] == 'destructor_array_tail' and target == 0x141CAC080
            assert [(i.mnemonic, i.op_str) for i in decoded] == [('mov', 'edx, 8'),
                ('lea', 'r9, [rip - 0xe5973c]'), ('lea', 'rcx, [rip + 0x23147dd]'),
                ('lea', 'r8d, [rdx - 6]'), ('jmp', '0x141ae0e54')]
            assert 0x140E52950 in known | canonical  # Original per-element destructor in R9.
        assert decoded[-1].op_str == row['tail_target'] and int(row['tail_target'], 16) in known | canonical
        if 'registration_target' in row:
            registration = bytes.fromhex(row['registration_bytes'])
            assert memory[reference-base:reference-base+len(registration)] == registration
            registered = list(decoder.disasm(registration, reference))
            assert sum(i.size for i in registered) == len(registration)
            assert registered[-1].mnemonic in ('call', 'jmp') and registered[-1].op_str == row['registration_target'] == '0x141ae083c'
            assert all(i.mnemonic not in ('call', 'jmp', 'ret') for i in registered[:-1])
        witnessed[target] = row
    lifter = CheckedLifter(image_size=len(memory), read_va=lambda va, n: memory[va-base:va-base+n],
                          image_base=base, original_sha256=config['original_exe_sha256'])
    rows, bodies, rejected = [], [], []
    pending = sorted(references)
    processed = set()
    for target in pending:
        if target in processed:
            continue
        processed.add(target)
        sites = references[target]
        offset = target - base
        if memory[offset-1] != 0xcc and target not in witnessed:
            rejected.append(hex(target))
            continue
        instructions = []
        for instruction in decoder.disasm(memory[offset:min(offset+4096, high-base)], target):
            if instruction.mnemonic == 'int3':
                break
            if instruction.address != target and instruction.address in known:
                break
            instructions.append(instruction)
        if not instructions or instructions[-1].mnemonic not in ('ret', 'jmp'):
            rejected.append(hex(target))
            continue
        end = instructions[-1].address + instructions[-1].size
        if memory[end-base] != 0xcc:
            rejected.append(hex(target))
            continue
        raw = memory[offset:end-base]
        if target in witnessed:
            assert raw.hex() == witnessed[target]['original_bytes']
        assert sum(i.size for i in instructions) == len(raw)
        name = f'fp_callback_{target:X}'
        body = lifter.lift_function(raw, target, name=name)
        if 'RECOMP_TODO(' in body:
            rejected.append(hex(target))
            continue
        bodies.append(body.replace('void '+name+'(', 'static void '+name+'(', 1))
        rows.append(dict(va=target, size=len(raw), raw=raw.hex(), sites=sites, name=name))
        for match in re.finditer(r'dispatch(?:_jmp)?\(c, 0x([0-9A-F]+)ull\)', body):
            callee = int(match[1], 16)
            if target in witnessed and callee == int(witnessed[target]['tail_target'], 16) and callee in canonical:
                continue
            if callee not in known and callee not in references:
                assert low <= callee < high
                references[callee] = [target]
                pending.append(callee)
    targets = known | {row['va'] for row in rows}
    # Do not install a callback whose static callees still have missing bodies.
    while True:
        keep = [(row, body) for row, body in zip(rows, bodies)
                if all(int(m[1], 16) in targets or (row['va'] in witnessed
                    and int(m[1], 16) == int(witnessed[row['va']]['tail_target'], 16) and int(m[1], 16) in canonical)
                    for m in re.finditer(r'dispatch(?:_jmp)?\(c, 0x([0-9A-F]+)ull\)', body))]
        if len(keep) == len(rows):
            break
        retained = {row['va'] for row, _ in keep}
        rejected.extend(hex(row['va']) for row in rows if row['va'] not in retained)
        rows, bodies = [row for row, _ in keep], [body for _, body in keep]
        targets = known | retained
    assert any(row['va'] == 0x141A601C0 and row['size'] == 89 for row in rows)
    assert set(witnessed) <= {row['va'] for row in rows}
    table = ['static const extra_entry mobile_callbacks[] = {']
    for row in sorted(rows, key=lambda row: row['va']):
        literal = '"' + ''.join('\\x'+row['raw'][i:i+2] for i in range(0, len(row['raw']), 2)) + '"'
        table.append('    {UINT64_C(0x%x),%d,%s,%s},' % (row['va'], row['size'], literal, row['name']))
    table.append('};')
    output = ROOT / 'android/host/mobile_callbacks.inc'
    output.write_text('/* Original referenced callbacks; generated by tools/generate_android_callbacks.py. */\n'
                      + '\n'.join(bodies + table) + '\n', encoding='utf-8', newline='\n')
    cases = ['/* Generated original-byte fixtures for the recovered callbacks. */',
             'static const test_case mobile_cases[] = {']
    for row in rows:
        literal = '"' + ''.join('\\x'+row['raw'][i:i+2] for i in range(0, len(row['raw']), 2)) + '"'
        cases.append('    {UINT64_C(0x%x),0,%d,%s},' % (row['va'], row['size'], literal))
    cases.append('};')
    (ROOT / 'tests/android_mobile_callbacks.h').write_text('\n'.join(cases)+'\n', encoding='utf-8')
    observed_cases = ['/* Exact witnessed LEA-RCX/tail callbacks; generated with original-byte gates. */',
                      'static const test_case observed_callback_cases[] = {']
    for target, row in sorted(witnessed.items()):
        raw = bytes.fromhex(row['original_bytes'])
        literal = '"' + ''.join('\\x%02x' % value for value in raw) + '"'
        observed_cases.append('    {UINT64_C(0x%x),UINT64_C(%s),%d,%s},' % (target, row['tail_target'], len(raw), literal))
    observed_cases.append('};')
    (ROOT / 'tests/android_observed_callbacks.h').write_text('\n'.join(observed_cases)+'\n', encoding='utf-8')
    report = dict(original_exe_sha256=config['original_exe_sha256'], recovered=rows, rejected=rejected,
                  output_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                  observed_boundary_config_sha256=hashlib.sha256(observed_path.read_bytes()).hexdigest(),
                  observed_boundaries=list(witnessed.values()))
    (ROOT / 'research/android-callback-closure.json').write_text(json.dumps(report, indent=1)+'\n')
    print(json.dumps(dict(callbacks=len(rows), rejected=len(rejected), output=str(output))))


if __name__ == '__main__':
    main()
