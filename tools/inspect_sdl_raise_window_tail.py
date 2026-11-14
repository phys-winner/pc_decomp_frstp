"""Original WIN_RaiseWindow leaf/caller proof, immutable pre-addition catalogues."""
from bisect import bisect_right
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP, X86_REG_RDI

ROOT = Path(__file__).resolve().parents[1]
HISTORY = ROOT / 'research/device-history/20261016T061712Z-cursor-display-90s'
BASE = ROOT / 'research/sdl-raise-base'
ADDRESS = 0x141B1EF20
EXACT = '488b8ac0000000488b490848ff2536881a00'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    config = json.loads((ROOT / 'port.json').read_text())
    executable = Path(config['game_directory']) / 'Frostpunk.exe'
    raw = executable.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == config['original_exe_sha256']
    paths = [Path(__file__).resolve(), ROOT / 'port.json', HISTORY / 'manifest.json',
             HISTORY / 'terminal/android-game-device.json', HISTORY / 'terminal/terminal-capture.json',
             HISTORY / 'terminal/cursor-display-terminal-native-current.log',
             HISTORY / 'android/host/win64_window.c', BASE / 'manifest.json',
             BASE / 'main-recomp-meta.json', BASE / 'supplemental-recomp-meta.json',
             BASE / 'main-functions.txt', BASE / 'supplemental-config.json',
             BASE / 'main-recomp-dispatch.c', BASE / 'dispatch-manifest.json']
    pins = {str(path): sha(path) for path in paths}
    assert pins[str(HISTORY / 'manifest.json')] == '45b75d48dcde36e93b8810de3fb1d545cec027caefaaa592f80a6d0127892617'
    stop = 'STOP missing static target=0x141b1ef20 return=0x141adf204'
    assert stop in json.loads(paths[3].read_text())['actual_stop_lines'] and stop in paths[5].read_text()
    assert json.loads(paths[4].read_text())['android_process_exit']['status'] == 71
    pe = pefile.PE(data=raw, fast_load=True)
    pe.parse_data_directories([1])
    image = pe.OPTIONAL_HEADER.ImageBase
    imports = {entry.address: module.dll.decode() + '!' + entry.name.decode()
               for module in pe.DIRECTORY_ENTRY_IMPORT for entry in module.imports if entry.name}
    assert imports[0x141CC7768] == 'USER32.dll!SetForegroundWindow'
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    ranges = [(image + begin, image + end, image + unwind)
              for begin, end, unwind in struct.iter_unpack('<III', pe.get_data(directory.VirtualAddress, directory.Size))]
    starts = [entry[0] for entry in ranges]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def read(address, count):
        value = pe.get_data(address - image, count)
        assert len(value) == count
        return value

    def record(ins):
        out = dict(va=hex(ins.address), bytes=ins.bytes.hex(), text=ins.mnemonic + ' ' + ins.op_str)
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp
                out['RIP_target'] = hex(target)
                if target in imports:
                    out['import'] = imports[target]
        return out

    def span(address, count):
        body = read(address, count)
        code = list(md.disasm(body, address))
        assert sum(ins.size for ins in code) == len(body)
        return dict(start=hex(address), end=hex(address + count), bytes=body.hex(),
                    sha256=hashlib.sha256(body).hexdigest(), instructions=[record(ins) for ins in code])

    def fragment(address):
        begin, end, unwind = ranges[bisect_right(starts, address) - 1]
        assert begin <= address < end
        return dict(**span(begin, end - begin), unwind=hex(unwind))

    before = ranges[bisect_right(starts, ADDRESS) - 1]
    after = ranges[bisect_right(starts, ADDRESS)]
    assert before[:2] == (0x141B1EEE0, ADDRESS) and after[0] == 0x141B1EF40
    assert read(ADDRESS, 18).hex() == EXACT and read(ADDRESS - 1, 1) == b'\xc3'
    assert read(ADDRESS + 18, 14) == b'\xcc' * 14
    section = pe.get_section_by_rva(ADDRESS - image)
    assert section.Name.rstrip(b'\0') == b'.text' and section.Characteristics & 0x20000000
    main_meta = json.loads((BASE / 'main-recomp-meta.json').read_text())
    main_entries = {int(address, 16) for address in re.findall(r'\{\s*(0x[0-9a-fA-F]+)ull,\s*L_', (BASE / 'main-recomp-dispatch.c').read_text())}
    assert sha(BASE / 'main-recomp-dispatch.c') == main_meta['generated_files']['recomp_dispatch.c']
    supplemental = json.loads((BASE / 'supplemental-recomp-meta.json').read_text())
    supplemental_entries = {int(entry['va'], 16) for entry in supplemental['entries']}
    assert len(main_entries) == main_meta['functions'] == 399917
    assert len(supplemental_entries) == 1984
    assert ADDRESS not in main_entries | supplemental_entries
    device = fragment(0x141AF3DB0)
    instructions = list(md.disasm(bytes.fromhex(device['bytes']), int(device['start'], 16)))
    assignments = []
    for left, right in zip(instructions, instructions[1:]):
        if left.mnemonic != 'lea' or left.op_str.split(',')[0] != 'rax' or right.mnemonic != 'mov':
            continue
        if len(left.operands) != 2 or left.operands[1].type != X86_OP_MEM or left.operands[1].mem.base != X86_REG_RIP:
            continue
        if len(right.operands) != 2 or right.operands[0].type != X86_OP_MEM or right.operands[0].mem.base != X86_REG_RDI or right.op_str.split(',')[-1].strip() != 'rax':
            continue
        target = left.address + left.size + left.operands[1].mem.disp
        coverage = 'main' if target in main_entries else 'supplemental_BASE' if target in supplemental_entries else 'missing_BASE'
        assignments.append(dict(LEA=record(left), store=record(right), slot=hex(right.operands[0].mem.disp), target=hex(target), coverage=coverage))
    counts = {name: sum(entry['coverage'] == name for entry in assignments) for name in ['main', 'supplemental_BASE', 'missing_BASE']}
    assert len(assignments) == 53 and counts == dict(main=49, supplemental_BASE=2, missing_BASE=2)
    assert [(entry['slot'], entry['target']) for entry in assignments if entry['coverage'] == 'missing_BASE'] == [('0xb0', '0x141b1ef20'), ('0x188', '0x141b223b0')]
    dyninit = span(0x141AEB8DF, 14)
    assert dyninit['instructions'][0]['RIP_target'] == '0x141b08d60'
    assert dyninit['instructions'][1]['RIP_target'] == '0x14289f118'
    thunk = span(0x141AED270, 7)
    assert thunk['instructions'][0]['RIP_target'] == '0x14289f118'
    raise_real = span(0x141B08D60, 73)
    assert raise_real['instructions'][-1]['text'] == 'ret '
    caller = fragment(0x141ADF1FF)
    assert any(entry['va'] == '0x141adf1ff' and entry['text'] == 'call 0x141aed270' for entry in caller['instructions'])
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), producer='tools/inspect_sdl_raise_window_tail.py',
                  original_exe_sha256=config['original_exe_sha256'], va=hex(ADDRESS), bytes=EXACT,
                  IAT_slot='0x141cc7768', import_dll='USER32.dll', import_name='SetForegroundWindow',
                  inputs_pre_post_sha256=pins, actual_terminal=dict(STOP=stop, status=71, sealed_manifest_sha256=pins[str(HISTORY/'manifest.json')]),
                  original_body=span(ADDRESS, 18),
                  original_boundary=dict(inside_text=True, entry_has_pdata=False, preceding_pdata=[hex(value) for value in before],
                                         next_pdata=[hex(value) for value in after], preceding_byte='c3', trailing_CC_bytes=14),
                  BASE_catalogue=dict(main_entries=len(main_entries), supplemental_entries=len(supplemental_entries), main_metadata_sha256=sha(BASE/'main-recomp-meta.json'), supplemental_metadata_sha256=sha(BASE/'supplemental-recomp-meta.json'), target_absent=True),
                  controller_caller=caller, SDL_dynapi_initializer=dyninit, SDL_dynapi_thunk=thunk,
                  REAL_SDL_RaiseWindow=raise_real, Windows_CreateDevice=device,
                  callback_closure=dict(total=53, counts=counts, assignments=assignments,
                                        other_missing='slot188 WIN_GL_SwapWindow141B223B0 is OpenGL-conditional; not part of this D3D RaiseWindow fix'),
                  semantics=[
                      '18originalbytes containonlytwoMOVloads andindirectIATtailJMP. RCX=[RDX+C0] SDLwindow.driverdata, thenRCX=[driverdata+8] guestHWND; tailtarget isloadedfromoriginalIAT atcalltime.',
                      'Thereisno newreturnaddresspush, no localstackadjustment, no flaginstruction. AllotherCPUstate mustremainunchanged before commonimportdispatch; importsinglepop returnsdirectlytoexistingguestcaller.',
                      'CreateDevice storesleaf intoSDLdevice+B0. REALSDL_RaiseWindow validatesdevice/window/magic andSDL_SHOWNbit4, thenloads+B0; ifnonNULL itmovesSDLwindowRCXtoRDX,deviceR8toRCX,andtailjumps.',
                      'Actualcontroller CALL141ADF1FF pushesreturn141ADF204, reachesdynapi141AED270→slot14289F118 initializedtoREAL141B08D60; selectedrealcallback reachesobservedleaf. Return141ADF204 isthereforeavalidCALLreturn, unlikepriorLuaGCinteriorarm case.',
                      'ProviderSetForegroundWindow alreadyexists inretainedWindow02e12; missingstaticAOTentryprecedesactualimportdispatch. Thisfixaddsoriginalleafsemantics, not an SDL/API bypass.',
                      'Theold1984supplementBASEandmain399917cataloguesomitthisleaf. Its precedingbyteC3 failsaCC-beforeentryshortleafeligibilityrule; genericindirectIATtail wasoutsideoldsealedgrammar. Newboundedexactentrydoesnotjustifychangingthatgrammar.'],
                  limits=['Originalbytes/staticcatalogueaudit only; originalCPUbytesnotexecuted, no new device command.',
                          'Controllerraisebranch isconditional in static code; actualSTOPprovesitselected onthisrun, notthatallpossiblelaunchesmustselectit.',
                          '53callbackcoverageusesimmutableBASEcatalogues; futureAOTadditiondoesnotinvalidatehistoricalmissingannotation.',
                          'No OpenGLcallbackimplementation, no physicalforegroundactivationoracle or gameframe/menuclaim.'])
    assert sha(executable) == config['original_exe_sha256'] and all(sha(path) == digest for path, digest in pins.items())
    target = ROOT / 'research/sdl-raise-window-tail.json'
    target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf8')
    print(json.dumps(dict(report=str(target.relative_to(ROOT)), sha256=sha(target), producer_sha256=sha(Path(__file__)), closure=counts), indent=2))


if __name__ == '__main__':
    main()
