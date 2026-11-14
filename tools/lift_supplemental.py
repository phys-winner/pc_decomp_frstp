"""Emit only exact original supplemental AOT entries after a main-table miss.

This does not modify the parent catalog/generated files or translate at runtime.
Every seed is independently byte-pinned and every dispatch target must already
exist in the sealed parent table or in another explicit supplemental seed.
"""
import argparse
import bisect
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config/Frostpunk.supplemental.json"
IMPORT_CONFIG = ROOT / "config/Frostpunk.supplemental-import-tails.json"
GC_CONFIG = ROOT / "config/Frostpunk.supplemental-lua-gc.json"
WINDOW_CONFIG = ROOT / "config/Frostpunk.supplemental-window-raise.json"
PARENT = ROOT / "generated/frostpunk-x64"
OUTPUT = ROOT / "generated/supplemental"
ORIGINAL = "45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc"
PROOF = {"path":"research/omitted-short-leaves.json", "sha256":"9896c85f1dd7528bfcd7354e67ea6b3baf4d328fc3ae5ddac4ea1cb6894fc400", "producer":"tools/census_short_leaves.py", "producer_sha256":"90ed2cb46c52882208bd35b1520f9b11bcedc24b2bad3edb53e273d86e8e6f56", "entries":1980, "original_code_bytes":32135, "original_LEA_sites":2192}
SEED_VA=0x140DE8740
SEED_BYTES=bytes.fromhex("4883c108e907a20600")
TARGET = re.compile(r"dispatch(?:_jmp)?\(c, 0x([0-9A-Fa-f]+)ull\)")
IMPORT_VA=0x141C04EC0
IMPORT_SLOT=0x141CC7C58
IMPORT_BYTES=bytes.fromhex("488bca48ff258e2d0c00")
WINDOW_VA=0x141B1EF20
WINDOW_SLOT=0x141CC7768
WINDOW_BYTES=bytes.fromhex("488b8ac0000000488b490848ff2536881a00")
GC_CASES = {
    0x141C10160: bytes.fromhex("488b473848894330eb15488b4738488983a0000000eb08488b47384889436848895f38488b742430"),
    0x141C1016A: bytes.fromhex("488b4738488983a0000000eb08488b47384889436848895f38488b742430"),
    0x141C10177: bytes.fromhex("488b47384889436848895f38488b742430"),
}
GC_TABLE = bytes.fromhex("8301c1016001c1015601c101ce00c1016a01c1017701c1011a01c101")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_proof():
    if sha(ROOT/PROOF["path"])!=PROOF["sha256"] or sha(ROOT/PROOF["producer"])!=PROOF["producer_sha256"]:
        raise ValueError("Sealed supplemental census/producer changed")
    proof=json.loads((ROOT/PROOF["path"]).read_text())
    if (proof["original_exe_sha256"]!=ORIGINAL or proof["parent_dispatch_entries"]!=399917 or len(proof["candidates"])!=PROOF["entries"] or
            sum(x["size"] for x in proof["candidates"])!=PROOF["original_code_bytes"] or sum(len(x["validated_original_LEA_sites"]) for x in proof["candidates"])!=PROOF["original_LEA_sites"]):
        raise ValueError("Sealed census count/size/identity differs from approved constants")
    return proof


def validate_config(config,proof):
    if set(config) != {"original_exe_sha256", "proof", "entries"} or config["original_exe_sha256"] != ORIGINAL or config["proof"]!=PROOF:
        raise ValueError("Supplemental config input identity/schema mismatch")
    if not isinstance(config["entries"], list) or len(config["entries"])!=PROOF["entries"]:
        raise ValueError("Explicit supplemental entry count differs from approved census")
    expected={int(x["address"],16):x for x in proof["candidates"]}
    entries = {}
    for entry in config["entries"]:
        if set(entry) != {"va", "size", "bytes", "reason"}:
            raise ValueError("Supplemental entry schema mismatch")
        va, raw = int(entry["va"], 16), bytes.fromhex(entry["bytes"])
        if va in entries or va not in expected or raw.hex()!=expected[va]["original_bytes"] or entry["size"]!=len(raw) or len(raw)!=expected[va]["size"] or not entry["reason"]:
            raise ValueError(f"Unproven, duplicate, changed, or truncated supplemental seed: {va:#x}")
        entries[va] = raw
    if set(entries) != set(expected):
        raise ValueError("Explicit supplemental seed set changed")
    return entries


def validate_tail(raw, va,known):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET, CS_GRP_PRIVILEGE
    from capstone.x86_const import X86_OP_IMM
    decoder = Cs(CS_ARCH_X86, CS_MODE_64); decoder.detail = True
    instructions = list(decoder.disasm(raw, va))
    if not 0<len(raw)<=64 or not instructions or sum(i.size for i in instructions)!=len(raw):
        raise ValueError("Supplemental body must decode fully within64bytes")
    for instruction in instructions[:-1]:
        if any(instruction.group(group) for group in (CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET,CS_GRP_PRIVILEGE)) or instruction.mnemonic in {"ud2","syscall","sysenter","xbegin","xend"}:
            raise ValueError("Supplemental tail wrapper has interior control flow")
    end=instructions[-1]
    if end.mnemonic=="ret" and bytes(end.bytes)==b"\xc3":return None
    if end.mnemonic=="jmp" and len(end.operands)==1 and end.operands[0].type==X86_OP_IMM:
        target=end.operands[0].imm
        if target not in known:raise ValueError("Supplemental tail target is not in sealed parent table")
        return target
    raise ValueError("Supplemental body must end in plainRET or direct parent-knownJMP")


def validate_original(va,raw,evidence,reader,text_bounds,pdata_starts,pdata_ends,known):
    from capstone import Cs,CS_ARCH_X86,CS_MODE_64
    from capstone.x86 import X86_OP_MEM,X86_REG_RIP
    low,high=text_bounds;index=bisect.bisect_right(pdata_starts,va)-1
    if va in known or va&15 or not low<va or va+len(raw)>high or (index>=0 and va<pdata_ends[index]):
        raise ValueError(f"Supplemental boundary/catalogue proof fails: {va:#x}")
    if reader(va,len(raw))!=raw or hashlib.sha256(raw).hexdigest()!=evidence["original_bytes_sha256"] or reader(va-1,1)!=b"\xcc":
        raise ValueError(f"Original supplemental bytes/precedingCC changed: {va:#x}")
    padding_end=(va+len(raw)+15)&~15
    if padding_end>high:raise ValueError("Supplemental padding leaves.text")
    padding=reader(va+len(raw),padding_end-va-len(raw))
    if padding!=b"\xcc"*len(padding) or padding.hex()!=evidence["trailing_padding"]["bytes"] or len(padding)!=evidence["trailing_padding"]["size"]:
        raise ValueError(f"Supplemental trailingCC proof fails: {va:#x}")
    tail=validate_tail(raw,va,known);expected=evidence["terminal"]
    if (tail is None and expected!={"kind":"plain_RET"}) or (tail is not None and expected!={"kind":"direct_unconditional_JMP","target":hex(tail),"target_in_parent_dispatch":True}):
        raise ValueError("Supplemental exact terminal differs from sealed proof")
    if not evidence["validated_original_LEA_sites"]:raise ValueError("Supplemental entry lacks originalLEA evidence")
    md=Cs(CS_ARCH_X86,CS_MODE_64);md.detail=True
    for site in evidence["validated_original_LEA_sites"]:
        address=int(site["original_site"],16);expected_bytes=bytes.fromhex(site["instruction_bytes"]);actual=reader(address,len(expected_bytes));instructions=list(md.disasm(actual,address))
        if actual!=expected_bytes or len(instructions)!=1:raise ValueError("Original LEA site bytes changed")
        inst=instructions[0]
        if inst.mnemonic!="lea" or len(inst.operands)!=2 or inst.operands[1].type!=X86_OP_MEM or inst.operands[1].mem.base!=X86_REG_RIP or inst.address+inst.size+inst.operands[1].mem.disp!=va:
            raise ValueError("Original LEA site does not name supplemental entry")
    return tail


def validate_targets(body, known):
    targets = {int(value, 16) for value in TARGET.findall(body)}
    if "RECOMP_TODO" in body or len(re.findall(r"dispatch(?:_jmp)?\(c,", body)) != len(TARGET.findall(body)):
        raise ValueError("Supplemental output has unsupported semantics or indirect dispatch")
    if targets - known:
        raise ValueError(f"Supplemental output has unknown static targets: {sorted(targets-known)}")
    return targets


def validate_import_config(config):
    expected=dict(va=hex(IMPORT_VA),size=10,bytes=IMPORT_BYTES.hex(),IAT_slot=hex(IMPORT_SLOT),
                  dll="api-ms-win-crt-heap-l1-1-0.dll",name="free")
    if set(config)!={"original_exe_sha256","proof","entries"}or config["original_exe_sha256"]!=ORIGINAL or config["entries"]!=[expected]:
        raise ValueError("Only the observed exact zfree import-tail seed is admitted")
    proof=config["proof"]
    if set(proof)!={"path","sha256","producer","producer_sha256"}or proof["path"]!="research/zfree-import-tail.json"or proof["producer"]!="tools/inspect_zfree_thunk.py":
        raise ValueError("Import-tail proof identity/schema changed")
    if sha(ROOT/proof["path"])!=proof["sha256"]or sha(ROOT/proof["producer"])!=proof["producer_sha256"]:
        raise ValueError("Import-tail proof/producer changed")
    evidence=json.loads((ROOT/proof["path"]).read_text())
    if(evidence["original_exe_sha256"]!=ORIGINAL or evidence["va"]!=hex(IMPORT_VA)or evidence["bytes"]!=IMPORT_BYTES.hex()or evidence["IAT_slot"]!=hex(IMPORT_SLOT)):
        raise ValueError("Import-tail original evidence changed")
    return proof


def validate_import_body(body):
    # This exact CheckedLifter output may read only the original free IAT and
    # tail-dispatch its current value. General indirect supplemental entries
    # remain rejected by validate_targets and the sealed1980 validator.
    if("RECOMP_TODO"in body or body.count("SET64(c->rcx, c->rdx);")!=1 or
       body.count("_ind = rd64(GVA(0x141CC7C58))")!=1 or body.count("dispatch_jmp(c, _ind);")!=1 or
       "dispatch(c,"in body or re.findall(r"GVA\(([^)]+)\)",body)!=["0x141CC7C58"]):
        raise ValueError("Checked import-tail translation differs from exact MOV/IAT-JMP contract")


def validate_window_config(config):
    expected=dict(va=hex(WINDOW_VA),size=18,bytes=WINDOW_BYTES.hex(),IAT_slot=hex(WINDOW_SLOT),dll="USER32.dll",name="SetForegroundWindow")
    if set(config)!={"original_exe_sha256","proof","entries"} or config["original_exe_sha256"]!=ORIGINAL or config["entries"]!=[expected]:
        raise ValueError("Only the observed exact SDL RaiseWindow import-tail seed is admitted")
    proof=config["proof"]
    if set(proof)!={"path","sha256","producer","producer_sha256"} or proof["path"]!="research/sdl-raise-window-tail.json" or proof["producer"]!="tools/inspect_sdl_raise_window_tail.py":
        raise ValueError("SDL RaiseWindow proof identity/schema changed")
    if sha(ROOT/proof["path"])!=proof["sha256"] or sha(ROOT/proof["producer"])!=proof["producer_sha256"]:
        raise ValueError("SDL RaiseWindow proof/producer changed")
    evidence=json.loads((ROOT/proof["path"]).read_text())
    if evidence["original_exe_sha256"]!=ORIGINAL or evidence["va"]!=hex(WINDOW_VA) or evidence["bytes"]!=WINDOW_BYTES.hex() or evidence["IAT_slot"]!=hex(WINDOW_SLOT):
        raise ValueError("SDL RaiseWindow original evidence changed")
    return proof


def validate_window_body(body):
    if ("RECOMP_TODO" in body or body.count("SET64(c->rcx, rd64((c->rdx + 0xC0ull)));")!=1 or body.count("SET64(c->rcx, rd64((c->rcx + 0x8ull)));")!=1 or
        body.count("_ind = rd64(GVA(0x141CC7768))")!=1 or body.count("dispatch_jmp(c, _ind);")!=1 or
        "dispatch(c," in body or re.findall(r"GVA\(([^)]+)\)",body)!=["0x141CC7768"]):
        raise ValueError("Checked SDL RaiseWindow translation differs from exact pointer-chain/IAT-JMP contract")


def validate_gc_config(config):
    expected=[dict(va=hex(va),size=len(raw),bytes=raw.hex(),tail="0x141c10188") for va,raw in GC_CASES.items()]
    if set(config)!={"original_exe_sha256","proof","entries"} or config["original_exe_sha256"]!=ORIGINAL or config["entries"]!=expected:
        raise ValueError("Only the three exact original Lua GC table suffixes are admitted")
    proof=config["proof"]
    if set(proof)!={"path","sha256","producer","producer_sha256"} or proof["path"]!="research/lua-gc-jump-table.json" or proof["producer"]!="tools/inspect_lua_gc_jump.py":
        raise ValueError("Lua GC proof identity/schema changed")
    if sha(ROOT/proof["path"])!=proof["sha256"] or sha(ROOT/proof["producer"])!=proof["producer_sha256"]:
        raise ValueError("Lua GC proof/producer changed")
    evidence=json.loads((ROOT/proof["path"]).read_text())
    if (evidence["original_exe_sha256"]!=ORIGINAL or evidence["observed_target"]!="0x141c1016a" or
            evidence["table"]["va"]!="0x141c10194" or evidence["table"]["bytes"]!=GC_TABLE.hex() or
            {int(x["va"],16):bytes.fromhex(x["bytes"]) for x in evidence["entries"]}!=GC_CASES):
        raise ValueError("Lua GC proof differs from exact original table/entries")
    return proof,evidence


def self_test():
    proof=load_proof();config=json.loads(CONFIG.read_text());entries=validate_config(config,proof)
    assert len(entries)==1980 and entries[SEED_VA]==SEED_BYTES
    import_config=json.loads(IMPORT_CONFIG.read_text());validate_import_config(import_config)
    gc_config=json.loads(GC_CONFIG.read_text());validate_gc_config(gc_config)
    window_config=json.loads(WINDOW_CONFIG.read_text());validate_window_config(window_config)
    for key,value in [("va","0x141b1ef21"),("bytes",WINDOW_BYTES[:-1].hex()),("IAT_slot","0x141cc7770"),("name","ShowWindow")]:
        bad=copy.deepcopy(window_config);bad["entries"][0][key]=value
        try:validate_window_config(bad)
        except ValueError:pass
        else:raise AssertionError("Unobserved SDL RaiseWindow config accepted")
    for key,value in [("va","0x141c1016b"),("bytes","488b4f38"),("tail","0x141c10183"),("size",1)]:
        bad=copy.deepcopy(gc_config);bad["entries"][1][key]=value
        try:validate_gc_config(bad)
        except ValueError:pass
        else:raise AssertionError("Unproven Lua GC suffix config accepted")
    for key,value in [("va","0x141c04ec1"),("bytes","488bcb48ff258e2d0c00"),("IAT_slot","0x141cc7c60"),("name","malloc")]:
        bad=copy.deepcopy(import_config);bad["entries"][0][key]=value
        try:validate_import_config(bad)
        except ValueError:pass
        else:raise AssertionError("Unobserved import-tail config accepted")
    assert validate_tail(SEED_BYTES,SEED_VA,{0x140E52950})==0x140E52950
    assert validate_tail(b"\xc3",SEED_VA,set()) is None
    malformed = []
    seed_index=next(i for i,e in enumerate(config["entries"]) if int(e["va"],16)==SEED_VA)
    changed = copy.deepcopy(config); changed["entries"][seed_index]["bytes"] = "4983c108e907a20600"; malformed.append(changed)
    truncated = copy.deepcopy(config); truncated["entries"][seed_index]["bytes"] = "4883c108e907a206"; malformed.append(truncated)
    size = copy.deepcopy(config); size["entries"][seed_index]["size"] = 8; malformed.append(size)
    unknown = copy.deepcopy(config); unknown["entries"][seed_index]["va"] = "0x140de8741"; malformed.append(unknown)
    duplicate = copy.deepcopy(config); duplicate["entries"].append(copy.deepcopy(duplicate["entries"][0])); malformed.append(duplicate)
    wrong_proof=copy.deepcopy(config);wrong_proof["proof"]["entries"]=1;malformed.append(wrong_proof)
    for bad in malformed:
        try:
            validate_config(bad,proof)
        except ValueError:
            pass
        else:
            raise AssertionError("Malformed supplemental config accepted")
    for raw,known in ((SEED_BYTES[:-1],{0x140E52950}),(bytes.fromhex("e800000000e900000000"),{SEED_VA+10}),(bytes.fromhex("c20800"),set()),(SEED_BYTES,set()),(bytes.fromhex("cce900000000"),{SEED_VA+6})):
        try:
            validate_tail(raw,SEED_VA,known)
        except ValueError:
            pass
        else:
            raise AssertionError("Truncated/non-tail wrapper accepted")
    assert validate_targets("dispatch(c, 0x140E52950ull); return;", {0x140E52950}) == {0x140E52950}
    for body in ("dispatch(c, 0x140E52950ull);", "dispatch(c, c->rax);", 'RECOMP_TODO(0x140DE8740, "x");'):
        try:
            validate_targets(body, set())
        except ValueError:
            pass
        else:
            raise AssertionError("Unknown/indirect/unsupported dispatch accepted")


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("--self-test", action="store_true"); args = parser.parse_args()
    self_test()
    if args.self_test:
        print("Supplemental config/bytes/truncation/unknown-target self-tests PASS"); return
    config_hash = sha(CONFIG)
    import_config=json.loads(IMPORT_CONFIG.read_text());import_proof=validate_import_config(import_config)
    gc_config=json.loads(GC_CONFIG.read_text());gc_proof,gc_evidence=validate_gc_config(gc_config)
    window_config=json.loads(WINDOW_CONFIG.read_text());window_proof=validate_window_config(window_config)
    proof=load_proof();config = json.loads(CONFIG.read_text()); entries = validate_config(config,proof)
    parent_path = PARENT / "recomp_meta.json"; parent = json.loads(parent_path.read_text()); parent_hash = sha(parent_path)
    if parent.get("original_exe_sha256") != ORIGINAL or not parent.get("original_entry_preserved"):
        raise ValueError("Missing checked parent image/entry identity")
    for name, expected in parent["generator_sources"].items():
        if sha(ROOT / name) != expected: raise ValueError(f"Parent generator changed: {name}")
    if sha(ROOT / parent["catalog"]) != parent["catalog_sha256"]:
        raise ValueError("Parent catalog changed")
    dispatch = PARENT / "recomp_dispatch.c"
    if sha(dispatch) != parent["generated_files"].get(dispatch.name) or sha(dispatch)!=proof["parent_dispatch_sha256"]: raise ValueError("Parent dispatch table changed")
    known = {int(value, 16) for value in re.findall(r"\{ 0x([0-9A-Fa-f]+)ull,", dispatch.read_text())}
    if len(known)!=399917 or len(known) != parent["functions"] or known & entries.keys():
        raise ValueError("Parent dispatch count mismatch or supplemental entry already present")
    cfg = json.loads((ROOT / "port.json").read_text()); exe = Path(cfg["game_directory"]) / "Frostpunk.exe"
    if sha(exe) != ORIGINAL: raise ValueError("Original executable changed")
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    from lift64_checked import CheckedLifter
    data, base, image_size, _, sections, *_ = generate64.read_pe(exe); reader = generate64.make_reader(data, base, sections)
    section=next(s for s in sections if s[0]==".text");text_bounds=(base+section[1],base+section[1]+min(section[2],section[4]))
    pe_offset=struct.unpack_from("<I",data,0x3c)[0];pd_rva,pd_size=struct.unpack_from("<II",data,pe_offset+24+112+3*8);pd_offset=generate64.rva_to_off(sections,pd_rva)
    if pd_offset is None or pd_size%12:raise ValueError("Original exception directory invalid")
    pdata=sorted((base+a,base+b)for a,b,_ in struct.iter_unpack("<III",data[pd_offset:pd_offset+pd_size]))
    if any(a>=b for a,b in pdata)or any(pdata[i][0]<pdata[i-1][1]for i in range(1,len(pdata))):raise ValueError("Original pdata invalid/overlapping")
    starts=[a for a,b in pdata];ends=[b for a,b in pdata]
    sources = dict(parent["generator_sources"])
    for path in [Path(__file__).resolve(), CONFIG, IMPORT_CONFIG, GC_CONFIG, WINDOW_CONFIG, ROOT/window_proof["path"],ROOT/window_proof["producer"], ROOT/gc_proof["path"],ROOT/gc_proof["producer"], ROOT/import_proof["path"],ROOT/import_proof["producer"], ROOT/PROOF["path"],ROOT/PROOF["producer"], *sorted((ROOT / "native/runtime64").glob("*.h"))]:
        sources[str(path.relative_to(ROOT))] = sha(path)
    if sources[str(CONFIG.relative_to(ROOT))] != config_hash:
        raise ValueError("Supplemental config changed while parsing")
    lifter = CheckedLifter(image_base=base, image_size=image_size, read_va=reader, original_sha256=ORIGINAL)
    bodies, evidence = [], [];evidence_by_va={int(x["address"],16):x for x in proof["candidates"]}
    for va, raw in sorted(entries.items()):
        if reader(va, len(raw)) != raw: raise ValueError(f"Original supplemental bytes changed: {va:#x}")
        item=evidence_by_va[va];tail_target=validate_original(va,raw,item,reader,text_bounds,starts,ends,known)
        body = lifter.lift_function(raw, va, name=f"frostpunk_supplemental_{va:012X}")
        targets = validate_targets(body,known)
        if targets != ({tail_target}if tail_target is not None else set()): raise ValueError("Original/generated branch target mismatch")
        bodies.append("static " + body)
        evidence.append(dict(va=hex(va), size=len(raw), bytes=raw.hex(), sha256=hashlib.sha256(raw).hexdigest(), targets=[hex(x) for x in sorted(targets)], name=f"frostpunk_supplemental_{va:012X}",original_LEA_sites=len(item["validated_original_LEA_sites"])))
    # One independently proven observed allocator ABI adapter, kept separate
    # from the unchanged sealed1980 LEA/plainRET/direct-parent-JMP grammar.
    if IMPORT_VA in known or IMPORT_VA in entries or reader(IMPORT_VA,10)!=IMPORT_BYTES or reader(IMPORT_VA-1,1)!=b"\xcc"or reader(IMPORT_VA+10,6)!=b"\xcc"*6:
        raise ValueError("Import-tail original bytes/boundaries/catalog mismatch")
    ix=bisect.bisect_right(starts,IMPORT_VA)-1
    if not text_bounds[0]<IMPORT_VA<IMPORT_VA+10<=text_bounds[1]or (ix>=0 and IMPORT_VA<ends[ix]):
        raise ValueError("Import-tail is not an aligned gap outside pdata")
    import_pe=__import__("pefile").PE(data=data,fast_load=True);import_pe.parse_data_directories(directories=[1])
    original_imports={x.address:(d.dll.decode(),x.name.decode())for d in import_pe.DIRECTORY_ENTRY_IMPORT for x in d.imports if x.name}
    if original_imports.get(IMPORT_SLOT)!=("api-ms-win-crt-heap-l1-1-0.dll","free"):
        raise ValueError("Original import-tail slot does not identify CRT free")
    body=lifter.lift_function(IMPORT_BYTES,IMPORT_VA,name=f"frostpunk_supplemental_{IMPORT_VA:012X}");validate_import_body(body)
    bodies.append("static "+body);entries[IMPORT_VA]=IMPORT_BYTES
    evidence.append(dict(va=hex(IMPORT_VA),size=10,bytes=IMPORT_BYTES.hex(),sha256=hashlib.sha256(IMPORT_BYTES).hexdigest(),targets=[],name=f"frostpunk_supplemental_{IMPORT_VA:012X}",IAT_slot=hex(IMPORT_SLOT),IAT_import="api-ms-win-crt-heap-l1-1-0.dll!free",observed_import_tail=True))
    # This leaf immediately follows a pdata function's RET, not CC padding.
    # Admit only its exact observed bytes; keep the generic1980 grammar intact.
    ix=bisect.bisect_right(starts,WINDOW_VA)-1
    if (WINDOW_VA in known or WINDOW_VA in entries or (starts[ix],ends[ix])!=(0x141B1EEE0,WINDOW_VA) or
        starts[ix+1]!=0x141B1EF40 or reader(WINDOW_VA-1,1)!=b"\xc3" or reader(WINDOW_VA,18)!=WINDOW_BYTES or
        reader(WINDOW_VA+18,14)!=b"\xcc"*14 or original_imports.get(WINDOW_SLOT)!=("USER32.dll","SetForegroundWindow")):
        raise ValueError("Original SDL RaiseWindow bytes/boundaries/import/catalog mismatch")
    body=lifter.lift_function(WINDOW_BYTES,WINDOW_VA,name=f"frostpunk_supplemental_{WINDOW_VA:012X}");validate_window_body(body)
    bodies.append("static "+body);entries[WINDOW_VA]=WINDOW_BYTES
    evidence.append(dict(va=hex(WINDOW_VA),size=18,bytes=WINDOW_BYTES.hex(),sha256=hashlib.sha256(WINDOW_BYTES).hexdigest(),targets=[],name=f"frostpunk_supplemental_{WINDOW_VA:012X}",IAT_slot=hex(WINDOW_SLOT),IAT_import="USER32.dll!SetForegroundWindow",observed_window_raise_tail=True))
    # These are original basic-block entries in one existing unwind fragment,
    # reached by its bounded seven-way RVA table, not new function prologues.
    # Exact suffix extents preserve the partially allocated original frame.
    if reader(0x141C10194,28)!=GC_TABLE or 0x141C10188 not in known:
        raise ValueError("Original Lua GC table/parent epilog changed")
    if reader(0x141C100C0,14)!=bytes.fromhex("48988b8c869401c1014803ceffe1"):
        raise ValueError("Original Lua GC indirect table dispatch changed")
    for va,raw in GC_CASES.items():
        ix=bisect.bisect_right(starts,va)-1
        if va in known or va in entries or (starts[ix],ends[ix])!=(0x141C10156,0x141C10188) or reader(va,len(raw))!=raw or va+len(raw)!=0x141C10188:
            raise ValueError("Original Lua GC suffix/catalogue/unwind extent changed")
        body=lifter.lift_function(raw,va,name=f"frostpunk_supplemental_{va:012X}")
        if validate_targets(body,known)!={0x141C10188}:
            raise ValueError("Lua GC suffix does not tail to the existing exact parent epilog")
        bodies.append("static "+body);entries[va]=raw
        evidence.append(dict(va=hex(va),size=len(raw),bytes=raw.hex(),sha256=hashlib.sha256(raw).hexdigest(),targets=["0x141c10188"],name=f"frostpunk_supplemental_{va:012X}",original_table=hex(0x141C10194),original_table_index=[i for i,x in enumerate(struct.unpack("<7I",GC_TABLE)) if base+x==va],inside_original_unwind_fragment=True,observed_jump_table_closure=True))
    evidence.sort(key=lambda item:int(item["va"],16))
    OUTPUT.mkdir(parents=True, exist_ok=True); marker = OUTPUT / "recomp_meta.json"; marker.unlink(missing_ok=True)
    header = '#pragma once\n#include "cpu64.h"\ntypedef void (*frostpunk_supplemental_fn)(CPU *);\nfrostpunk_supplemental_fn frostpunk_supplemental_lookup(uint64_t target);\n'
    (OUTPUT / "recomp_supplemental.h").write_text(header)
    code = '#include "recomp_supplemental.h"\n\n' + "\n".join(bodies)
    code += '\ntypedef struct {uint64_t va; frostpunk_supplemental_fn function;} supplemental_entry;\nstatic const supplemental_entry entries[]={\n'
    code += "".join(f"    {{0x{va:X}ull,frostpunk_supplemental_{va:012X}}},\n" for va in sorted(entries)) + '};\n'
    code += 'frostpunk_supplemental_fn frostpunk_supplemental_lookup(uint64_t target) {\n    unsigned low=0,high=(unsigned)(sizeof(entries)/sizeof(*entries));\n    while(low<high){unsigned mid=low+(high-low)/2;if(entries[mid].va<target)low=mid+1;else high=mid;}\n    return low<sizeof(entries)/sizeof(*entries)&&entries[low].va==target?entries[low].function:0;\n}\n'
    (OUTPUT / "recomp_supplemental.c").write_text(code)
    generated = {name: sha(OUTPUT / name) for name in ("recomp_supplemental.c", "recomp_supplemental.h")}
    if (OUTPUT / "recomp_supplemental.c").read_text() != code or (OUTPUT / "recomp_supplemental.h").read_text() != header:
        raise ValueError("Supplemental output changed during emission")
    if sha(parent_path) != parent_hash or sha(exe) != ORIGINAL or any(sha(ROOT/name) != expected for name, expected in sources.items()):
        raise ValueError("Supplemental inputs changed during emission")
    if any(sha(OUTPUT / name) != expected for name, expected in generated.items()):
        raise ValueError("Supplemental generated files changed before sealing")
    metadata = dict(original_exe_sha256=ORIGINAL, parent_metadata_sha256=parent_hash, parent_metadata=str(parent_path.relative_to(ROOT)), generator_sources=sources, generated_files=generated, entries=evidence, no_runtime_cpu_translation=True, original_entry_preserved=True, game_boot_proven=False,sealed_census=PROOF,sealed_census_entries=1980,observed_import_tail_entries=1,observed_import_tail_proof=import_proof,observed_window_raise_entries=1,observed_window_raise_proof=window_proof,observed_gc_table_entries=3,observed_gc_table_proof=gc_proof,original_code_bytes=sum(map(len,entries.values())))
    marker.write_text(json.dumps(metadata, indent=2)+"\n")
    print(json.dumps(dict(entries=len(entries), translation_units=1, parent_metadata_sha256=parent_hash, metadata=str(marker)), indent=2))


if __name__ == "__main__": main()
