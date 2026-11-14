"""Lift only the five approved, independently proved bounded-table entries."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from lift64_checked import CheckedLifter

ROOT = Path(__file__).resolve().parents[1]
CENSUS_SHA = "8a686deca1f74575095fb42133c2702141ed6681f707de4d9bf5909982a5a8bf"
APPROVED = [(0x140ED24F5, 0x140ED255D, 0x140ED2055, "0813"),
            (0x140F9A5D2, 0x140F9A61E, 0x140F9A53B, "0823"),
            (0x140F9A61E, 0x140F9A633, 0x140F9A633, "0823"),
            (0x140F9A6AB, 0x140F9A6FB, 0x140F9A549, "0823"),
            (0x141AFFBEF, 0x141AFFC3F, 0x141AFFC3F, "0968")]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--promote", action="store_true")
    args = parser.parse_args()
    stage = ROOT / "build/windows-switch-extension"
    stage.mkdir(parents=True, exist_ok=True)
    source_dir = ROOT / "native/windows" if args.promote else stage
    test_dir = ROOT / "tests" if args.promote else stage
    receipt = ROOT / "research/windows-switch-extension.json" if args.promote else stage / "windows-switch-extension.json"
    census_path = ROOT / "research/windows-bounded-jump-tables.json"
    assert sha(census_path) == CENSUS_SHA
    census = json.loads(census_path.read_text())
    config = json.loads((ROOT / "port.json").read_text())
    original = Path(config["game_directory"]) / "Frostpunk.exe"
    assert sha(original) == config["original_exe_sha256"] == census["original_exe_sha256"]
    pe = pefile.PE(str(original), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")

    def read(va, size):
        assert base + text.VirtualAddress <= va and va + size <= base + text.VirtualAddress + text.SizeOfRawData
        return pe.get_data(va - base, size)

    lifter = CheckedLifter(image_size=pe.OPTIONAL_HEADER.SizeOfImage, read_va=read, image_base=base,
                          original_sha256=config["original_exe_sha256"])
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    metadata = json.loads((ROOT / "generated/frostpunk-x64/recomp_meta.json").read_text())
    bodies, entries, cases, baselines, rows, sources = [], [], [], [], [], {}
    for va, end, tail, unit in APPROVED:
        witnesses = [(table, entry) for table in census["tables"] for entry in table["entries"]
                     if entry["target"] == hex(va) and entry["missing_global_and_local"]]
        assert len(witnesses) == 1
        table, entry = witnesses[0]
        proof = entry["boundary"]
        assert proof["kind"] == "complete_pdata_prefix_decode"
        prefix_start = int(proof["start"], 16)
        prefix = read(prefix_start, va - prefix_start)
        assert hashlib.sha256(prefix).hexdigest() == proof["prefix_sha256"]
        assert sum(i.size for i in decoder.disasm(prefix, prefix_start)) == len(prefix)
        code = read(va, end - va)
        assert sum(i.size for i in decoder.disasm(code, va)) == len(code)
        name = f"switch_extension_{va:X}"
        body = lifter.lift_function(code, va, name=name)
        assert "RECOMP_TODO(" not in body
        bodies.append(body.replace("void " + name + "(", "static void " + name + "(", 1))
        literal = '"' + ''.join('\\x%02x' % b for b in code) + '"'
        entries.append(f"    {{UINT64_C(0x{va:x}),{len(code)},{literal},{name}}},")
        cases.append(f"    {{UINT64_C(0x{va:x}),UINT64_C(0x{tail:x}),{len(code)},{literal}}},")
        canonical = ROOT / f"generated/frostpunk-x64/recomp_funcs_{unit}.c"
        assert sha(canonical) == metadata["generated_files"][canonical.name]
        sources[canonical.relative_to(ROOT).as_posix()] = sha(canonical)
        lines = canonical.read_text().splitlines()
        first = next(n for n, line in enumerate(lines) if f"/* {va:012X}:" in line)
        last = next(n for n in range(first + 1, len(lines)) if f"/* {end:012X}:" in lines[n])
        # A split C function can end before the next original instruction.
        stop = next((n for n in range(first, last) if lines[n] == "}"), last)
        while stop > first and (lines[stop - 1].startswith("L_") or lines[stop - 1].strip().startswith("c->rip =")):
            stop -= 1
        baseline = "\n".join(lines[first:stop])
        labels = set(re.findall(r"^L_([0-9A-F]+):", baseline, re.M))
        baseline = re.sub(r"goto L_([0-9A-F]+);", lambda m: m[0] if m[1] in labels else
                          "{ dispatch(c,0x%sull); return; }" % m[1], baseline)
        baselines.append(f"static void baseline_{va:X}(CPU *c) {{\n    c->rip=0x{va:X}ull;\n" + baseline +
                         f"\n    dispatch(c,0x{end:X}ull);\n}}\n")
        rows.append(dict(address=hex(va), end=hex(end), size=len(code), original_bytes=code.hex(),
                         original_bytes_sha256=hashlib.sha256(code).hexdigest(),
                         lifted_C_sha256=hashlib.sha256(body.encode()).hexdigest(),
                         table=table["table"], table_sha256=table["table_sha256"], indices=entry["indices"], boundary=proof))
    output = source_dir / "switch_extension_bodies.inc"
    output.write_text("/* Five exact bounded-table continuation lifts; canonical C is sealed. */\n\n" +
                      "\n\n".join(bodies) + "\n#define SWITCH_EXTENSION_ENTRIES \\\n" + " \\\n".join(entries) + "\n", encoding="utf-8")
    cases_path = test_dir / "windows_switch_extension_cases.h"
    cases_path.write_text("static const test_case switch_extension_cases[]={\n" + "\n".join(cases) + "\n};\n", encoding="ascii")
    baseline_path = test_dir / "windows_switch_extension_baseline.h"
    baseline_path.write_text("\n".join(baselines), encoding="utf-8")
    report = dict(success=True, original_exe_sha256=sha(original), census_sha256=CENSUS_SHA,
                  producer_sha256=sha(Path(__file__)), count=5, bytes=sum(row["size"] for row in rows),
                  entries=rows, canonical_sources=sources, canonical_modified=False, original_bytes_executed=False,
                  outputs={path.name: sha(path) for path in (output, cases_path, baseline_path)})
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(receipt=str(receipt), count=report["count"], bytes=report["bytes"], promoted=args.promote)))


if __name__ == "__main__":
    main()
