"""Read-only census of exact omitted original LEA-referenced short leaf/thunks.

Writes evidence only. Does not generate C, modify catalogues, compile, or run any
guest/phone code. Candidates require narrow byte/CFG checks and zero lifter TODO.
"""
import bisect
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET, CS_GRP_PRIVILEGE
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
from lift64_checked import CheckedLifter, ORIGINAL_SHA256

ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "generated/frostpunk-x64"
OUTPUT = ROOT / "research/omitted-short-leaves.json"
LEA = re.compile(rb"GVA\(0x([0-9A-Fa-f]+)\).*?/\*\s*([0-9A-Fa-f]+):\s*lea\s+([^*]*)\*/")
DISPATCH = re.compile(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*L_[0-9A-Fa-f]+\s*\}")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    cfg = json.loads((ROOT / "port.json").read_text())
    path = Path(cfg["game_directory"]) / "Frostpunk.exe"
    original = path.read_bytes()
    if sha(original) != ORIGINAL_SHA256 or cfg["original_exe_sha256"] != ORIGINAL_SHA256:
        raise RuntimeError("Original PE pin changed")
    pe = pefile.PE(data=original)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    low, high = base + text.VirtualAddress, base + text.VirtualAddress + min(text.Misc_VirtualSize, text.SizeOfRawData)
    def read(va, n):
        if va < low or va > high or n > high - va:
            raise ValueError("Outside file-backed .text")
        off = text.PointerToRawData + va - low
        return original[off:off + n]
    pdata = sorted((base + row.struct.BeginAddress, base + row.struct.EndAddress) for row in pe.DIRECTORY_ENTRY_EXCEPTION)
    pdata_starts = [a for a, b in pdata]
    def in_pdata(va):
        i = bisect.bisect_right(pdata_starts, va) - 1
        return i >= 0 and va < pdata[i][1]
    dispatch_path = DIRECTORY / "recomp_dispatch.c"
    dispatch_raw = dispatch_path.read_bytes()
    known = {int(x, 16) for x in DISPATCH.findall(dispatch_raw)}
    if len(known) != 399917:
        raise RuntimeError(f"Expected immutable parent399917, found{len(known)}")
    sources, targets = {}, defaultdict(dict)
    all_targets=set();all_sites=set();early_rejections={}
    total = 0
    for source in sorted(DIRECTORY.glob("recomp_funcs_*.c")):
        raw = source.read_bytes()
        name = source.relative_to(ROOT).as_posix()
        sources[name] = sha(raw)
        for match in LEA.finditer(raw):
            target, site = int(match[1], 16), int(match[2], 16)
            total += 1
            all_targets.add(target);all_sites.add(site)
            why=None
            if not low<=target<high:why="outside_text"
            elif target in known:why="already_in_parent_dispatch"
            elif in_pdata(target):why="inside_pdata_range"
            elif target&15:why="not_16_byte_aligned"
            elif target==low or read(target-1,1)!=b"\xcc":why="preceding_byte_not_CC"
            if why:
                early_rejections[target]=why
                continue
            # Every original instruction site is deduplicated across generated
            # split bodies; its source occurrences remain evidence.
            row = targets[target].setdefault(site, dict(original_site=f"0x{site:x}", original_comment=match[3].decode().strip(), generated_occurrences=[]))
            row["generated_occurrences"].append(dict(file=name, line=raw.count(b"\n", 0, match.start()) + 1))
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    lifter = CheckedLifter(image_size=pe.OPTIONAL_HEADER.SizeOfImage, read_va=read, image_base=base, original_sha256=ORIGINAL_SHA256)
    reject = Counter(early_rejections.values())
    accepted = []
    unique_original_sites = len(all_sites)
    for target, sites in sorted(targets.items()):
        why = None
        if not low <= target < high:
            why = "outside_text"
        elif target in known:
            why = "already_in_parent_dispatch"
        elif in_pdata(target):
            why = "inside_pdata_range"
        elif target & 15:
            why = "not_16_byte_aligned"
        elif target == low or read(target - 1, 1) != b"\xcc":
            why = "preceding_byte_not_CC"
        if why:
            reject[why] += 1
            continue
        references = []
        for site, row in sorted(sites.items()):
            try:
                instruction = next(md.disasm(read(site, min(15, high - site)), site, count=1))
                operands = instruction.operands
                valid = instruction.mnemonic == "lea" and len(operands) == 2 and operands[1].type == X86_OP_MEM and operands[1].mem.base == X86_REG_RIP and instruction.address + instruction.size + operands[1].mem.disp == target
            except (ValueError, StopIteration):
                valid = False
            if not valid:
                raise RuntimeError(f"Original LEA comment/target mismatch {site:#x} -> {target:#x}")
            references.append(dict(**row, instruction_bytes=bytes(instruction.bytes).hex()))
        body = []
        terminal = None
        limit = min(64, high - target)
        for instruction in md.disasm(read(target, limit), target):
            if instruction.address != target + sum(i.size for i in body):
                why = "instruction_gap"
                break
            body.append(instruction)
            if instruction.mnemonic == "ret" and bytes(instruction.bytes) == b"\xc3":
                terminal = dict(kind="plain_RET")
                break
            if instruction.mnemonic == "jmp" and len(instruction.operands) == 1 and instruction.operands[0].type == X86_OP_IMM:
                destination = instruction.operands[0].imm
                if destination in known:
                    terminal = dict(kind="direct_unconditional_JMP", target=f"0x{destination:x}", target_in_parent_dispatch=True)
                else:
                    why = "terminal_JMP_target_not_in_parent_dispatch"
                break
            if any(instruction.group(g) for g in (CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET, CS_GRP_PRIVILEGE)) or instruction.mnemonic in {"ud2", "syscall", "sysenter", "xbegin", "xend"}:
                why = "earlier_control_flow_or_privileged_instruction"
                break
        if not terminal:
            reject[why or "no_terminal_within64_decodable_bytes"] += 1
            continue
        size = sum(i.size for i in body)
        end = target + size
        padding_end = (end + 15) & ~15
        if padding_end > high or read(end, padding_end - end) != b"\xcc" * (padding_end - end):
            reject["trailing_padding_not_all_CC_to_next16"] += 1
            continue
        code = read(target, size)
        generated = lifter.lift_function(code, target, name=f"short_leaf_{target:X}")
        todo = [s.strip() for s in generated.splitlines() if "RECOMP_TODO" in s]
        if todo:
            reject["CheckedLifter_TODO"] += 1
            continue
        accepted.append(dict(address=f"0x{target:x}", size=size, original_bytes=code.hex(), original_bytes_sha256=sha(code), preceding_byte="cc", text_file_offset=text.PointerToRawData + target - low, outside_all_pdata=True, parent_dispatch_missing=True, aligned16=True, trailing_padding=dict(address=f"0x{end:x}", size=padding_end - end, bytes=read(end, padding_end - end).hex(), next16=f"0x{padding_end:x}"), instructions=[dict(address=f"0x{i.address:x}", bytes=bytes(i.bytes).hex(), mnemonic=i.mnemonic, operands=i.op_str) for i in body], terminal=terminal, validated_original_LEA_sites=references, CheckedLifter_TODO_count=0, lifted_C_sha256=sha(generated.encode())))
    # The parent/source inputs are immutable throughout the census.
    if dispatch_path.read_bytes() != dispatch_raw or any(sha((ROOT / name).read_bytes()) != digest for name, digest in sources.items()):
        raise RuntimeError("Parent generated inputs changed during read-only census")
    dependencies = [Path(__file__), ROOT / "tools/lift64_checked.py", ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py", ROOT / "reference/pcrecomp/tools/lift/generate64.py"]
    result = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), scope="Read-only exact-byte candidate census, no catalogue/C generation/build/runtime execution or phone commands", original_exe_sha256=ORIGINAL_SHA256, parent_dispatch_sha256=sha(dispatch_raw), parent_dispatch_entries=len(known), generated_source_count=len(sources), generated_source_sha256=sources, checker_source_sha256={p.relative_to(ROOT).as_posix():sha(p.read_bytes()) for p in dependencies}, text=dict(begin=f"0x{low:x}", end=f"0x{high:x}"), pdata_range_count=len(pdata), counts=dict(generated_LEA_GVA_occurrences=total, unique_targets=len(all_targets), unique_original_LEA_sites=unique_original_sites, accepted=len(accepted), rejected=dict(reject), terminal_kinds=dict(Counter(t["terminal"]["kind"] for t in accepted))), candidates=accepted, limitations=["This is only LEA-referenced straight-line code ending within64B; other callback/branch/jump-table discovery is outside scope", "No equivalence/runtime execution proof is claimed; acceptance is input evidence for a separate explicit static-AOT configuration", "A zero-byte padding interval is allowed when the plainRET/JMP already ends on the next16 boundary; the actual interval is recorded"])
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(report=str(OUTPUT), counts=result["counts"], actual_missing_140de8740=next((x for x in accepted if x["address"] == "0x140de8740"), None))))


if __name__ == "__main__":
    main()
