"""Extract the exact encrypted wrapper DLL as data, optionally lift/compile it.

No guest instruction executes, no entry is redirected, and no Steam API result
is substituted. This is the pinned Frostpunk wrapper's 32-round XTEA-CBC loop.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

import reference64
from inspect_bind64 import decode_header
from lift64_checked import CheckedLifter, ORIGINAL_SHA256

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "generated/payload"
MASK = 0xFFFFFFFF


def decrypt_xtea_cbc(data, key):
    if len(data) % 8 or len(key) != 4:
        raise ValueError("Invalid XTEA block/key length")
    result, previous = bytearray(), (0x55555555, 0x55555555)
    for at in range(0, len(data), 8):
        cipher = struct.unpack_from("<II", data, at)
        left, right = cipher
        total = 0xC6EF3720
        for _ in range(32):
            right = (right - ((((left << 4) ^ (left >> 5)) + left) ^
                              (total + key[(total >> 11) & 3]))) & MASK
            total = (total - 0x9E3779B9) & MASK
            left = (left - ((((right << 4) ^ (right >> 5)) + right) ^
                            (total + key[total & 3]))) & MASK
        result.extend(struct.pack("<II", left ^ previous[0], right ^ previous[1]))
        previous = cipher
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--compile", action="store_true", help="Generate and compile private payload objects")
    args = parser.parse_args()
    config = json.loads((ROOT / "port.json").read_text())
    original = Path(config["game_directory"]) / "Frostpunk.exe"
    if reference64.digest(original) != ORIGINAL_SHA256:
        raise ValueError("Original executable changed; rederive the payload layout")
    reference64.prepare_reference()
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    import pefile
    raw, base, _, entry, sections, *_ = generate64.read_pe(original)
    reader = generate64.make_reader(raw, base, sections)
    header = decode_header(reader(base + entry - 0xF0, 0xF0))
    offset, size = struct.unpack_from("<II", header, 0x30)
    bind_va = base + entry - struct.unpack_from("<I", header, 0x18)[0]
    bind = next(section for section in sections if section[0] == ".bind")
    if bind_va != base + bind[1] or offset + size > bind[2] or size % 8:
        raise ValueError("Payload range is outside the original .bind section")
    # Exact original decoder evidence: key=header+98, IV=55555555 twice,
    # sum=C6EF3720, 32 rounds, subtraction delta=9E3779B9, CBC XOR.
    proof = {0x144507388: "488b442438480598000000",
             0x14450745B: "c78424a00000002037efc6",
             0x1445074E7: "8b8424a00000002db979379e"}
    for va, encoded in proof.items():
        if reader(va, len(bytes.fromhex(encoded))) != bytes.fromhex(encoded):
            raise ValueError(f"Original XTEA evidence changed at {va:#x}")
    key = struct.unpack_from("<4I", header, 0x98)
    encrypted = reader(bind_va + offset, size)
    plain = decrypt_xtea_cbc(encrypted, key)
    payload = pefile.PE(data=plain)
    if payload.FILE_HEADER.Machine != 0x8664 or payload.OPTIONAL_HEADER.ImageBase != 0x180000000:
        raise ValueError("Decrypted payload is not the expected PE64 module")
    exports = [{"name": symbol.name.decode() if symbol.name else None,
                "rva": hex(symbol.address), "va": hex(payload.OPTIONAL_HEADER.ImageBase + symbol.address)}
               for symbol in payload.DIRECTORY_ENTRY_EXPORT.symbols]
    if exports != [{"name": "steam", "rva": "0x3b00", "va": "0x180003b00"}]:
        raise ValueError("Payload export identity changed")
    (OUT / "input").mkdir(parents=True, exist_ok=True)
    dll = OUT / "input/SteamDRMP.dll"
    dll.write_bytes(plain)
    text_section = next(section for section in payload.sections if section.Name.startswith(b".text\0"))
    text_bytes = payload.get_data(text_section.VirtualAddress, text_section.Misc_VirtualSize)
    expected_header = ('#pragma once\n#include <stdint.h>\n'
                       f'#define FROSTPUNK_PAYLOAD_BASE UINT64_C(0x{payload.OPTIONAL_HEADER.ImageBase:X})\n'
                       f'#define FROSTPUNK_PAYLOAD_SIZE 0x{payload.OPTIONAL_HEADER.SizeOfImage:X}u\n'
                       f'#define FROSTPUNK_PAYLOAD_TEXT_RVA 0x{text_section.VirtualAddress:X}u\n'
                       f'#define FROSTPUNK_PAYLOAD_SHA256 "{reference64.digest(dll)}"\n'
                       'static const unsigned char frostpunk_payload_expected_text[] = {\n' +
                       '\n'.join('    ' + ','.join(f'0x{value:02x}' for value in text_bytes[at:at + 16]) + ','
                                 for at in range(0, len(text_bytes), 16)) + '\n};\n')
    (OUT / "payload_expected.h").write_text(expected_header)
    pd = payload.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    ranges = reference64.unwind_ranges(payload.get_data(pd.VirtualAddress, pd.Size),
                                      payload.OPTIONAL_HEADER.SizeOfImage)
    module_base = payload.OPTIONAL_HEADER.ImageBase
    bounds = {module_base + begin: (end - begin, f"pdata_{module_base + begin:X}")
              for begin, end in ranges.items()}
    # Both exported function and DllMain entry are already exact unwind entries.
    for rva in (payload.OPTIONAL_HEADER.AddressOfEntryPoint, 0x3B00):
        if module_base + rva not in bounds:
            raise ValueError(f"Payload required entry has no unwind extent: {rva:#x}")
    catalog = OUT / "payload.functions.txt"
    catalog.write_text("# Extracted original payload unwind ranges; recovery incomplete.\n" +
                       "".join(f"0x{va:X} {size:X} {name}\n" for va, (size, name) in sorted(bounds.items())))
    imports = [{"slot": hex(symbol.address),
                "dll": entry.dll.decode(),
                "name": symbol.name.decode() if symbol.name else f"#{symbol.ordinal}"}
               for entry in payload.DIRECTORY_ENTRY_IMPORT for symbol in entry.imports]
    report = {"original_exe_sha256": ORIGINAL_SHA256, "payload_sha256": reference64.digest(dll),
              "payload_file": str(dll.relative_to(ROOT)), "encrypted_va": hex(bind_va + offset),
              "encrypted_bytes": size, "cipher": "XTEA32-CBC, little-endian words, IV 55555555:55555555",
              "key_header_offset": "0x98", "instruction_evidence": {hex(va): value for va, value in proof.items()},
              "image_base": hex(module_base), "image_size": payload.OPTIONAL_HEADER.SizeOfImage,
              "entry_va": hex(module_base + payload.OPTIONAL_HEADER.AddressOfEntryPoint),
              "exports": exports, "pdata_records": len(ranges), "imports": imports,
              "original_modified": False, "guest_cpu_executed": False,
              "license_checks_changed": False, "catalog_complete": False, "game_boot_proven": False}
    if args.generate or args.compile:
        generated = OUT / "recomp"
        generated.mkdir(exist_ok=True)
        (generated / "recomp_meta.json").unlink(missing_ok=True)
        generate64.Lifter = CheckedLifter  # generic fixes; no Frostpunk-specific wrapper overrides
        generate64.HLE_BASE = 0xE5E60000
        saved = sys.argv
        sys.argv = ["generate64", str(dll), str(catalog), str(generated)]
        try:
            generate64.main()
        finally:
            sys.argv = saved
        for name in ("recomp_dispatch.c", "recomp_dispatch.h", "recomp_imports.c"):
            path = generated / name
            text = path.read_text()
            for symbol in ("recomp_dispatch_table", "recomp_dispatch_count", "recomp_imports"):
                text = text.replace(symbol, "payload_" + symbol)
            path.write_text(text)
        metadata_path = generated / "recomp_meta.json"
        metadata = json.loads(metadata_path.read_text())
        metadata["payload_sha256"] = report["payload_sha256"]
        metadata["hle_base"] = "0xe5e60000"
        metadata["symbol_prefix"] = "payload_"
        metadata["generator_sources"] = {str(path.relative_to(ROOT)): reference64.digest(path) for path in (
            Path(__file__).resolve(), ROOT / "tools/lift64_checked.py",
            ROOT / "reference/pcrecomp/tools/lift/generate64.py",
            ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py",
            ROOT / "reference/pcrecomp/tools/lift/lift32_cpu.py")}
        metadata["generated_files"] = {path.name: reference64.digest(path) for path in sorted(generated.glob("recomp_*.*"))
                                       if path.suffix in (".c", ".h")}
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        report["generation"] = {k: value for k, value in metadata.items() if k not in ("generated_files", "generator_sources")}
        if args.compile:
            toolchain = Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin"
            objects = OUT / "objects"
            objects.mkdir(exist_ok=True)
            inputs = sorted(generated.glob("recomp_funcs_*.c")) + [generated / "recomp_dispatch.c", generated / "recomp_imports.c"]
            flags = ["--target=aarch64-linux-android30", "-O2", "-std=c11", "-fPIC", "-fno-strict-aliasing", "-fwrapv",
                     "-ffp-contract=off", "-fno-math-errno", "-g0", "-w", "-I" + str(ROOT / "native/runtime64"), "-I" + str(generated)]
            compiled = []
            for source in inputs:
                obj = objects / (source.stem + ".o")
                subprocess.run([str(toolchain / "clang.exe"), *flags, "-c", str(source), "-o", str(obj)], check=True)
                with obj.open("rb") as stream:
                    elf = stream.read(20)
                if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[18:20], "little") != 183:
                    raise ValueError("Payload object is not ELF64 AArch64")
                compiled.append({"source": str(source.relative_to(ROOT)), "source_sha256": reference64.digest(source),
                                 "object": str(obj.relative_to(ROOT)), "object_sha256": reference64.digest(obj)})
            report["compilation"] = {"target": "aarch64-linux-android30", "objects_only": True, "flags": flags,
                                     "compiler_sha256": reference64.digest(toolchain / "clang.exe"),
                                     "runtime_headers": {p.name: reference64.digest(p) for p in (ROOT / "native/runtime64").glob("*.h")},
                                     "objects": compiled, "success": True}
    target = ROOT / "research/bind-payload.json"
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: value for k, value in report.items() if k not in ("imports", "compilation")}, indent=2))


if __name__ == "__main__":
    main()
