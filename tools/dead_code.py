"""Lifted library code that is unreachable or replaced by native code, left out of the runtime.

    python -X utf8 -B tools/dead_code.py [--check]

OpenAL Soft (Frostpunk.exe links it statically, 0x141BABD20-0x141C02DD0): every public AL/ALC
entry is served by the native OpenAL (android/host/win64_openal.c: 30 typed routes, every other
entry stops) and the dispatcher always offers the entry range to that provider first
(bootstrap.c dispatch_resolved), so the library's own bodies are unreachable.  Three of its
functions are called by the image outside any entry (a CRT initializer and two TLS callbacks)
and only touch OpenAL Soft's own state: android/host/openal_static_removed.c answers them.

Lua 5.1.1 (the core, 0x141C097A0-0x141C22410): android/host/lua_native.c runs the unmodified
5.1.1 sources natively and defines the 58 C API functions that the engine and tolua++ call as
strong L_<VA> symbols (the API(...) lines of that file are the list).

Both checked in IDA (research/ida/Frostpunk.exe.i64, 2026-11-18): no code outside a range calls a
function inside it other than its entries, and outside data refers to the range only through the
export table, .pdata/.xdata, the initializer and TLS callback lists (OpenAL) or the library's own
tables (Lua).  No fastlift unit outside the ranges references a non-entry symbol inside them.

Outputs (content-addressed, build/dead-code/):
- recomp_dispatch.o: generated/frostpunk-x64/recomp_dispatch.c without the removed entries; the
  three OpenAL keepers are bound to the stubs, the Lua API entries keep their L_<VA> names (the
  native definitions).  Same recipe as build/arm64/objects/recomp_dispatch.o (--check proves the
  recipe by rebuilding the unfiltered table byte for byte);
- recomp_funcs_0977..0984.o: the fastlift units that hold the two libraries, compiled with
  -ffunction-sections so --gc-sections drops every body nothing references any more.  The Lua
  API symbols they define become weak (the native ones win).  Unit 0982 also carries the CRC
  entry of tools/build_native_crc.py: its symbol becomes weak with the alias
  fp_original_L_000141C04BB0, which is what that tool's routing produced.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import fastbuild as fb  # noqa: E402

OUT = ROOT / 'build/dead-code'
TABLE = ROOT / 'generated/frostpunk-x64/recomp_dispatch.c'
TABLE_OBJECT = 'build/arm64/objects/recomp_dispatch.o'
OPENAL = (0x141BABD20, 0x141C02DD0)
KEEP = {0x141BB0D70: 'fp_openal_static_constructor', 0x141BD2780: 'fp_openal_static_tls_callback',
        0x141BD2760: 'fp_openal_static_tls_callback'}
LUA = (0x141C097A0, 0x141C22410)
LUA_SOURCE = ROOT / 'android/host/lua_native.c'
# Native replacements of lifted library entries: source -> number of strong L_<VA> it defines.
# Lua replaces its whole core (the rest of the range goes); Theora replaces the eleven legacy
# decoder entries and keeps the lifted bodies for the encoder and the verification mode.
# vorbisfile replaces the three entries of the engine's OGGStream; the low-level libvorbis stays
# lifted for the audio track of the videos.
NATIVE = {'android/host/lua_native.c': 58, 'android/host/theora_native.c': 11, 'android/host/vorbisfile_native.c': 3}
INCLUDES = ['reference/lua-5.1.1/src', 'reference/libtheora-1.2.0alpha1/include', 'android/host/ogg_config',
            'reference/libogg-1.3.5/include', 'reference/libvorbis-1.3.7/include']
# libvorbis 1.3.7 without the encoder setup (vorbisenc.c) and the tools; libogg 1.3.5.
LIBRARY_SOURCES = (['reference/libvorbis-1.3.7/lib/%s.c' % n for n in (
    'analysis', 'bitrate', 'block', 'codebook', 'envelope', 'floor0', 'floor1', 'info', 'lookup', 'lpc', 'mapping0',
    'psy', 'registry', 'res0', 'sharedbook', 'smallft', 'synthesis', 'vorbisfile', 'window')] +
    ['android/host/vorbis_msvc_pi.c',   # mdct.c and lsp.c with the float M_PI of the game's MSVC build
     'reference/libogg-1.3.5/src/framing.c', 'reference/libogg-1.3.5/src/bitwise.c'])
SECTION_UNITS = (977, 978, 979, 980, 981, 982, 983, 984)
CRC_UNIT, CRC_SYMBOL = 982, 'L_000141C04BB0'
HOST = ['android/host/openal_static_removed.c', *NATIVE, *LIBRARY_SOURCES]
TABLE_FLAGS = ['--target=aarch64-linux-android30', '-O2', '-std=c11', '-fPIC', '-fvisibility=hidden', '-fno-strict-aliasing',
               '-fwrapv', '-ffp-contract=off', '-fno-math-errno', '-g0', '-w',
               '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'generated/frostpunk-x64')]


def sha(data):
    return hashlib.sha256(data if isinstance(data, bytes) else Path(data).read_bytes()).hexdigest()


def native_entries():
    """{source: VAs} of the strong L_<VA> definitions in the NATIVE host sources."""
    out = {}
    for source, expected in NATIVE.items():
        text = (ROOT / source).read_text(encoding='utf-8')
        found = {int(m, 16) for m in re.findall(r'^API\((0001[0-9A-F]{8}),', text, re.M)}
        found |= {int(m, 16) for m in re.findall(r'^void L_(0001[0-9A-F]{8})\(CPU \*c\)', text, re.M)}
        if len(found) != expected:
            raise RuntimeError('%s defines %d entries, expected %d' % (source, len(found), expected))
        out[source] = found
    if any(not (LUA[0] <= va < LUA[1]) for va in out['android/host/lua_native.c']):
        raise RuntimeError('a Lua entry outside the core range')
    return out


def lua_api():
    return native_entries()['android/host/lua_native.c']


def units_defining(vas):
    """{unit: set(VA)} of the fastlift units that define these symbols."""
    pattern = re.compile(r'^void L_([0-9A-F]{12})\(CPU \*restrict c\)$', re.M)
    out = {}
    for source in sorted((ROOT / 'build/fastlift/src').glob('recomp_funcs_*.c')):
        hits = {int(m, 16) for m in pattern.findall(source.read_text(encoding='utf-8', errors='replace'))} & vas
        if hits:
            out[int(source.stem[-4:])] = hits
    missing = vas - set().union(*out.values()) if out else vas
    if missing:
        raise RuntimeError('no fastlift unit defines %s' % sorted(hex(v) for v in missing)[:8])
    return out


def compile_c(text, target, flags):
    target.parent.mkdir(parents=True, exist_ok=True)
    source = target.with_suffix('.c')
    source.write_bytes(text)
    done = subprocess.run([str(fb.BIN / 'clang.exe'), *flags, '-c', str(source), '-o', str(target)], capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError('compile failed: %s\n%s' % (source, done.stderr[-4000:]))
    return target


def filtered_table(api):
    text = TABLE.read_bytes()
    entry = re.compile(rb'^    \{ 0x([0-9A-F]+)ull, (L_[0-9A-F]{12}) \},\r?\n', re.M)
    removed = {'openal': 0, 'lua': 0}
    kept = 0

    def one(m):
        nonlocal kept
        va = int(m.group(1), 16)
        if OPENAL[0] <= va < OPENAL[1]:
            if va in KEEP:
                kept += 1
                return m.group(0).replace(m.group(2), KEEP[va].encode())
            removed['openal'] += 1
            return b''
        if LUA[0] <= va < LUA[1] and va not in api:
            removed['lua'] += 1
            return b''
        return m.group(0)
    out = entry.sub(one, text)
    if kept != len(KEEP):
        raise RuntimeError('kept entries %d, expected %d' % (kept, len(KEEP)))
    count = re.search(rb'recomp_dispatch_count = (\d+);', out)
    total = len(entry.findall(out)) + kept   # the kept entries now name the stubs
    if not count or int(count.group(1)) != total + sum(removed.values()):
        raise RuntimeError('unexpected recomp_dispatch_count')
    out = out.replace(count.group(0), b'recomp_dispatch_count = %d;' % total)
    prototypes = b''.join(b'void %s(CPU *c);\n' % n.encode() for n in sorted(set(KEEP.values())))
    out = re.sub(rb'(#include "recomp_dispatch.h"\r?\n)', lambda m: m.group(1) + prototypes, out, count=1)
    if prototypes not in out:
        raise RuntimeError('no place for the stub prototypes')
    return out, removed


def unit_objects(api):
    """api: every VA defined natively.  Each unit that holds a removed library or defines one of
    them is recompiled with function sections; the natively defined symbols become weak, with
    fp_original_ aliases for the bodies (used by verification modes)."""
    import build_rdvk
    import fastlift
    flags = [*fastlift.CFLAGS, '-ffunction-sections', '-I' + str(ROOT / 'build/fastlift/include'),
             '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host')]
    out = {}
    for unit in sorted(set(SECTION_UNITS) | set(units_defining(api))):
        source = ROOT / 'build/fastlift/src' / ('recomp_funcs_%04d.c' % unit)
        key = sha((sha(source) + json.dumps(flags) + json.dumps(sorted(api))).encode())[:16]
        folder = OUT / 'units' / key
        compiled = folder / 'compiled' / ('recomp_funcs_%04d.o' % unit)   # the object name stays the unit's
        target = folder / ('recomp_funcs_%04d.o' % unit)
        if not target.exists():
            compiled.parent.mkdir(parents=True, exist_ok=True)
            done = subprocess.run([str(fb.BIN / 'clang.exe'), *flags, '-c', str(source), '-o', str(compiled)], capture_output=True, text=True)
            if done.returncode:
                raise RuntimeError('compile failed: %s\n%s' % (source, done.stderr[-4000:]))
            syms = build_rdvk.symbols_of(compiled)
            args = []
            for va in sorted(api):
                name = 'L_%012X' % va
                if name in syms:
                    value, section, _, visibility = syms[name]
                    args += ['--weaken-symbol=' + name,
                             '--add-symbol=fp_original_%s=%s:0x%x,global,function,%s' % (name, section, value,
                                                                                        'hidden' if visibility == 'HIDDEN' else 'default')]
            if unit == CRC_UNIT:
                value, section, _, visibility = syms[CRC_SYMBOL]
                args += ['--weaken-symbol=' + CRC_SYMBOL,
                         '--add-symbol=fp_original_%s=%s:0x%x,global,function,%s' % (CRC_SYMBOL, section, value,
                                                                                    'hidden' if visibility == 'HIDDEN' else 'default')]
            if args:
                subprocess.run([str(fb.BIN / 'llvm-objcopy.exe'), *args, str(compiled), str(target)], check=True)
            else:
                target.write_bytes(compiled.read_bytes())
            check = build_rdvk.symbols_of(target)
            for va in api:
                name = 'L_%012X' % va
                if name in syms and check[name][2] != 'WEAK':
                    raise RuntimeError('%s did not become weak in unit %d' % (name, unit))
        out[unit] = target
    return out


def objects(check=False):
    """{'table', 'units', 'host', 'include', 'crc_routed'} for the runtime link."""
    if check:
        plain = compile_c(TABLE.read_bytes(), OUT / 'check/recomp_dispatch.o', TABLE_FLAGS)
        if sha(plain) != sha(ROOT / TABLE_OBJECT):
            raise RuntimeError('recipe does not rebuild %s' % TABLE_OBJECT)
        print('table recipe reproduces', TABLE_OBJECT, flush=True)
    native = native_entries()
    api = set().union(*native.values())
    text, removed = filtered_table(native['android/host/lua_native.c'])
    table = OUT / 'table' / sha(text)[:16] / 'recomp_dispatch.o'
    if not table.exists():
        compile_c(text, table, TABLE_FLAGS)
    units = unit_objects(api)
    import build_native_crc
    report = dict(openal_range=[hex(v) for v in OPENAL], lua_range=[hex(v) for v in LUA], removed_entries=removed,
                  kept=sorted(hex(v) for v in KEEP), native={s: len(v) for s, v in native.items()}, table=str(table.relative_to(ROOT)),
                  units={u: str(p.relative_to(ROOT)) for u, p in units.items()})
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'manifest.json').write_text(json.dumps(report, indent=1) + '\n')
    print('dead code: dispatch entries removed %s; native entries %s; units %s with function sections'
          % (removed, {Path(s).stem: len(v) for s, v in native.items()}, list(units)), flush=True)
    return dict(table=table, units=units, host=HOST, includes=INCLUDES, crc_routed=build_native_crc.ROUTED_UNIT)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true')
    objects(ap.parse_args().check)
