"""Read-only native Windows waveOut count/caps ABI oracle; never opens audio."""
import ctypes as C
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    class CapsA(C.Structure):
        _fields_ = [('wMid', C.c_uint16), ('wPid', C.c_uint16), ('vDriverVersion', C.c_uint32),
                    ('szPname', C.c_char*32), ('dwFormats', C.c_uint32), ('wChannels', C.c_uint16),
                    ('wReserved1', C.c_uint16), ('dwSupport', C.c_uint32)]

    class CapsW(C.Structure):
        _fields_ = [('wMid', C.c_uint16), ('wPid', C.c_uint16), ('vDriverVersion', C.c_uint32),
                    ('szPname', C.c_uint16*32), ('dwFormats', C.c_uint32), ('wChannels', C.c_uint16),
                    ('wReserved1', C.c_uint16), ('dwSupport', C.c_uint32)]

    assert C.sizeof(CapsA) == 52 and C.sizeof(CapsW) == 84
    assert CapsA.szPname.offset == CapsW.szPname.offset == 8
    files = [Path(__file__).resolve(), Path('C:/Windows/System32/winmm.dll'),
             Path('C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/um/mmeapi.h')]
    pins = {str(p): sha(p) for p in files}
    lib = C.WinDLL('winmm.dll', use_last_error=True)
    count = lib.waveOutGetNumDevs
    count.argtypes = []
    count.restype = C.c_uint32
    C.set_last_error(0x5a17)
    initial_count = count()
    count_error = C.get_last_error()
    rows = []
    # Valid writable storage is always large enough for the declared size.
    # No NULL/invalid-pointer call, waveOutOpen, playback or global OS setter.
    ids = sorted({0, initial_count+17, (1 << (C.sizeof(C.c_void_p)*8))-1})
    for suffix, cls in [('A', CapsA), ('W', CapsW)]:
        fn = getattr(lib, 'waveOutGetDevCaps'+suffix)
        fn.argtypes = [C.c_size_t, C.c_void_p, C.c_uint32]
        fn.restype = C.c_uint32
        size = C.sizeof(cls)
        for device in ids:
            for cb in [0, 1, size-1, size, size+1]:
                buffer = (C.c_ubyte*256)(*([0xa5]*256))
                C.set_last_error(0x5a17)
                result = fn(device, C.byref(buffer, 16), cb)
                error = C.get_last_error()
                data = bytes(buffer)
                assert data[:16] == bytes([0xa5])*16 and data[144:] == bytes([0xa5])*112
                decoded = None
                if not result and cb >= size:
                    value = cls.from_buffer_copy(data[16:16+size])
                    decoded = {n: getattr(value, n) for n in ['wMid', 'wPid', 'vDriverVersion', 'dwFormats', 'wChannels', 'wReserved1', 'dwSupport']}
                    decoded['szPname'] = bytes(value.szPname).split(b'\0')[0].decode('mbcs', errors='replace') if suffix == 'A' else bytes(value.szPname).decode('utf-16-le', errors='surrogatepass').split('\0')[0]
                rows.append(dict(API='waveOutGetDevCaps'+suffix, device=hex(device), cb=cb, MMRESULT=result,
                                 LastError=error, initialized_byte=0xa5, full_buffer_hex=data.hex(), decoded=decoded,
                                 changed_relative_offsets=[n for n, v in enumerate(data[16:144]) if v != 0xa5]))
    final_count = count()
    assert all(sha(p) == d for p, d in pins.items())
    report = dict(recorded_utc=datetime.now(timezone.utc).isoformat(), producer=str(Path(__file__).relative_to(ROOT)),
                  inputs_pre_post_sha256=pins, native_only=True, phone_used=False, original_game_executed=False,
                  audio_opened=False, OS_settings_modified=False,
                  layouts={name: dict(size=C.sizeof(cls), offsets={n: getattr(cls, n).offset for n, _ in cls._fields_})
                           for name, cls in [('WAVEOUTCAPSA', CapsA), ('WAVEOUTCAPSW', CapsW)]},
                  count=dict(result=initial_count, LastError=count_error, final_result=final_count), rows=rows,
                  limits=['Native machine endpoint count/names are measurements of Windows only, not Android endpoint evidence.',
                          'Zero-count guest behavior must be derived from original control flow; this oracle does not authorize a fake zero/one endpoint.',
                          'Only valid writable caps buffers were queried. No native invalid-pointer parity claim.'])
    path = ROOT/'research/winmm-query-oracle.json'
    path.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(report=str(path.relative_to(ROOT)), sha256=sha(path), native_count=initial_count,
                          cases=len(rows), capsA_bytes=52, capsW_bytes=84), indent=2))


if __name__ == '__main__':
    main()
