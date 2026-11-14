"""Read original stored code pointers and report narrowly bounded omitted leaves.

Writes evidence only: no catalogue changes, C generation, build or execution.
"""
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
from lift64_checked import CheckedLifter, ORIGINAL_SHA256
from lift_supplemental import validate_tail

ROOT = Path(__file__).resolve().parents[1]


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def main():
    cfg = json.loads((ROOT / "port.json").read_text())
    original_path = Path(cfg["game_directory"]) / "Frostpunk.exe"
    original = original_path.read_bytes()
    if sha(original) != ORIGINAL_SHA256 or cfg["original_exe_sha256"] != ORIGINAL_SHA256:
        raise ValueError("Original PE identity changed")
    pe = pefile.PE(data=original, fast_load=True)
    base, image_size = pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.SizeOfImage
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    low = base + text.VirtualAddress
    high = low + min(text.Misc_VirtualSize, text.SizeOfRawData)

    def read(va, n):
        if va < low or n < 0 or va > high or n > high - va:
            raise ValueError("Read outside file-backed .text")
        offset = text.PointerToRawData + va - low
        return original[offset:offset + n]

    pins = {ROOT / "generated/frostpunk-x64/recomp_dispatch.c": None,
            ROOT / "generated/frostpunk-x64/recomp_meta.json": None,
            ROOT / "generated/supplemental/recomp_supplemental.c": None,
            ROOT / "generated/supplemental/recomp_meta.json": None}
    pins = {path: sha(path.read_bytes()) for path in pins}
    main_raw = (ROOT / "generated/frostpunk-x64/recomp_dispatch.c").read_bytes()
    supplemental_raw = (ROOT / "generated/supplemental/recomp_supplemental.c").read_bytes()
    parent = {int(v, 16) for v in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*L_[0-9A-Fa-f]+\s*\}", main_raw)}
    supplemental = {int(v, 16) for v in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*frostpunk_supplemental_[0-9A-Fa-f]+\s*\}", supplemental_raw)}
    if len(parent) != 399917 or len(supplemental) != 1985 or parent & supplemental:
        raise ValueError("Immutable main/supplemental dispatch set differs from expected counts")
    for directory, source in (("frostpunk-x64", "recomp_dispatch.c"), ("supplemental", "recomp_supplemental.c")):
        metadata = json.loads((ROOT / "generated" / directory / "recomp_meta.json").read_text())
        if (metadata["original_exe_sha256"] != ORIGINAL_SHA256 or
                sha((ROOT / "generated" / directory / source).read_bytes()) != metadata["generated_files"][source]):
            raise ValueError("Generated dispatch provenance changed")
    known = parent | supplemental
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    pdata_offset = pe.get_offset_from_rva(directory.VirtualAddress)
    pdata_raw = original[pdata_offset:pdata_offset + directory.Size]
    if len(pdata_raw) != directory.Size or len(pdata_raw) % 12:
        raise ValueError("Invalid raw exception-directory extent")
    pdata = sorted((base + a, base + b) for a, b, unwind in struct.iter_unpack("<III", pdata_raw) if a or b or unwind)
    if any(not base <= a < b <= base + image_size for a, b in pdata):
        raise ValueError("Invalid original pdata range")
    starts, prefix_ends, maximum = [], [], 0
    for begin, end in pdata:
        starts.append(begin)
        maximum = max(maximum, end)
        prefix_ends.append(maximum)

    def in_pdata(va):
        i = bisect.bisect_right(starts, va) - 1
        return i >= 0 and va < prefix_ends[i]

    refs = defaultdict(list)
    sections, total_slots = [], 0
    # Scan all byte offsets, including packed/unaligned stored pointers.
    assert low >> 32 == (high - 1) >> 32
    high_word = re.compile(re.escape(struct.pack("<I", low >> 32)))
    for section in pe.sections:
        name = section.Name.rstrip(b"\0").decode()
        if name not in (".rdata", ".data"):
            continue
        size = min(section.Misc_VirtualSize, section.SizeOfRawData)
        raw = original[section.PointerToRawData:section.PointerToRawData + size]
        if len(raw) != size:
            raise ValueError("Truncated data section")
        section_pointers = 0
        for match in high_word.finditer(raw):
            at = match.start() - 4
            if at < 0 or at + 8 > len(raw):
                continue
            target = struct.unpack_from("<Q", raw, at)[0]
            if not low <= target < high:
                continue
            address = base + section.VirtualAddress + at
            refs[target].append(dict(section=name, address=hex(address), file_offset=section.PointerToRawData + at,
                                     aligned8=not (address & 7), pointer_bytes=raw[at:at + 8].hex()))
            section_pointers += 1
        total_slots += section_pointers
        sections.append(dict(name=name, address=hex(base + section.VirtualAddress), file_backed_bytes=size,
                             raw_sha256=sha(raw), code_pointer_occurrences=section_pointers))
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    lifter = CheckedLifter(image_size=image_size, read_va=read, image_base=base, original_sha256=ORIGINAL_SHA256)
    rejected, accepted = Counter(), []
    for target, references in sorted(refs.items()):
        if target in known:
            rejected["already_in_main_or_supplemental"] += 1
            continue
        if in_pdata(target):
            rejected["inside_pdata"] += 1
            continue
        if target & 15:
            rejected["target_not_aligned16"] += 1
            continue
        if target == low or read(target - 1, 1) != b"\xcc":
            rejected["preceding_byte_not_CC"] += 1
            continue
        body, size = [], 0
        for instruction in md.disasm(read(target, min(64, high - target)), target):
            body.append(instruction)
            size += instruction.size
            if instruction.mnemonic in ("ret", "jmp"):
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
        lifted = lifter.lift_function(raw, target, name=f"data_leaf_{target:X}")
        if "RECOMP_TODO" in lifted:
            rejected["CheckedLifter_TODO"] += 1
            continue
        accepted.append(dict(address=hex(target), size=size, original_bytes=raw.hex(), original_bytes_sha256=sha(raw),
                             text_file_offset=text.PointerToRawData + target - low, aligned16=True, preceding_byte="cc",
                             outside_all_pdata=True, missing_main_and_supplemental=True,
                             trailing_padding_bytes=read(target + size, padding_end - target - size).hex(),
                             terminal=dict(kind="direct_known_JMP" if tail is not None else "plain_RET",
                                           target=hex(tail) if tail is not None else None,
                                           catalog=("main" if tail in parent else "supplemental") if tail is not None else None),
                             instructions=[dict(address=hex(i.address), bytes=bytes(i.bytes).hex(), mnemonic=i.mnemonic,
                                                operands=i.op_str) for i in body],
                             stored_pointer_references=references, CheckedLifter_TODO_count=0,
                             lifted_C_sha256=sha(lifted.encode())))
    if any(sha(path.read_bytes()) != expected for path, expected in pins.items()) or sha(original_path.read_bytes()) != ORIGINAL_SHA256:
        raise ValueError("Original/catalog inputs changed during read-only census")
    dependencies = [Path(__file__), ROOT / "tools/lift_supplemental.py", ROOT / "tools/lift64_checked.py",
                    ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py", ROOT / "reference/pcrecomp/tools/lift/generate64.py"]
    result = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), original_exe_sha256=ORIGINAL_SHA256,
                  scope="Stored original .rdata/.data PE64 code pointers at every byte offset; evidence only, no AOT generation or guest execution",
                  original_modified=False, generated_sources_modified=False, pointer_scan_stride_bytes=1,
                  parent_dispatch_entries=len(parent), supplemental_dispatch_entries=len(supplemental),
                  catalog_sha256={p.relative_to(ROOT).as_posix(): h for p, h in pins.items()},
                  producer_sha256={p.relative_to(ROOT).as_posix(): sha(p.read_bytes()) for p in dependencies},
                  sections=sections, pdata_ranges=len(pdata), pdata_raw_sha256=sha(pdata_raw),
                  counts=dict(stored_code_pointer_occurrences=total_slots, unique_code_pointer_targets=len(refs),
                              omitted_unique_targets=len(set(refs) - known), accepted=len(accepted),
                              accepted_stored_pointer_occurrences=sum(len(r["stored_pointer_references"]) for r in accepted),
                              original_code_bytes=sum(r["size"] for r in accepted),
                              terminal_kinds=dict(Counter(r["terminal"]["kind"] for r in accepted)), rejected=dict(rejected)),
                  observed_missing_140fee310=next((r for r in accepted if r["address"] == "0x140fee310"), None),
                  candidates=accepted,
                  limitations=["Only file-backed .rdata/.data absolute 64-bit pointers are scanned; LEA sites, relative tables, other sections and runtime-created pointers are outside scope",
                               "Candidates must start aligned16 after CC outside all pdata, end within64B in plain RET/direct main-or-supplemental JMP, have no earlier control flow, CC padding and zero lifter TODO",
                               "Static byte/CFG evidence only; no runtime/equivalence proof and no broad extra-AOT configuration is created"])
    output = ROOT / "research/windows-omitted-data-leaves.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(report=str(output), counts=result["counts"], witness_140fee310=result["observed_missing_140fee310"] is not None)))


if __name__ == "__main__":
    assert validate_tail(bytes.fromhex("4883c118e93746e6ff"), 0x140FEE310, {0x140E52950}) == 0x140E52950
    assert validate_tail(b"\xc3", 0x140FEE310, set()) is None
    main()
