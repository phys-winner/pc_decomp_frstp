"""Prepare pinned pcrecomp and original unwind/wrapper/proven-leaf catalogs.

    python tools/reference64.py
    python tools/reference64.py --generate

The Steam installation and The Void reference are read-only inputs. Unwind
ranges and explicit .bind/leaf entry ranges do not establish complete recovery.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT.parent / "the_void/reference/pcrecomp"
VENDOR = ROOT / "reference/pcrecomp"
REVISION = "904ac3129d0c9d05efbeec19a1dce6abbe7f5d26"
# Original callback: RCX points to a 12-byte object, zero its first uint64,
# store binary32(1.0) at +8, return RCX. LEA R9 at 0x1412FC073 passes this
# constructor to the original EH-vector helper with stride16/count6.
# Upstream plausible_code reads only16 bytes: its third instruction ends at
# byte17, so only2 instructions decode and the >=3-instruction filter rejects
# it. Keep this exact proven seed rather than widening the generic heuristic.
LEAF_BYTES = {0x1412FCEC0: bytes.fromhex("48c70100000000488bc1c741080000803fc3")}
FILES = {
    "LICENSE": "a03f354a044f9acea7de6044746edb9f4d2c811c185adcc74f985348a36268c9",
    "tools/lift/generate64.py": "bf00af55f4b6a12760aacfa89252096e6e9e2cd48a0f0de062d1ba5f9da215ef",
    "tools/lift/lift64_cpu.py": "e8310bf1fdc3dcd64ea9416d7028eabaeb44ae3329bd94d7d9c81424d1249cc5",
    "tools/lift/lift32_cpu.py": "c63a3c2e4bc6060d4c8f20db9a53563c6bf6a8a916ca1f4126243499a061ab59",
    "tools/lift/difftest64.py": "fd702dfd09bcac5d343abbd383b7503a10e897a1a6dd219172c374205bc65947",
    "tools/lift/difftest64.asm": "90d8dcc1a5674414801ad7059b0ce97f72840f2e3d8597bac52a18d69fb95a20",
    "runtime/recomp64_cpu/cpu64.h": "966c88a35642be1bd12f11ff141f8c71d6fb314c16c31847bcff8513519e26d2",
    "runtime/recomp64_cpu/eh64.c": "d394b0abe10e3e7951dae1daa10e61f556676d4c7b9f7257f033330da38c3226",
    "runtime/recomp64_cpu/eh64.h": "e5c313e51dbab542d6eabf7b9a79c9c9a4325eb89c2cfb35b17328fbcd9ab4a9",
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare_reference():
    for name, expected in FILES.items():
        source, target = REFERENCE / name, VENDOR / name
        if not target.exists():
            if digest(source) != expected:
                raise ValueError(f"Reference hash mismatch: {source}")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        if digest(target) != expected:
            raise ValueError(f"Pinned dependency changed: {target}")
    (VENDOR / "provenance.json").write_text(json.dumps({
        "url": "https://github.com/sp00nznet/pcrecomp.git",
        "revision": REVISION, "license": "MIT", "sha256": FILES,
        "note": "Minimal unmodified file subset copied from the local The Void reference.",
    }, indent=2) + "\n", encoding="utf-8")


def unwind_ranges(raw, image_size):
    if len(raw) % 12:
        raise ValueError("Exception directory is not a RUNTIME_FUNCTION array")
    ranges = {}
    for start, end, unwind in struct.iter_unpack("<III", raw):
        if not (0 < start < end <= image_size and 0 < unwind < image_size):
            raise ValueError(f"Invalid unwind range: {start:#x}..{end:#x}, unwind={unwind:#x}")
        if start in ranges and ranges[start] != end:
            raise ValueError(f"Conflicting unwind range at {start:#x}")
        ranges[start] = end
    return ranges


def validate_straight_leaf(raw, va):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    instructions = list(decoder.disasm(raw, va))
    if (not instructions or sum(i.size for i in instructions) != len(raw)
            or instructions[-1].mnemonic != "ret" or instructions[-1].op_str):
        raise ValueError(f"Explicit leaf is truncated or lacks a final plain RET at {va:#x}")
    for instruction in instructions[:-1]:
        if any(instruction.group(group) for group in (CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, CS_GRP_IRET)):
            raise ValueError(f"Explicit leaf has interior control flow at {instruction.address:#x}")
    return instructions


def validate_leaf_bytes(va, raw):
    if va not in LEAF_BYTES or raw != LEAF_BYTES[va]:
        raise ValueError(f"Explicit original leaf bytes changed at {va:#x}")
    return validate_straight_leaf(raw, va)


def self_test():
    assert unwind_ranges(struct.pack("<III", 0x1000, 0x1010, 0x2000), 0x3000) == {0x1000: 0x1010}
    for raw in (b"x", struct.pack("<III", 0x1010, 0x1000, 0x2000),
                struct.pack("<III", 0x1000, 0x3010, 0x2000)):
        try:
            unwind_ranges(raw, 0x3000)
        except ValueError:
            pass
        else:
            raise AssertionError("Invalid exception directory accepted")
    va, raw = next(iter(LEAF_BYTES.items()))
    assert len(validate_leaf_bytes(va, raw)) == 4
    # Reproduce the discovery omission without relaxing the upstream filter.
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    assert len(list(Cs(CS_ARCH_X86, CS_MODE_64).disasm(raw[:16], va))) == 2
    for changed in (raw[:-1], raw[:6] + b"\x01" + raw[7:], b"\xc3"):
        try:
            validate_leaf_bytes(va, changed)
        except ValueError:
            pass
        else:
            raise AssertionError("Unpinned leaf bytes accepted")
    for nonleaf in (bytes.fromhex("e800000000c3"), bytes.fromhex("eb00c3"), b"\xc3\xc3", b"\xc3\x90", b"\x48\xc7"):
        try:
            validate_straight_leaf(nonleaf, va)
        except ValueError:
            pass
        else:
            raise AssertionError("Non-leaf or truncated input accepted")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path(
        r"C:\Program Files (x86)\Steam\steamapps\common\Frostpunk\Frostpunk.exe"))
    parser.add_argument("--generate", action="store_true", help="Lift the entire bootstrap catalog")
    args = parser.parse_args()
    self_test()
    prepare_reference()
    sys.path.insert(0, str(VENDOR / "tools/lift"))
    import generate64
    from lift64_checked import ORIGINAL_SHA256, BIND_RANGES
    data, base, image_size, entry, sections, *_ = generate64.read_pe(args.exe)
    if hashlib.sha256(data).hexdigest() != ORIGINAL_SHA256:
        raise ValueError("Original executable changed; rederive explicit .bind ranges")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    if machine != 0x8664:
        raise ValueError(f"Expected x86-64 PE, got {machine:#x}")
    rva, size = struct.unpack_from("<II", data, pe + 24 + 112 + 3 * 8)
    offset = generate64.rva_to_off(sections, rva)
    if offset is None or not size or offset + size > len(data):
        raise ValueError("Missing or truncated exception directory")
    ranges = unwind_ranges(data[offset:offset + size], image_size)
    catalog = ROOT / "config/Frostpunk.pdata.functions.txt"
    catalog.parent.mkdir(parents=True, exist_ok=True)
    # shortcut: unwind ranges omit frameless leaves; close static targets and
    # compare against an IDA catalog before treating this as complete recovery.
    catalog.write_text("# Bootstrap unwind ranges; function recovery is incomplete.\n" + "".join(
        f"0x{base + start:X}  {end - start:X}  pdata_{base + start:X}\n"
        for start, end in sorted(ranges.items())), encoding="utf-8")
    bind_catalog = ROOT / "config/Frostpunk.bind.functions.txt"
    explicit = generate64.load_bounds(bind_catalog)
    if {va: size_name[0] for va, size_name in explicit.items()} != dict(BIND_RANGES):
        raise ValueError("Explicit .bind catalog disagrees with the hash-pinned original ranges")
    if base + entry != BIND_RANGES[0][0]:
        raise ValueError("Explicit .bind catalog does not preserve the supplied PE entry")
    leaf_catalog = ROOT / "config/Frostpunk.leaf.functions.txt"
    leaves = generate64.load_bounds(leaf_catalog)
    if {va: size_name[0] for va, size_name in leaves.items()} != {va: len(raw) for va, raw in LEAF_BYTES.items()}:
        raise ValueError("Explicit leaf catalog disagrees with exact-byte-pinned ranges")
    read_va = generate64.make_reader(data, base, sections)
    leaf_evidence = []
    for va, (size, name) in sorted(leaves.items()):
        if not any(section[0] == ".text" and base + section[1] <= va and va + size <= base + section[1] + section[2] for section in sections):
            raise ValueError(f"Explicit leaf is outside the original .text: {va:#x}")
        if any(base + start < va + size and va < base + end for start, end in ranges.items()):
            raise ValueError(f"Explicit leaf unexpectedly overlaps .pdata: {va:#x}")
        raw = read_va(va, size)
        instructions = validate_leaf_bytes(va, raw)
        leaf_evidence.append({"va": hex(va), "size": size, "name": name,
                              "bytes": raw.hex(), "sha256": hashlib.sha256(raw).hexdigest(),
                              "instructions": [i.mnemonic + (" " + i.op_str if i.op_str else "") for i in instructions],
                              "original_lea_reference": "0x1412fc073", "original_vector_helper_call": "0x1412fc083"})
    combined = ROOT / "config/Frostpunk.functions.txt"
    combined.write_text(catalog.read_text() + "\n# Explicit original wrapper entry and loader.\n" +
                        "".join(f"0x{va:X}  {size:X}  {name}\n"
                                for va, (size, name) in sorted(explicit.items())) +
                        "\n# Exact-byte-pinned original indirect-call leaves.\n" +
                        "".join(f"0x{va:X}  {size:X}  {name}\n"
                                for va, (size, name) in sorted(leaves.items())), encoding="utf-8")
    catalog_sources = {str(path.relative_to(ROOT)): digest(path) for path in (catalog, bind_catalog, leaf_catalog)}
    report = {
        "input": str(args.exe.resolve()), "sha256": hashlib.sha256(data).hexdigest(),
        "image_base": hex(base), "entry": hex(base + entry), "image_size": image_size,
        "entry_section": next((s[0] for s in sections if s[1] <= entry < s[1] + s[2]), None),
        "unwind_ranges": len(ranges), "pdata_catalog": str(catalog.relative_to(ROOT)),
        "explicit_bind_ranges": len(explicit), "catalog": str(combined.relative_to(ROOT)),
        "explicit_leaf_ranges": len(leaves), "explicit_leaf_seeds": leaf_evidence,
        "catalog_sources": catalog_sources,
        "catalog_sha256": digest(combined),
        "catalog_complete": False, "pcrecomp_revision": REVISION,
    }
    out = ROOT / "research/pdata-catalog.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if args.generate:
        subprocess.run([sys.executable, str(ROOT / "tools/lift64_checked.py"),
                        str(args.exe), str(combined), str(ROOT / "generated/frostpunk-x64")], check=True)
        # Seal the explicit input catalogs as well as the combined catalog.
        # Existing object/link builders verify every generator_sources path.
        meta_path = ROOT / "generated/frostpunk-x64/recomp_meta.json"
        metadata = json.loads(meta_path.read_text())
        meta_path.unlink()
        if any(digest(ROOT / name) != expected for name, expected in catalog_sources.items()):
            raise ValueError("Explicit catalogs changed during generation")
        metadata["generator_sources"].update(catalog_sources)
        metadata.update(catalog_sources=catalog_sources, explicit_leaf_seeds=leaf_evidence)
        meta_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        print("[*] Exact original leaf/catalog provenance sealed", flush=True)


if __name__ == "__main__":
    main()
