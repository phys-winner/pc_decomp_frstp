"""Statically lift the supplied Frostpunk wrapper entry up to a strict host boundary.

This deliberately preserves the PE entry; it does not select the decoded OEP.
The two ranges are hash-pinned bring-up inputs, not a complete game catalogue.
"""
import argparse
import bisect
import hashlib
import json
from pathlib import Path
import re
import sys

import reference64
from leaf_probe import prepare_runtime

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated/startup"
RANGES = ((0x144506310, 0x60), (0x1445063D0, 0x35A8))
CRT_ENTRY = 0x141AE13E0
TLS_CALLBACKS = (0x141BD2780, 0x141AE10D0, 0x141BD2760, 0x141AE0F94)


def crt_closure(lifter, reader, generate64, depth):
    """Bounded direct-target closure: unwind extents plus proven FF25 IAT thunks."""
    bounds = generate64.load_bounds(ROOT / "config/Frostpunk.pdata.functions.txt")
    starts = sorted(bounds)
    # This actual frameless TLS leaf has no .pdata record: TEST/JNE/LEA/JMP/RET.
    explicit_leaves = {0x141BD2760: 0x11}
    discovered, pending, unresolved = {}, {CRT_ENTRY, *TLS_CALLBACKS}, set()
    target_pattern = re.compile(r'dispatch(?:_jmp)?\(c, 0x([0-9A-Fa-f]+)ull\)')
    for _ in range(depth + 1):
        following = set()
        for va in sorted(pending):
            if va in discovered:
                continue
            index = bisect.bisect_right(starts, va) - 1
            begin = starts[index] if index >= 0 else None
            if va in explicit_leaves:
                size = explicit_leaves[va]
            elif begin is not None and va < begin + bounds[begin][0]:
                size = begin + bounds[begin][0] - va
            elif reader(va, 2) == b"\xff\x25":
                size = 6
            else:
                unresolved.add(va)
                continue
            if len(discovered) >= 128:
                raise ValueError("Bounded CRT closure exceeds 128 entries; use the full catalog")
            body = lifter.lift_function(reader(va, size), va, name=f"L_{va:012X}")
            discovered[va] = size
            following.update(int(target, 16) for target in target_pattern.findall(body))
        pending = following - discovered.keys()
    unresolved.update(pending)
    return discovered, sorted(unresolved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--crt", action="store_true", help="Add bounded original CRT diagnostic entries")
    parser.add_argument("--crt-depth", type=int, default=2, help="Direct-target CRT closure depth (default 2)")
    args = parser.parse_args()
    if not 0 <= args.crt_depth <= 5:
        parser.error("--crt-depth must be in 0..5")
    config = json.loads((ROOT / "port.json").read_text())
    exe = Path(config["game_directory"]) / "Frostpunk.exe"
    raw = exe.read_bytes()
    if hashlib.sha256(raw).hexdigest() != config["original_exe_sha256"]:
        raise ValueError("Original executable changed; rederive wrapper ranges")
    reference64.prepare_reference()
    if not all((ROOT / "native/runtime64" / name).exists() for name in ("cpu64.h", "eh64.c", "eh64.h")):
        prepare_runtime()
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    from lift64_checked import CheckedLifter, ORIGINAL_SHA256, BIND_RANGES, self_test
    if config["original_exe_sha256"] != ORIGINAL_SHA256 or RANGES != BIND_RANGES:
        raise ValueError("The project wrapper rules require their exact original executable")
    self_test()

    data, base, image_size, entry, sections, imp_rva, *_ = generate64.read_pe(exe)
    if base + entry != RANGES[0][0]:
        raise ValueError("Pinned startup ranges do not match the supplied PE entry")
    read = generate64.make_reader(data, base, sections)
    imports = generate64.parse_imports(data, base, sections, imp_rva)
    lifter = CheckedLifter(image_size=image_size, read_va=read, image_base=base,
                           original_sha256=ORIGINAL_SHA256)
    ranges, unresolved = dict(RANGES), []
    if args.crt:
        extra, unresolved = crt_closure(lifter, read, generate64, args.crt_depth)
        ranges.update(extra)
    ranges = sorted(ranges.items())
    OUT.mkdir(parents=True, exist_ok=True)
    # Remove completion marker before emission; builders must require it.
    meta_path = OUT / "recomp_meta.json"
    meta_path.unlink(missing_ok=True)
    bodies = [lifter.lift_function(read(va, size), va, name=f"L_{va:012X}") for va, size in ranges]
    call_next = lifter.lift_function(bytes.fromhex("e80000000058c3"), RANGES[0][0],
                                    name="frostpunk_call_next_test")
    assert "dispatch(c," not in call_next and "push64(c, 0x144506315ull);" in call_next
    assert "dispatch_jmp(c, target)" in bodies[[va for va, _ in ranges].index(RANGES[0][0])]
    ordinary_call = lifter.lift_function(bytes.fromhex("e801000000c3c3"), RANGES[0][0])
    assert "dispatch(c, 0x144506316ull)" in ordinary_call
    unproven_site = lifter.lift_function(bytes.fromhex("e800000000c3"), RANGES[0][0] + 0x100)
    assert "dispatch(c, 0x144506415ull)" in unproven_site
    header = '#pragma once\n#include "cpu64.h"\n'
    header += "".join(f"void L_{va:012X}(CPU *c);\n" for va, _ in ranges)
    header += "void frostpunk_call_next_test(CPU *c);\n"
    (OUT / "recomp_funcs.h").write_text(header, encoding="utf-8")
    (OUT / "recomp_funcs.c").write_text('#include "recomp_funcs.h"\n' + "\n\n".join(bodies + [call_next]) + "\n",
                                          encoding="utf-8")
    (OUT / "recomp_dispatch.h").write_text(
        '#pragma once\n#include "cpu64.h"\n'
        'typedef struct { uint64_t va; void (*fn)(CPU *); } recomp_entry_t;\n'
        'extern const recomp_entry_t recomp_dispatch_table[];\n'
        'extern const uint32_t recomp_dispatch_count;\n'
        'typedef struct { uint64_t slot; const char *dll; const char *name; } hle_import_t;\n'
        'extern const hle_import_t recomp_imports[];\n'
        f'#define HLE_BASE 0x{generate64.HLE_BASE:X}ull\n'
        f'#define HLE_STRIDE {generate64.HLE_STRIDE}\n'
        f'#define HLE_COUNT {len(imports)}\n'
        '#define HLE_SENTINEL(i) (HLE_BASE + (uint64_t)(i) * HLE_STRIDE)\n', encoding="utf-8")
    (OUT / "recomp_dispatch.c").write_text(
        '#include "recomp_funcs.h"\n#include "recomp_dispatch.h"\n'
        'const recomp_entry_t recomp_dispatch_table[] = {\n' +
        "".join(f'    {{0x{va:X}ull, L_{va:012X}}},\n' for va, _ in ranges) + '};\n'
        f'const uint32_t recomp_dispatch_count = {len(ranges)};\n', encoding="utf-8")
    (OUT / "recomp_imports.c").write_text(
        '#include "recomp_dispatch.h"\nconst hle_import_t recomp_imports[] = {\n' +
        "".join(f'    {{0x{slot:X}ull, {json.dumps(dll)}, {json.dumps(name)}}},\n'
                for slot, (dll, name) in sorted(imports.items())) + '};\n', encoding="utf-8")
    todo_lines = [line.strip() for body in bodies for line in body.splitlines() if "RECOMP_TODO(" in line]
    report = {
        "original_exe_sha256": config["original_exe_sha256"],
        "image_base": base, "size_of_image": image_size, "entry_point": base + entry,
        "ranges": [{"va": hex(va), "bytes": size, "sha256": hashlib.sha256(read(va, size)).hexdigest()}
                   for va, size in ranges],
        "functions": len(ranges), "imports": len(imports),
        "crt_diagnostic_entry": hex(CRT_ENTRY) if args.crt else None,
        "crt_tls_callback_entries": [hex(va) for va in TLS_CALLBACKS] if args.crt else [],
        "crt_direct_closure_depth": args.crt_depth if args.crt else None,
        "crt_unresolved_direct_targets": [hex(va) for va in unresolved],
        "recomp_todo_sites": len(todo_lines), "recomp_todo_lines": todo_lines,
        "catalog_complete": False, "game_boot_proven": False,
        "original_entry_preserved": True, "no_runtime_cpu_translation": True,
        "tls_callbacks_executed": False,
        "limit": "Bounded supplied-wrapper/optional original-CRT diagnostic catalog; a missing target traps. Game boot is not proven.",
        "generator_sources": {str(p.relative_to(ROOT)): reference64.digest(p) for p in (
            ROOT / "tools/lift_startup.py", ROOT / "tools/lift64_checked.py",
            ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py",
            ROOT / "reference/pcrecomp/tools/lift/lift32_cpu.py")},
        "files": {p.name: reference64.digest(p) for p in sorted(OUT.iterdir()) if p.is_file()},
    }
    meta_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("files", "recomp_todo_lines")}, indent=2))


if __name__ == "__main__":
    main()
