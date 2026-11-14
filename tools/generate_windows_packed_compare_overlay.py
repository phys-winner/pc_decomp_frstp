"""Produce byte-proven Windows-only repairs for the sealed packed compare lowering."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re

import capstone
import pefile
from leaf_probe import ROOT

HOT_SITES = {0x140F63D68, 0x140F6401B, 0x140F64092, 0x140F6412A, 0x140F64196,
             0x140F672A0, 0x140F67311, 0x140F6739A, 0x140F673FE}
OPERATIONS = {"cmpeqps": ("FP_CMPPS_EQ", 0, "(_a == _b)"),
              "cmpltps": ("FP_CMPPS_LT", 1, "(_a < _b)"),
              "cmpeqpd": ("FP_CMPPD_EQ", 0, "(_a == _b)"),
              "cmpltpd": ("FP_CMPPD_LT", 1, "(_a < _b)"),
              "cmpnlepd": ("FP_CMPPD_NLE", 6, "!(_a <= _b)")}
GPRS = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate(scope="hot9"):
    assert scope in {"hot9", "all"}
    config = json.loads((ROOT / "port.json").read_text())
    original = Path(config["game_directory"]) / "Frostpunk.exe"
    assert digest(original) == config["original_exe_sha256"]
    pe = pefile.PE(str(original), fast_load=True)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    directory = ROOT / "generated/frostpunk-x64"
    metadata = json.loads((directory / "recomp_meta.json").read_text())
    assert metadata["original_exe_sha256"] == config["original_exe_sha256"]
    candidates = [directory / "recomp_funcs_0820.c"] if scope == "hot9" else sorted(directory.glob("recomp_funcs_*.c"))
    sites_by_va = {}
    units = []
    counts = collections.Counter()
    for canonical in candidates:
        lines = canonical.read_text().splitlines()
        changed = False
        for index, line in enumerate(lines):
            match = re.search(r'/\* ([0-9A-F]+): (cmp\w+(?:ps|pd)) (.*) \*/', line)
            if not match or (scope == "hot9" and int(match[1], 16) not in HOT_SITES):
                continue
            va = int(match[1], 16)
            instruction = next(decoder.disasm(pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 15), va))
            assert instruction.mnemonic == match[2] and instruction.mnemonic in OPERATIONS
            assert instruction.mnemonic + " " + instruction.op_str == match[2] + " " + match[3]
            operation, predicate, condition = OPERATIONS[instruction.mnemonic]
            assert instruction.bytes[-1] == predicate
            assert len(instruction.operands) == 2 and instruction.operands[0].type == capstone.CS_OP_REG
            destination = int(instruction.reg_name(instruction.operands[0].reg)[3:])
            assert destination < 16 and instruction.operands[0].size == 16
            operand = instruction.operands[1]
            source_info = {}
            if operand.type == capstone.CS_OP_REG:
                source = int(instruction.reg_name(operand.reg)[3:])
                assert source < 16 and operand.size == 16
                source_scalar = f"c->xmm[{source}].f32[0]"
                call = f"fp_simd_packed_compare(c,{destination},&c->xmm[{source}],{operation})"
                source_info = dict(source_kind="register", source=source)
            else:
                assert operand.type == capstone.CS_OP_MEM and operand.size == 16
                memory = operand.mem
                assert instruction.addr_size == 8 and not memory.segment and not memory.index and memory.disp >= 0
                base = instruction.reg_name(memory.base)
                if base == "rip":
                    target = va + instruction.size + memory.disp
                    address = f"GVA(0x{target:X})"
                    source_info = dict(source_kind="memory", source_gpr=-1, source_disp=0, source_va=f"0x{target:X}")
                else:
                    assert base in GPRS
                    address = f"(c->{base}" + (f" + 0x{memory.disp:X}ull" if memory.disp else "") + ")"
                    source_info = dict(source_kind="memory", source_gpr=GPRS.index(base), source_disp=memory.disp, source_va=None)
                source_scalar = f"rdss({address})"
                call = f"fp_simd_packed_compare_memory(c,{destination},{address},{operation})"
                source_info["source_address"] = address
            expected = (f"{{ float _a = c->xmm[{destination}].f32[0], _b = {source_scalar}; "
                        f"c->xmm[{destination}].u32[0] = {condition} ? 0xFFFFFFFFu : 0u; }}")
            old_code, comment = line.split(" /*", 1)
            assert old_code.strip() == expected, (canonical.name, index+1, old_code, expected)
            text = instruction.mnemonic + " " + instruction.op_str
            replacement = f'{{ if(!{call})frostpunk_unsupported(0x{va:X}ull,"{text}"); }}'
            lines[index] = "    " + replacement + " /*" + comment
            location = dict(path=str(canonical.relative_to(ROOT)).replace("\\", "/"), line=index+1)
            if va in sites_by_va:
                previous = sites_by_va[va]
                assert previous["old_code"] == old_code.strip() and previous["new_code"] == replacement
                previous["canonical_locations"].append(location)
                previous["canonical_lines"].append(index+1)
            else:
                sites_by_va[va] = dict(va=f"0x{va:X}", bytes=instruction.bytes.hex(), instruction=text,
                    canonical_locations=[location], canonical_lines=[index+1], destination=destination,
                    operation=operation, old_code=old_code.strip(), new_code=replacement, **source_info)
            counts[instruction.mnemonic] += 1
            changed = True
        if not changed:
            continue
        assert digest(canonical) == metadata["generated_files"][canonical.name]
        lines.insert(3, '#include "simd_float.h"')
        output_directory = "build/windows/generated-overlay" if scope == "hot9" else "build/windows/generated-overlay-all"
        out = ROOT / output_directory / canonical.name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
        units.append(dict(canonical=dict(path=str(canonical.relative_to(ROOT)).replace("\\", "/"), sha256=digest(canonical)),
                          overlay=dict(path=str(out.relative_to(ROOT)).replace("\\", "/"), sha256=digest(out))))
    if scope == "hot9":
        assert set(sites_by_va) == HOT_SITES and sum(counts.values()) == 17 and len(units) == 1
    else:
        assert dict(counts) == {"cmpltps": 194, "cmpeqps": 87, "cmpeqpd": 4, "cmpltpd": 2, "cmpnlepd": 1}
        assert len(sites_by_va) == 212 and len(units) == 46
    sites = [sites_by_va[va] for va in sorted(sites_by_va)]
    manifest = dict(success=False, scope=scope, original_exe_sha256=config["original_exe_sha256"],
        generator=dict(path=str(Path(__file__).resolve().relative_to(ROOT)).replace("\\", "/"), sha256=digest(Path(__file__))),
        units=units, dependencies={}, sites=sites, occurrences=sum(counts.values()), operation_counts=dict(counts), validation=dict(success=False))
    if scope == "hot9":
        manifest.update(units[0])
    path = ROOT / "research/windows-packed-compare-overlay.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest, path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", choices=["hot9", "all"], default="hot9")
    args = parser.parse_args()
    manifest, path = generate(args.scope)
    print(json.dumps(dict(scope=manifest["scope"], sites=len(manifest["sites"]), units=len(manifest["units"]),
        occurrences=manifest["occurrences"], validation_required=True, manifest=str(path.relative_to(ROOT))), indent=2))
