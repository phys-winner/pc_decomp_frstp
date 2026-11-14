"""Read-only bounded RVA jump-table census in the immutable x64 translation."""
import bisect
from collections import Counter, deque
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RIP

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = "45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc"
FUNCTION = re.compile(rb"^void L_([0-9A-Fa-f]+)\(CPU \*c\)", re.M)
DISPATCH = re.compile(rb"dispatch(?:_jmp)?\(c, 0x([0-9A-Fa-f]+)ull\)")
REGISTER_JUMP = re.compile(rb"/\*\s*([0-9A-Fa-f]+): jmp (r(?:ax|bx|cx|dx|si|di|bp|sp|[89]|1[0-5])) \*/")
COMMENT_ADDRESS = re.compile(rb"/\*\s*([0-9A-Fa-f]+):")
LOCAL_CASE = re.compile(rb"case 0x([0-9A-Fa-f]+)ull: goto")


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(name):
    if name.startswith("r") and name[1:2].isdigit():
        return name.rstrip("dwb")
    return {alias: "r" + tail for tail, aliases in {
        "ax": ("rax", "eax", "ax", "al", "ah"),
        "bx": ("rbx", "ebx", "bx", "bl", "bh"),
        "cx": ("rcx", "ecx", "cx", "cl", "ch"),
        "dx": ("rdx", "edx", "dx", "dl", "dh"),
        "si": ("rsi", "esi", "si", "sil"),
        "di": ("rdi", "edi", "di", "dil"),
        "bp": ("rbp", "ebp", "bp", "bpl"),
        "sp": ("rsp", "esp", "sp", "spl"),
    }.items() for alias in aliases}.get(name, name)


def reg(inst, operand):
    return canonical(inst.reg_name(operand.reg)) if operand.type == X86_OP_REG else None


def writes(inst, name):
    return any(canonical(inst.reg_name(r)) == name for r in inst.regs_access()[1])


def recognize(instructions):
    """Accept one fallthrough CMP/JA, LEA, dword load, ADD, JMP dataflow."""
    if not instructions or instructions[-1].mnemonic != "jmp":
        return None
    jump = instructions[-1]
    if len(jump.operands) != 1 or jump.operands[0].type != X86_OP_REG or jump.operands[0].size != 8:
        return None
    destination = reg(jump, jump.operands[0])
    for ai in range(len(instructions) - 2, max(-1, len(instructions) - 7), -1):
        add = instructions[ai]
        if (add.mnemonic != "add" or len(add.operands) != 2 or
                any(o.type != X86_OP_REG or o.size != 8 for o in add.operands) or
                reg(add, add.operands[0]) != destination):
            continue
        base_register = reg(add, add.operands[1])
        if base_register == destination or any(writes(i, destination) for i in instructions[ai + 1:-1]):
            continue
        for li in range(ai - 1, max(-1, ai - 7), -1):
            load = instructions[li]
            if (load.mnemonic not in ("mov", "movsxd") or len(load.operands) != 2 or
                    reg(load, load.operands[0]) != destination or load.operands[1].type != X86_OP_MEM or
                    load.operands[1].size != 4 or load.operands[0].size != (8 if load.mnemonic == "movsxd" else 4)):
                continue
            memory = load.operands[1].mem
            index = canonical(load.reg_name(memory.index))
            if (not memory.index or memory.scale != 4 or canonical(load.reg_name(memory.base)) != base_register or
                    any(writes(i, destination) for i in instructions[li + 1:ai])):
                continue
            for bi in range(li - 1, -1, -1):
                lea = instructions[bi]
                if (lea.mnemonic != "lea" or len(lea.operands) != 2 or
                        reg(lea, lea.operands[0]) != base_register or lea.operands[0].size != 8 or
                        lea.operands[1].type != X86_OP_MEM or lea.operands[1].mem.base != X86_REG_RIP or
                        any(writes(i, base_register) for i in instructions[bi + 1:li + 1])):
                    continue
                for ci in range(li - 2, -1, -1):
                    cmp = instructions[ci]
                    bound = instructions[ci + 1]
                    if (cmp.mnemonic != "cmp" or len(cmp.operands) != 2 or
                            reg(cmp, cmp.operands[0]) != index or cmp.operands[1].type != X86_OP_IMM or
                            not 0 <= cmp.operands[1].imm < 256 or cmp.operands[0].size not in (4, 8) or
                            bound.mnemonic != "ja" or len(bound.operands) != 1 or bound.operands[0].type != X86_OP_IMM):
                        continue
                    following = instructions[ci + 2:-1]
                    if any(i.group(CS_GRP_JUMP) or i.group(CS_GRP_CALL) or i.group(CS_GRP_RET) for i in following):
                        continue
                    # 32-bit bounds need a proved zero/sign extension of the same index.
                    index_writes = [i for i in instructions[ci + 2:li] if writes(i, index)]
                    if any(not (i.mnemonic == "cdqe" and index == "rax" or
                                i.mnemonic == "movsxd" and len(i.operands) == 2 and
                                reg(i, i.operands[0]) == index == reg(i, i.operands[1]) and i.operands[1].size == 4)
                           for i in index_writes):
                        continue
                    if cmp.operands[0].size == 4 and not index_writes:
                        previous = next((i for i in reversed(instructions[:ci]) if writes(i, index)), None)
                        if (previous is None or not previous.operands or previous.operands[0].type != X86_OP_REG or
                                previous.operands[0].size != 4 or reg(previous, previous.operands[0]) != index):
                            continue
                    fixed_base = lea.address + lea.size + lea.operands[1].mem.disp
                    return dict(compare=cmp, bound=bound, lea=lea, load=load, add=add, jump=jump,
                                base=fixed_base, table=fixed_base + memory.disp, count=cmp.operands[1].imm + 1,
                                signed=load.mnemonic == "movsxd", index=index,
                                instruction_slice=instructions[ci:])
    return None


def main():
    cfg = json.loads((ROOT / "port.json").read_text())
    original_path = Path(cfg["game_directory"]) / "Frostpunk.exe"
    original = original_path.read_bytes()
    if sha(original) != ORIGINAL or cfg["original_exe_sha256"] != ORIGINAL:
        raise ValueError("Original executable identity changed")
    pe = pefile.PE(data=original, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    text_start, text_end = base + text.VirtualAddress, base + text.VirtualAddress + min(text.Misc_VirtualSize, text.SizeOfRawData)

    def read(va, size):
        for section in pe.sections:
            low = base + section.VirtualAddress
            high = low + min(section.Misc_VirtualSize, section.SizeOfRawData)
            if low <= va <= high and 0 <= size <= high - va:
                off = section.PointerToRawData + va - low
                return original[off:off + size]
        raise ValueError("Outside file-backed section")

    dispatch_path = ROOT / "generated/frostpunk-x64/recomp_dispatch.c"
    supplemental_path = ROOT / "generated/supplemental/recomp_supplemental.c"
    pinned = {dispatch_path: sha(dispatch_path.read_bytes()), supplemental_path: sha(supplemental_path.read_bytes())}
    parent = {int(x, 16) for x in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*L_", dispatch_path.read_bytes())}
    supplemental = {int(x, 16) for x in re.findall(rb"\{\s*0x([0-9A-Fa-f]+)ull,\s*frostpunk_supplemental_", supplemental_path.read_bytes())}
    assert len(parent) == 399917 and len(supplemental) == 1985 and not parent & supplemental
    known = parent | supplemental
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    pdata = sorted((base + a, base + b) for a, b, u in struct.iter_unpack("<III", pe.get_data(directory.VirtualAddress, directory.Size)) if a or b or u)
    pdata_starts = [a for a, b in pdata]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    boundaries = {}

    def boundary(va):
        if va in known:
            return dict(kind="main" if va in parent else "supplemental")
        pi = bisect.bisect_right(pdata_starts, va) - 1
        if pi < 0 or not pdata[pi][0] <= va < pdata[pi][1]:
            return None
        a, b = pdata[pi]
        if b - a > 16384:
            return None
        if a not in boundaries:
            raw = read(a, b - a)
            decoded = list(md.disasm(raw, a))
            boundaries[a] = ({i.address for i in decoded}, sum(i.size for i in decoded) == len(raw), sha(raw))
        starts, complete, digest = boundaries[a]
        if va not in starts:
            return None
        # A fragment can contain its jump-table data after executable cases.
        prefix = read(a, va - a)
        return dict(kind="complete_pdata_prefix_decode", start=hex(a), end=hex(b),
                    prefix_size=len(prefix), prefix_sha256=sha(prefix),
                    fragment_bytes_sha256=digest, whole_fragment_decoded=complete,
                    first_target_bytes=read(va, min(15, b - va)).hex())

    files = sorted((ROOT / "generated/frostpunk-x64").glob("recomp_funcs_*.c"))
    assert len(files) == 1000
    adjacency, tables, seen_jumps, unproven = {}, [], set(), []
    rejected = Counter()
    for fi, path in enumerate(files):
        raw = path.read_bytes()
        pinned[path] = sha(raw)
        functions = list(FUNCTION.finditer(raw))
        positions = [m.start() for m in functions]
        for n, function in enumerate(functions):
            body = raw[function.end():functions[n + 1].start() if n + 1 < len(functions) else len(raw)]
            adjacency[int(function[1], 16)] = {int(m[1], 16) for m in DISPATCH.finditer(body)}
        for match in REGISTER_JUMP.finditer(raw):
            site = int(match[1], 16)
            if site in seen_jumps:
                continue
            seen_jumps.add(site)
            owner_index = bisect.bisect_right(positions, match.start()) - 1
            if owner_index < 0:
                raise ValueError("Indirect jump has no generated owner")
            owner = int(functions[owner_index][1], 16)
            owner_end = positions[owner_index + 1] if owner_index + 1 < len(positions) else len(raw)
            local = {int(m[1], 16) for m in LOCAL_CASE.finditer(raw[positions[owner_index]:owner_end])}
            context = raw[max(0, match.start() - 18000):match.end()]
            starts = sorted({int(m[1], 16) for m in COMMENT_ADDRESS.finditer(context)
                             if site - 192 <= int(m[1], 16) <= site})
            found = None
            for start in starts:
                decoded = list(md.disasm(read(start, site - start + 15), start))
                finish = next((i for i, ins in enumerate(decoded) if ins.address == site), None)
                if finish is not None:
                    found = recognize(decoded[:finish + 1])
                    if found:
                        break
            if not found:
                rejected["not_supported_bounded_RVA_pattern"] += 1
                continue
            try:
                table_bytes = read(found["table"], 4 * found["count"])
            except ValueError:
                rejected["table_outside_file_backed_section"] += 1
                continue
            values = struct.unpack("<" + ("i" if found["signed"] else "I") * found["count"], table_bytes)
            targets = [found["base"] + value for value in values]
            if any(not text_start <= target < text_end for target in targets):
                rejected["table_target_outside_text"] += 1
                continue
            proofs = {target: (dict(kind="current_owner_local_switch") if target in local and target not in known
                               else boundary(target)) for target in set(targets)}
            if any(proof is None for proof in proofs.values()):
                rejected["target_without_known_entry_or_complete_pdata_prefix_decode"] += 1
                unproven.append(dict(jump=hex(site), owner=hex(owner), table=hex(found["table"]),
                                     count=found["count"], bytes=table_bytes.hex(),
                                     unproven_targets=[hex(target) for target, proof in proofs.items() if proof is None],
                                     missing_global_and_local=[hex(target) for target in sorted(set(targets))
                                                               if target not in known and target not in local]))
                continue
            row = dict(jump=hex(site), owner=hex(owner), source=path.relative_to(ROOT).as_posix(),
                       source_line=raw.count(b"\n", 0, match.start()) + 1,
                       compare=hex(found["compare"].address), unsigned_max=found["count"] - 1,
                       range_reject=hex(found["bound"].operands[0].imm), index_register=found["index"],
                       fixed_base=hex(found["base"]), table=hex(found["table"]), count=found["count"],
                       signed_dwords=found["signed"], table_bytes=table_bytes.hex(), table_sha256=sha(table_bytes),
                       instructions=[dict(address=hex(i.address), bytes=i.bytes.hex(), mnemonic=i.mnemonic, operands=i.op_str)
                                     for i in found["instruction_slice"]],
                       entries=[dict(target=hex(target), indices=[n for n, t in enumerate(targets) if t == target],
                                     main=target in parent, supplemental=target in supplemental, local_switch=target in local,
                                     missing_global_and_local=target not in known and target not in local,
                                     boundary=proofs[target]) for target in sorted(set(targets))])
            tables.append(row)
            # Include proved switch edges when evaluating the static closure below.
            adjacency[owner].update(targets)
        if (fi + 1) % 200 == 0:
            print(f"{fi + 1}/1000 sources, {len(tables)} verified bounded table dispatches", flush=True)

    profile_hashes, observed = {}, set()
    for path in sorted((ROOT / "build/windows/profiles").glob("*.json")):
        raw = path.read_bytes()
        profile_hashes[path.relative_to(ROOT).as_posix()] = sha(raw)
        for thread in json.loads(raw).get("threads", []):
            for location in thread.get("locations", {}):
                match = re.search(r"frostpunk-windows-x64\.exe!L_([0-9A-Fa-f]+)\+", location)
                if match:
                    observed.add(int(match[1], 16))
    seeds = {0x141AE13E0, 0x140E02420} | observed
    queue, reachable = deque(seeds), set(seeds)
    while queue:
        for target in adjacency.get(queue.popleft(), ()):
            if target not in reachable:
                reachable.add(target)
                queue.append(target)
    for row in tables:
        owner = int(row["owner"], 16)
        row["static_startup_closure"] = owner in reachable
        row["profile_observed_owner"] = owner in observed
    missing = sorted({entry["target"] for row in tables for entry in row["entries"] if entry["missing_global_and_local"]})
    startup_missing = sorted({entry["target"] for row in tables if row["static_startup_closure"]
                              for entry in row["entries"] if entry["missing_global_and_local"]})
    assert any(row["jump"] == "0x140ec9eb8" and row["count"] == 15 and
               {entry["target"] for entry in row["entries"] if entry["missing_global_and_local"]} == {"0x140ec9f6d", "0x140eca00a"}
               for row in tables), "Observed fifteen-way table must be independently recovered"
    if any(sha(path.read_bytes()) != digest for path, digest in pinned.items()) or sha(original_path.read_bytes()) != ORIGINAL:
        raise ValueError("Original or immutable generated inputs changed during census")
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), original_exe_sha256=ORIGINAL,
                  producer="tools/census_windows_jump_tables.py", producer_sha256=sha(Path(__file__).read_bytes()),
                  input_sha256={path.relative_to(ROOT).as_posix(): digest for path, digest in pinned.items()},
                  profile_sha256=profile_hashes, original_modified=False, generated_modified=False,
                  counts=dict(unique_register_jump_sites=len(seen_jumps), accepted_table_dispatches=len(tables),
                              unique_tables=len({row["table"] for row in tables}),
                              accepted_in_static_startup_closure=sum(row["static_startup_closure"] for row in tables),
                              profile_observed_owners=len(observed), static_closure_nodes=len(reachable),
                              missing_global_and_local=len(missing), startup_closure_missing=len(startup_missing), rejected=dict(rejected)),
                  startup_seeds=[hex(x) for x in sorted(seeds)], missing_targets=missing, startup_closure_missing_targets=startup_missing,
                  tables=tables, unproven_tables=unproven,
                  limitations=["Only original byte-verified register JMP sites present in generated comments are examined; no data-word sweep",
                               "Recognizes only unsigned CMP/JA fallthrough bounds <=256, RIP-relative LEA fixed base, dword indexed MOV/MOVSXD, same-base ADD/JMP with checked register writes",
                               "Startup closure is a conservative direct generated dispatch plus proved switch-edge graph from CRT, WinMain and profile-observed code symbols; reachability is not actual execution",
                               "Missing target means absent main399917/supplemental1985 and absent current owner's local indirect-switch cases; separate Windows extra-AOT coverage is reported by its own receipt",
                               "Existing main/supplemental entries and current owner local switch cases are treated as existing translation boundaries; missing-entry proof is complete linear decoding from containing pdata start to the target, fragment <=16KiB",
                               "Unproven tables are retained separately and excluded from accepted/missing counts; no execution/equivalence proof or AOT generation is claimed"])
    output = ROOT / "research/windows-bounded-jump-tables.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(report=str(output), sha256=sha(output.read_bytes()), counts=report["counts"],
                          missing_targets=missing, startup_closure_missing_targets=startup_missing)), flush=True)


if __name__ == "__main__":
    assert canonical("r10d") == "r10" and canonical("ecx") == "rcx"
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    fixture = bytes.fromhex("83f80e0f878c010000488d0d5a6113ff48984889742440448b848140a0ec004c03c141ffe0")
    decode = lambda raw: list(decoder.disasm(raw, 0x140EC9E96))
    assert recognize(decode(fixture))["table"] == 0x140ECA040
    assert recognize(decode(fixture))["count"] == 15
    assert recognize(decode(fixture.replace(bytes.fromhex("0f87"), bytes.fromhex("0f85")))) is None
    assert recognize(decode(fixture.replace(bytes.fromhex("4898"), b""))) is None
    assert recognize(decode(fixture.replace(bytes.fromhex("4889742440"), bytes.fromhex("b901000000")))) is None
    assert recognize(decode(fixture.replace(bytes.fromhex("4889742440"), bytes.fromhex("e900000000")))) is None
    assert recognize(decode(fixture.replace(bytes.fromhex("4c03c1"), bytes.fromhex("4c03c2")))) is None
    main()
