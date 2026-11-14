"""Evidence-only LEA leaf census after a proven preceding RET instruction."""
import bisect
from collections import Counter, defaultdict
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
from lift64_checked import CheckedLifter, ORIGINAL_SHA256
from lift_supplemental import validate_tail

ROOT = Path(__file__).resolve().parents[1]
LEA = re.compile(rb"GVA\(0x([0-9A-Fa-f]+)\).*?/\*\s*([0-9A-Fa-f]+):\s*lea\s+([^*]*)\*/")


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    cfg = json.loads((ROOT / "port.json").read_text())
    original_path = Path(cfg["game_directory"]) / "Frostpunk.exe"
    original = original_path.read_bytes()
    if sha(original) != ORIGINAL_SHA256 or cfg["original_exe_sha256"] != ORIGINAL_SHA256:
        raise ValueError("Original executable identity changed")
    pe = pefile.PE(data=original, fast_load=True)
    base, image_size = pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    low, high = base + text.VirtualAddress, base + text.VirtualAddress + min(text.Misc_VirtualSize, text.SizeOfRawData)

    def read(va, n):
        if va < low or n < 0 or va > high or n > high - va:
            raise ValueError("Outside file-backed .text")
        off = text.PointerToRawData + va - low
        return original[off:off + n]

    parent_path = ROOT / "generated/frostpunk-x64/recomp_dispatch.c"
    supplemental_path = ROOT / "generated/supplemental/recomp_supplemental.c"
    pinned = {parent_path: sha(parent_path.read_bytes()), supplemental_path: sha(supplemental_path.read_bytes())}
    parent = {int(v, 16) for v in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*L_[0-9A-Fa-f]+\s*\}", parent_path.read_bytes())}
    supplemental = {int(v, 16) for v in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*frostpunk_supplemental_[0-9A-Fa-f]+\s*\}", supplemental_path.read_bytes())}
    if len(parent) != 399917 or len(supplemental) != 1985 or parent & supplemental:
        raise ValueError("Immutable main/supplemental dispatch set changed")
    known = parent | supplemental
    dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    pdata_offset = pe.get_offset_from_rva(dd.VirtualAddress)
    pdata_raw = original[pdata_offset:pdata_offset + dd.Size]
    if len(pdata_raw) != dd.Size or len(pdata_raw) % 12:
        raise ValueError("Invalid raw pdata extent")
    pdata = sorted((base + a, base + b) for a, b, u in struct.iter_unpack("<III", pdata_raw) if a or b or u)
    if any(not base <= a < b <= base + image_size for a, b in pdata):
        raise ValueError("Invalid pdata range")
    starts, prefix_ends, end = [], [], 0
    for a, b in pdata:
        starts.append(a)
        end = max(end, b)
        prefix_ends.append(end)

    def in_pdata(va):
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and va < prefix_ends[i]

    anchors = sorted(known | set(starts))
    sources = sorted((ROOT / "generated/frostpunk-x64").glob("recomp_funcs_*.c"))
    if len(sources) != 1000:
        raise ValueError("Generated source set is incomplete")
    source_hashes, refs, rejected = {}, defaultdict(dict), Counter()
    all_targets, sites, occurrences = set(), set(), 0
    early_rejected = {}
    for path in sources:
        raw = path.read_bytes()
        source_hashes[path] = sha(raw)
        for match in LEA.finditer(raw):
            target, site = int(match[1], 16), int(match[2], 16)
            occurrences += 1
            all_targets.add(target)
            sites.add(site)
            reason = ("outside_text" if not low <= target < high else
                      "already_main_or_supplemental" if target in known else
                      "inside_pdata" if in_pdata(target) else
                      "not_aligned16" if target & 15 else
                      "preceding_byte_not_RET" if target == low or read(target - 1, 1) != b"\xc3" else None)
            if reason:
                early_rejected[target] = reason
                continue
            row = refs[target].setdefault(site, dict(address=hex(site), original_comment=match[3].decode().strip(), occurrences=[]))
            row["occurrences"].append(dict(file=path.relative_to(ROOT).as_posix(), line=raw.count(b"\n", 0, match.start()) + 1))
    rejected.update(early_rejected.values())
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    lifter = CheckedLifter(image_size=image_size, read_va=read, image_base=base, original_sha256=ORIGINAL_SHA256)
    accepted = []
    for target, original_sites in sorted(refs.items()):
        i = bisect.bisect_left(anchors, target) - 1
        anchor = anchors[i] if i >= 0 else 0
        if not low <= anchor < target or target - anchor > 256:
            rejected["no_known_predecessor_boundary_within256"] += 1
            continue
        predecessor_raw = read(anchor, target - anchor)
        predecessor = list(md.disasm(predecessor_raw, anchor))
        if (not predecessor or sum(inst.size for inst in predecessor) != len(predecessor_raw) or
                predecessor[-1].address != target - 1 or bytes(predecessor[-1].bytes) != b"\xc3" or predecessor[-1].mnemonic != "ret"):
            rejected["predecessor_decode_does_not_end_in_RET_at_target_minus1"] += 1
            continue
        for site, row in sorted(original_sites.items()):
            instructions = list(md.disasm(read(site, min(15, high - site)), site, count=1))
            if not instructions:
                raise ValueError("Original LEA is undecodable")
            inst = instructions[0]
            if (inst.mnemonic != "lea" or len(inst.operands) != 2 or inst.operands[1].type != X86_OP_MEM or
                    inst.operands[1].mem.base != X86_REG_RIP or inst.address + inst.size + inst.operands[1].mem.disp != target):
                raise ValueError("Original LEA bytes/target differ from generated comment")
            row["instruction_bytes"] = bytes(inst.bytes).hex()
        body, size = [], 0
        for inst in md.disasm(read(target, min(64, high - target)), target):
            body.append(inst)
            size += inst.size
            if inst.mnemonic in ("ret", "jmp"):
                break
        raw = read(target, size)
        try:
            tail = validate_tail(raw, target, known)
        except ValueError as error:
            rejected[str(error)] += 1
            continue
        padding_end = (target + size + 15) & ~15
        if padding_end > high or read(target + size, padding_end - target - size) != b"\xcc" * (padding_end - target - size):
            rejected["trailing_padding_not_CC_to_next16"] += 1
            continue
        lifted = lifter.lift_function(raw, target, name=f"ret_leaf_{target:X}")
        if "RECOMP_TODO" in lifted:
            rejected["CheckedLifter_TODO"] += 1
            continue
        accepted.append(dict(address=hex(target), size=size, original_bytes=raw.hex(), original_bytes_sha256=sha(raw),
                             text_file_offset=text.PointerToRawData + target - low, aligned16=True,
                             outside_all_pdata=True, missing_main_and_supplemental=True,
                             predecessor=dict(anchor=hex(anchor), anchor_in_main=anchor in parent,
                                              anchor_in_supplemental=anchor in supplemental, anchor_in_pdata_starts=anchor in starts,
                                              decoded_bytes=predecessor_raw.hex(), decoded_bytes_sha256=sha(predecessor_raw),
                                              decoded_instructions=len(predecessor), final_RET_address=hex(target - 1)),
                             trailing_padding_bytes=read(target + size, padding_end - target - size).hex(),
                             terminal=dict(kind="direct_known_JMP" if tail is not None else "plain_RET",
                                           target=hex(tail) if tail is not None else None,
                                           catalog=("main" if tail in parent else "supplemental") if tail is not None else None),
                             instructions=[dict(address=hex(inst.address), bytes=bytes(inst.bytes).hex(), mnemonic=inst.mnemonic,
                                                operands=inst.op_str) for inst in body],
                             verified_LEA_sites=list(original_sites.values()), CheckedLifter_TODO_count=0,
                             lifted_C_sha256=sha(lifted.encode())))
    if (any(sha(p.read_bytes()) != h for p, h in pinned.items()) or
            any(sha(p.read_bytes()) != h for p, h in source_hashes.items()) or sha(original_path.read_bytes()) != ORIGINAL_SHA256):
        raise ValueError("Original/generated source inputs changed during census")
    actuals = {r["address"]: r for r in accepted if r["address"] in ("0x140fee310", "0x141cbdae0")}
    assert set(actuals) == {"0x140fee310", "0x141cbdae0"}, "Observed two misses must satisfy this independently checked policy"
    dependencies = [Path(__file__), ROOT / "tools/lift_supplemental.py", ROOT / "tools/lift64_checked.py",
                    ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py", ROOT / "reference/pcrecomp/tools/lift/generate64.py"]
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), original_exe_sha256=ORIGINAL_SHA256,
                  scope="Read-only LEA referenced omitted leaf evidence after validated preceding RET; no C/config generation or execution",
                  original_modified=False, generated_sources_modified=False,
                  parent_dispatch_entries=len(parent), supplemental_dispatch_entries=len(supplemental), pdata_ranges=len(pdata),
                  source_sha256={p.relative_to(ROOT).as_posix(): h for p, h in {**pinned, **source_hashes}.items()},
                  producer_sha256={p.relative_to(ROOT).as_posix(): sha(p.read_bytes()) for p in dependencies},
                  counts=dict(LEA_occurrences=occurrences, unique_targets=len(all_targets), unique_sites=len(sites),
                              candidates_after_early_filters=len(refs), accepted=len(accepted),
                              original_code_bytes=sum(r["size"] for r in accepted),
                              verified_LEA_sites=sum(len(r["verified_LEA_sites"]) for r in accepted),
                              terminal_kinds=dict(Counter(r["terminal"]["kind"] for r in accepted)), rejected=dict(rejected)),
                  observed_misses=actuals, candidates=accepted,
                  limitations=["Only LEA references in immutable generated sources, aligned16 targets outside pdata, after a RET proven by <=256B full decode from a known boundary",
                               "Body <=64B, no interior control flow, exact plain RET/direct known main-or-supplemental JMP, CC padding, and zero CheckedLifter TODO",
                               "No equivalence/runtime proof or broad extra-AOT configuration is claimed"])
    output = ROOT / "research/windows-omitted-ret-leaves.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(report=str(output), counts=report["counts"], observed_misses=list(actuals))))


if __name__ == "__main__":
    assert validate_tail(bytes.fromhex("4883c118e93746e6ff"), 0x140FEE310, {0x140E52950}) == 0x140E52950
    main()
