"""Original waveOut count/caps monitor, reset branches and direct API census."""
import bisect
import hashlib
import json
from pathlib import Path
import re
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).resolve().parents[1]
TERMINAL = 'research/device-history/20261016T013104Z-shared-stl-90s/terminal/'


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    cfg = json.loads((ROOT/'port.json').read_text())
    path = Path(cfg['game_directory'])/'Frostpunk.exe'
    before = path.stat()
    raw = path.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == cfg['original_exe_sha256']
    p = pefile.PE(data=raw, fast_load=True)
    p.parse_data_directories(directories=[1])
    b = p.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    imports = {i.address: d.dll.decode()+'!'+i.name.decode() for d in p.DIRECTORY_ENTRY_IMPORT for i in d.imports if i.name}
    winmm = {a: n for a, n in imports.items() if n.lower().startswith('winmm.dll!')}
    assert winmm[0x141CC7AD0] == 'WINMM.dll!waveOutGetNumDevs'
    assert winmm[0x141CC7AD8] == 'WINMM.dll!waveOutGetDevCapsW'
    dd = p.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    ranges = sorted((b+a, b+z, b+u) for a, z, u in struct.iter_unpack('<III', p.get_data(dd.VirtualAddress, dd.Size)))
    starts = [a for a, z, u in ranges]
    cache = {}

    def read(a, n):
        value = p.get_data(a-b, n)
        assert len(value) == n
        return value

    def row(i):
        r = dict(va=hex(i.address), bytes=i.bytes.hex(), mnemonic=i.mnemonic, operands=i.op_str)
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                target = i.address+i.size+op.mem.disp
                r['RIP_target'] = hex(target)
                if target in imports:
                    r['import'] = imports[target]
        return r

    def containing(pc):
        n = bisect.bisect_right(starts, pc)-1
        if n < 0:
            return None
        a, z, u = ranges[n]
        if not a <= pc < z:
            return None
        if a not in cache:
            ins = list(md.disasm(read(a, z-a), a))
            assert sum(i.size for i in ins) == z-a
            cache[a] = (z, u, ins)
        return a, cache[a]

    census, callers, unresolved_refs = [], [], []
    for s in p.sections:
        if not s.Characteristics & 0x20000000:
            continue
        data, va = s.get_data(), b+s.VirtualAddress
        for match in re.finditer(rb'\xff[\x15\x25]', data):
            off = match.start()
            if off+6 > len(data):
                continue
            target = va+off+6+struct.unpack_from('<i', data, off+2)[0]
            if target not in winmm:
                continue
            found = containing(va+off)
            if found:
                a, (z, u, ins) = found
                # Include a possible REX prefix before FF15/FF25; the disp32
                # still ends at the actual instruction end.
                i = next((i for i in ins if i.address <= va+off and i.address+i.size == va+off+6
                          and i.mnemonic in ['call', 'jmp']), None)
                if i is not None:
                    census.append(dict(instruction=row(i), fragment_start=hex(a), fragment_end=hex(z),
                                       embedded_OpenAL_neighborhood=0x141BABD20 <= i.address < 0x141BD5000))
                else:
                    unresolved_refs.append(dict(raw_FF_site=hex(va+off), import_slot=hex(target), reason='Not decoded call/jump boundary in containing pdata'))
            else:
                unresolved_refs.append(dict(raw_FF_site=hex(va+off), import_slot=hex(target), reason='Outside pdata; not claimed as a validated caller'))
        for match in re.finditer(rb'[\xe8\xe9]', data):
            off = match.start()
            if off+5 > len(data) or va+off+5+struct.unpack_from('<i', data, off+1)[0] != 0x140F33110:
                continue
            found = containing(va+off)
            if found:
                a, (z, u, ins) = found
                i = next((i for i in ins if i.address == va+off), None)
                if i is not None:
                    callers.append(row(i))
    assert [x['va'] for x in callers] == ['0x140f31aee', '0x141003b98']
    fragments = {}
    selected = [(a, z, u) for a, z, u in ranges if
                0x140F33110 <= a < 0x140F3339F or 0x140F32CA0 <= a < 0x140F3310E or
                0x140F31AC0 <= a < 0x140F31C9A or a == 0x140E27CF0 or a == 0x141003B70]
    for a, z, u in selected:
        found = containing(a)
        instructions = found[1][2]
        fragments[hex(a)] = dict(start=hex(a), end=hex(z), unwind=hex(u), sha256=hashlib.sha256(read(a, z-a)).hexdigest(), instructions=[row(i) for i in instructions])
    assert read(0x141D6D090+7*8, 8) == struct.pack('<Q', 0x140F31AC0)
    terminal_path = ROOT/(TERMINAL+'android-game-device.json')
    terminal = json.loads(terminal_path.read_text())
    stop = 'STOP unimplemented import WINMM.dll!waveOutGetNumDevs slot=0x141cc7ad0 return=0x140f33141 dispatch_calls=0'
    assert any('WINMM.dll!waveOutGetNumDevs' in x and 'return=0x140f33141' in x for x in terminal['actual_stop_lines'])
    inputs = [str(Path(__file__).relative_to(ROOT)), TERMINAL+'android-game-device.json', TERMINAL+'native-current-segment.log',
              'research/winmm-query-oracle.json', 'tools/oracle_winmm_queries.py']
    pins = {name: sha(ROOT/name) for name in inputs}
    report = dict(producer=str(Path(__file__).relative_to(ROOT)), inputs_pre_post_sha256=pins,
                  original_exe=str(path), original_exe_sha256=cfg['original_exe_sha256'],
                  scope='Read-only original bytes/fragment boundaries and local native query evidence. No production, device, playback or endpoint emulation.',
                  actual=dict(receipt=TERMINAL+'android-game-device.json', PID=22889, start_ticks=17314909,
                              API='WINMM.dll!waveOutGetNumDevs', slot='0x141cc7ad0', call='0x140f3313b', return_pc='0x140f33141'),
                  gate='140F3312B checks object+1B6; zero skips monitor. Original_Init sets+1B6=1 at140F31307.',
                  zero_path=['TEST EAX at33141; JE33335 when count0.',
                             '33335 tests object+190 OpenALdevice; NULL returns33315.',
                             'Existingdevice: if+1A8=0,33362 calls32CA0. If+1A8!=0,3334B/33353/3335B call32D10/32CA0/32F30.',
                             '32CA0 clears source state, destroys context(+198), closes device(+190), zerosboth, then tailcalls_Init140F311F0 withEDX0. This is a real reset/reopen, not a harmless diagnostic skip.',
                             '32D10 clears+1A8, makescontextcurrent/processes it and resumes sources;32F30 sets+1A8, clearscurrentcontext and suspendscontext. These preserve prior suspension around reset.'],
                  nonzero_path=['33149 setsR8D84;3314F RDX=stack+30;33154 RCX0;33156 callswaveOutGetDevCapsW IAT141CC7AD8.',
                                'NonzeroMMRESULT3315E jumps33315: returns without reset.',
                                'Success reads WCHARdevice name atcaps+8(stack+38), initializing cachedownedwstring142A8A798 iflength142A8A7A0=0.',
                                '3325E checks OpenALdevice; absent→reset. Existingdevice and exact UTF16name equality loop33284..332A1→return3330D.',
                                'Device-name mismatch332AA clears cachedname, resetsOpenAL via32CA0 and preserves prior+1A8 suspension. No waveOutOpen/playback call in this monitor.'],
                  frequency=dict(direct_callers=callers, worker='140F31AC0 increments+130; basecommand processing31ACF; unless+1B5flag set,33110 called31AEE BEFOREelapsed-update throttle31B36.',
                                 outer_loop='140E27CF0 calls vtable+38 at27D9B; SoundEnginevtable141D6D090 slot7=140F31AC0. Loops27DA8→27D10 until+100exitpointer, withMsgWait timeout+118. Therefore recurringworkerpoll, not only OS device-change notification.',
                                 consequence='If count0 is reported while nativeOpenALdevice repeatedly opens successfully, reset/reopen can recur on normal worker iterations. Count0 cannot be sold as preserving sound.'),
                  caps_ABI=dict(WAVEOUTCAPSA_bytes=52, WAVEOUTCAPSW_bytes=84, name_offset=8, name_capacity=32,
                                W_offsets=dict(wMid=0,wPid=2,vDriverVersion=4,szPname=8,dwFormats=72,wChannels=76,wReserved1=78,dwSupport=80)),
                  imports=[dict(slot=hex(a),API=n) for a,n in sorted(winmm.items())], validated_direct_WinMM_calls=census,
                  unresolved_raw_WinMM_references=unresolved_refs,
                  fragments=fragments,
                  limits=['Nonzero native Windows count/caps are not Android endpoint evidence; no fabricated Android count/capability/name is authorized.',
                          'Embedded OpenAL WinMM calls are listed separately by address neighborhood and are not proven reached through the active native provider.',
                          'No global fake zero-success stub or unconditional count1 is proposed. A genuine provider/profile needs an explicit platform contract.'])
    assert all(sha(ROOT/n)==d for n,d in pins.items())
    assert (before.st_size,before.st_mtime_ns)==(path.stat().st_size,path.stat().st_mtime_ns)
    target=ROOT/'research/winmm-monitor-callers.json'
    target.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(report=str(target.relative_to(ROOT)),sha256=sha(target),fragments=len(fragments),direct_WinMM_calls=len(census),WinMMimports=len(winmm)),indent=2))


if __name__ == '__main__':
    main()
