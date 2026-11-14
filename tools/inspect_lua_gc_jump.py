"""Pin the original Lua GC jump-table cases missed by the static entry table.

Read-only PE inspection. No original x64 instruction is executed.
"""
import bisect
import hashlib
import json
from pathlib import Path
import re
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = "45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc"
TABLE = 0x141C10194
TABLE_BYTES = bytes.fromhex("8301c1016001c1015601c101ce00c1016a01c1017701c1011a01c101")
CASES = {
    0x141C10160: bytes.fromhex("488b473848894330eb15488b4738488983a0000000eb08488b47384889436848895f38488b742430"),
    0x141C1016A: bytes.fromhex("488b4738488983a0000000eb08488b47384889436848895f38488b742430"),
    0x141C10177: bytes.fromhex("488b47384889436848895f38488b742430"),
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inspect():
    cfg = json.loads((ROOT / "port.json").read_text())
    path = Path(cfg["game_directory"]) / "Frostpunk.exe"
    before = path.stat()
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == ORIGINAL
    pe = pefile.PE(data=data, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    read = lambda va, n: pe.get_data(va - base, n)
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    spans = sorted((base+a, base+b, base+u) for a, b, u in struct.iter_unpack("<III", pe.get_data(directory.VirtualAddress, directory.Size)))
    starts = [a for a, b, u in spans]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    parent_file = ROOT / "generated/frostpunk-x64/recomp_dispatch.c"
    known = {int(x, 16) for x in re.findall(r"\{ 0x([0-9A-Fa-f]+)ull,", parent_file.read_text())}
    assert len(known) == 399917
    assert read(TABLE, 28) == TABLE_BYTES
    targets = [base+x for x in struct.unpack("<7I", TABLE_BYTES)]
    assert targets == [0x141C10183, 0x141C10160, 0x141C10156, 0x141C100CE, 0x141C1016A, 0x141C10177, 0x141C1011A]
    fragment_start, fragment_end, fragment_unwind = spans[bisect.bisect_right(starts, 0x141C1016A)-1]
    assert (fragment_start, fragment_end) == (0x141C10156, 0x141C10188)
    fragment = read(fragment_start, fragment_end-fragment_start)
    instructions = list(md.disasm(fragment, fragment_start))
    assert sum(i.size for i in instructions) == len(fragment)
    boundaries = {i.address for i in instructions}
    evidence = []
    for va, raw in CASES.items():
        assert va not in known and va in boundaries and va+len(raw) == fragment_end
        assert read(va, len(raw)) == raw
        decoded = list(md.disasm(raw, va))
        assert sum(i.size for i in decoded) == len(raw)
        assert all(i.mnemonic in {"mov", "jmp"} for i in decoded)
        for i in decoded:
            if i.mnemonic == "jmp":
                assert i.operands[0].type == X86_OP_IMM and i.operands[0].imm == 0x141C1017F
        evidence.append(dict(va=hex(va), size=len(raw), bytes=raw.hex(), sha256=hashlib.sha256(raw).hexdigest(),
                             jump_table_indices=[i for i, target in enumerate(targets) if target == va],
                             original_GC_type=[4+i for i, target in enumerate(targets) if target == va],
                             tail=hex(fragment_end), tail_in_parent=True))
    assert 0x141C10188 in known
    # Both the first object and the linked-list continuation use the same table.
    branch_bytes = bytes.fromhex("48988b8c869401c1014803ceffe1")
    assert read(0x141C100C0, len(branch_bytes)) == branch_bytes
    assert read(0x141C1009A, 4) == bytes.fromhex("0fb64208")
    assert read(0x141C100A5, 3) == bytes.fromhex("83e804")
    assert read(0x141C100AB, 9) == bytes.fromhex("83f8060f87d4000000")
    assert read(0x141C100FE, 12) == bytes.fromhex("0fb6430883e80483f80676b6")
    after = path.stat()
    assert (before.st_size, before.st_mtime_ns) == (after.st_size, after.st_mtime_ns)
    checkpoint = ROOT / "research/lua-gc-jump-checkpoint.json"
    actual = json.loads(checkpoint.read_text())
    assert "STOP missing static target=0x141c1016a return=0x142985960" in actual["actual_stop_lines"]
    return dict(original_exe=str(path), original_exe_sha256=ORIGINAL,
                producer="tools/inspect_lua_gc_jump.py", producer_sha256=sha(__file__),
                actual_checkpoint=str(checkpoint.relative_to(ROOT)), actual_checkpoint_sha256=sha(checkpoint),
                parent_dispatch_sha256=sha(parent_file),
                observed_target=hex(0x141C1016A), control_transfer="indirect JMP RCX, not CALL",
                table=dict(va=hex(TABLE), bytes=TABLE_BYTES.hex(), size=28, targets=[hex(x) for x in targets],
                           index="zero-extended GC byte at object+8 minus4, unsigned range0..6 checked before indexed load",
                           branch_va=hex(0x141C100CC), branch_sequence_va=hex(0x141C100C0), branch_sequence_bytes=branch_bytes.hex()),
                fragment=dict(start=hex(fragment_start), end=hex(fragment_end), unwind=hex(fragment_unwind),
                              bytes=fragment.hex(), sha256=hashlib.sha256(fragment).hexdigest(),
                              instructions=[dict(va=hex(i.address), bytes=i.bytes.hex(), mnemonic=i.mnemonic, operands=i.op_str) for i in instructions]),
                entries=evidence,
                contract="Each new entry is a suffix inside an existing unwind fragment. Load old head from [RDI+38], store at [RBX+30|A0|68], set [RDI+38]=RBX, restore RSI from [RSP+30], tail-dispatch existing141C10188 epilog. No new CALL, stack adjustment or flags change before existing epilog.",
                return_log_limit="At the indirect JMP, RSP points to the already allocated local frame; the logged QWORD142985960 is not a validated function return address. The existing epilog restores RBX/RDI and advances through the actual saved return slot.",
                scope="Only three missing entries from this seven-way original table; other targets are existing parent entries or an internal label in the dispatching parent function. Main399917 table/1003 objects unchanged.",
                original_x64_executed=False)


def main():
    report = inspect()
    target = ROOT / "research/lua-gc-jump-table.json"
    target.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(dict(receipt=str(target), sha256=sha(target), entries=len(report["entries"]))))


if __name__ == "__main__":
    main()
