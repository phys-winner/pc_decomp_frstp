"""Lift a pinned Win64 dependency from data, using a private DIR64-rebased image.

    python tools/lift_dependency.py --compile

The original system DLL is never executed or modified. This preserves exported
C++ method implementations; Windows/UCRT imports and guest EH remain host work.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

import pefile
import reference64
from lift64_checked import CheckedLifter

ROOT = Path(__file__).resolve().parents[1]
MSVCP_SHA256 = "7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca"


def compile_flags(generated):
    return ["--target=aarch64-linux-android30", "-O2", "-std=c11", "-fPIC", "-fno-strict-aliasing", "-fwrapv",
            "-ffp-contract=off", "-fno-math-errno", "-g0", "-w", "-I" + str(ROOT / "native/runtime64"), "-I" + str(generated)]


def checked_compilation(compilation, generated, clang):
    if (not compilation["success"] or compilation["target"] != "aarch64-linux-android30"
            or compilation["compiler_sha256"] != reference64.digest(clang)
            or compilation["flags"] != compile_flags(generated)):
        raise ValueError("Dependency compiler/options mismatch")
    pins, objects = [], []
    for name, expected in compilation["runtime_headers"].items():
        path = ROOT / "native/runtime64" / name
        if reference64.digest(path) != expected:
            raise ValueError("Dependency runtime header changed: " + name)
        pins.append(path)
    sources = set(generated.glob("recomp_funcs_*.c")) | {generated / "recomp_dispatch.c", generated / "recomp_imports.c"}
    recorded = {ROOT / row["source"] for row in compilation["objects"]}
    if recorded != sources or len(recorded) != len(compilation["objects"]):
        raise ValueError("Dependency compile receipt omits/duplicates generated sources")
    for row in compilation["objects"]:
        source, obj = ROOT / row["source"], ROOT / row["object"]
        if reference64.digest(source) != row["source_sha256"] or reference64.digest(obj) != row["object_sha256"]:
            raise ValueError("Dependency source/object changed: " + obj.name)
        with obj.open("rb") as stream:
            elf = stream.read(20)
        if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[16:18], "little") != 1 or int.from_bytes(elf[18:20], "little") != 183:
            raise ValueError("Dependency object is not ELF64 AArch64 ET_REL")
        objects.append(obj)
        pins.extend((source, obj))
    return objects, pins


def verified_objects(clang):
    contract_path = ROOT / "research/msvcp140-loader-contract.json"
    receipt_path = ROOT / "research/msvcp140-dependency.json"
    meta_path = ROOT / "generated/dependencies/msvcp140/recomp/recomp_meta.json"
    contract = json.loads(contract_path.read_text())
    report, metadata = json.loads(receipt_path.read_text()), json.loads(meta_path.read_text())
    if (reference64.digest(receipt_path) != contract["receipt_sha256"]
            or reference64.digest(meta_path) != contract["metadata_sha256"]
            or report["original_sha256"] != MSVCP_SHA256 or report["missing_game_exports"]
            or metadata["private_sha256"] != report["private_sha256"]):
        raise ValueError("Dependency identity/metadata contract changed")
    pins = [contract_path, receipt_path, meta_path]
    for path, expected in ((ROOT / report["original_copy"], report["original_sha256"]),
                           (ROOT / report["private_copy"], report["private_sha256"]),
                           (meta_path.parent.parent / "functions.txt", metadata["catalog_sha256"])):
        if reference64.digest(path) != expected:
            raise ValueError("Dependency input image/catalog changed: " + str(path))
        pins.append(path)
    for name, expected in metadata["generator_sources"].items():
        path = ROOT / name
        if reference64.digest(path) != expected:
            raise ValueError("Dependency generator changed: " + name)
        pins.append(path)
    for name, expected in metadata["generated_files"].items():
        path = meta_path.parent / name
        if Path(name).name != name or reference64.digest(path) != expected:
            raise ValueError("Dependency generated source/header changed: " + name)
        pins.append(path)
    for name, expected in contract["loader_inputs"].items():
        path = ROOT / name
        if reference64.digest(path) != expected:
            raise ValueError("Dependency loader header/helper changed: " + name)
        pins.append(path)
    objects, compile_pins = checked_compilation(report["compilation"], meta_path.parent, clang)
    if len(objects) != contract["object_count"] or len(objects) != metadata["translation_units"] + 2:
        raise ValueError("Dependency object count differs from completed generation")
    return objects, pins + compile_pins, contract


def rebase_dir64(original, new_base):
    pe = pefile.PE(data=original)
    if pe.FILE_HEADER.Machine != 0x8664 or new_base % 0x10000:
        raise ValueError("Expected PE64 and 64-KiB-aligned private image base")
    delta = new_base - pe.OPTIONAL_HEADER.ImageBase
    result, changed = bytearray(original), []
    for block in pe.DIRECTORY_ENTRY_BASERELOC:
        for item in block.entries:
            if item.type == 0:
                continue
            if item.type != 10:
                raise ValueError(f"Unsupported base relocation type {item.type}")
            offset = pe.get_offset_from_rva(item.rva)
            value = struct.unpack_from("<Q", original, offset)[0]
            if not 0 <= value + delta < 1 << 64:
                raise ValueError("Relocation overflows a guest pointer")
            struct.pack_into("<Q", result, offset, value + delta)
            changed.append(item.rva)
    struct.pack_into("<Q", result, pe.OPTIONAL_HEADER.get_field_absolute_offset("ImageBase"), new_base)
    # Rebase and its inverse must preserve every original byte, including code.
    check = bytearray(result)
    for rva in changed:
        offset = pe.get_offset_from_rva(rva)
        struct.pack_into("<Q", check, offset, struct.unpack_from("<Q", result, offset)[0] - delta)
    struct.pack_into("<Q", check, pe.OPTIONAL_HEADER.get_field_absolute_offset("ImageBase"), pe.OPTIONAL_HEADER.ImageBase)
    if bytes(check) != original:
        raise ValueError("Private relocation changed bytes outside loader relocation fields")
    return bytes(result), changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("C:/Windows/System32/msvcp140.dll"))
    parser.add_argument("--sha256", default=MSVCP_SHA256)
    parser.add_argument("--base", type=lambda value: int(value, 0), default=0x1A0000000)
    parser.add_argument("--hle-base", type=lambda value: int(value, 0), default=0xE5E80000)
    parser.add_argument("--generate", action="store_true")
    parser.add_argument("--compile", action="store_true")
    args = parser.parse_args()
    name = args.source.stem.lower()
    if not re.fullmatch(r"[a-z0-9_]+", name):
        raise ValueError("Dependency name must be a simple library stem")
    if reference64.digest(args.source) != args.sha256:
        raise ValueError("System dependency changed; review and pin its new identity")
    reference64.prepare_reference()
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    out = ROOT / "generated/dependencies" / name
    previous_path = ROOT / "research" / (name + "-dependency.json")
    previous = json.loads(previous_path.read_text()) if previous_path.exists() else {}
    (out / "input").mkdir(parents=True, exist_ok=True)
    source = args.source.read_bytes()
    original_file = out / "input" / args.source.name
    original_file.write_bytes(source)
    relocated, relocation_rvas = rebase_dir64(source, args.base)
    private = out / "input" / (name + ".static.dll")
    private.write_bytes(relocated)
    pe = pefile.PE(data=relocated)
    pd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
    unwind = reference64.unwind_ranges(pe.get_data(pd.VirtualAddress, pd.Size), pe.OPTIONAL_HEADER.SizeOfImage)
    bounds = {begin: (end - begin, f"pdata_{args.base + begin:X}") for begin, end in unwind.items()}
    exports, code_exports = [], set()
    for symbol in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        section = pe.get_section_by_rva(symbol.address)
        executable = bool(section and section.Characteristics & 0x20000000 and not symbol.forwarder)
        exports.append({"name": symbol.name.decode() if symbol.name else None, "ordinal": symbol.ordinal,
                        "rva": hex(symbol.address), "va": hex(args.base + symbol.address), "code": executable,
                        "forwarder": symbol.forwarder.decode() if symbol.forwarder else None})
        if executable:
            code_exports.add(symbol.address)
    starts = sorted(set(bounds) | code_exports)
    for rva in sorted(code_exports):
        if rva in bounds:
            continue
        index = bisect.bisect_right(sorted(unwind), rva) - 1
        begin = sorted(unwind)[index] if index >= 0 else None
        if begin is not None and rva < unwind[begin]:
            end = unwind[begin]
        else:
            section = pe.get_section_by_rva(rva)
            index = bisect.bisect_right(starts, rva)
            end = min(starts[index] if index < len(starts) else section.VirtualAddress + section.Misc_VirtualSize,
                      section.VirtualAddress + section.Misc_VirtualSize)
        bounds[rva] = (min(end - rva, 0x10000), f"export_{args.base + rva:X}")
    if pe.OPTIONAL_HEADER.AddressOfEntryPoint not in bounds:
        raise ValueError("DLL entry needs an independently bounded root")
    catalog = out / "functions.txt"
    catalog.write_text("# Original unwind ranges + code export roots; recovery incomplete.\n" +
                       "".join(f"0x{args.base + rva:X} {size:X} {label}\n"
                               for rva, (size, label) in sorted(bounds.items())))
    imports = [{"dll": descriptor.dll.decode(), "name": symbol.name.decode() if symbol.name else f"#{symbol.ordinal}",
                "slot": hex(symbol.address)} for descriptor in pe.DIRECTORY_ENTRY_IMPORT for symbol in descriptor.imports]
    game = json.loads((ROOT / "research/input-report.json").read_text())
    game_imports = [row for row in game["imports"] if row["dll"].lower() == args.source.name.lower()]
    exported_names = {row["name"] for row in exports}
    missing = [row["name"] for row in game_imports if row["name"] not in exported_names]
    report = {"source": str(args.source), "original_sha256": args.sha256,
              "original_copy": str(original_file.relative_to(ROOT)), "private_copy": str(private.relative_to(ROOT)),
              "private_sha256": reference64.digest(private), "original_base": hex(pefile.PE(data=source).OPTIONAL_HEADER.ImageBase),
              "static_base": hex(args.base), "entry_va": hex(args.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint),
              "image_size": pe.OPTIONAL_HEADER.SizeOfImage, "dir64_relocations": len(relocation_rvas),
              "rebase_inverse_exact_original": True, "exports": exports, "imports": imports,
              "pdata_ranges": len(unwind), "initial_catalog_ranges": len(bounds),
              "game_imports": game_imports, "missing_game_exports": missing,
              "source_modified": False, "guest_cpu_executed": False, "game_boot_proven": False,
              "limits": ["Original C++ ABI is preserved through guest CPU state, not host C++ ABI.",
                         "Dependencies, DllMain, static TLS, CFG pointers and cross-module EH still need runtime integration."]}
    if missing:
        raise ValueError(f"Selected dependency omits required exports: {missing}")
    if args.generate or args.compile:
        generated = out / "recomp"
        generated.mkdir(exist_ok=True)
        (generated / "recomp_meta.json").unlink(missing_ok=True)
        generate64.Lifter = CheckedLifter
        generate64.HLE_BASE = args.hle_base
        saved = sys.argv
        sys.argv = ["generate64", str(private), str(catalog), str(generated)]
        try:
            generate64.main()
        finally:
            sys.argv = saved
        for file_name in ("recomp_dispatch.c", "recomp_dispatch.h", "recomp_imports.c"):
            path = generated / file_name
            text = path.read_text()
            for symbol in ("recomp_dispatch_table", "recomp_dispatch_count", "recomp_imports"):
                text = text.replace(symbol, name + "_" + symbol)
            path.write_text(text)
        meta_path = generated / "recomp_meta.json"
        metadata = json.loads(meta_path.read_text())
        metadata.update({"original_sha256": args.sha256, "private_sha256": report["private_sha256"],
                         "symbol_prefix": name + "_", "hle_base": hex(args.hle_base),
                         "catalog_sha256": reference64.digest(catalog),
                         "generator_sources": {str(path.relative_to(ROOT)): reference64.digest(path) for path in (
                             Path(__file__).resolve(), ROOT / "tools/lift64_checked.py",
                             ROOT / "reference/pcrecomp/tools/lift/generate64.py",
                             ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py", ROOT / "reference/pcrecomp/tools/lift/lift32_cpu.py")},
                         "generated_files": {path.name: reference64.digest(path) for path in sorted(generated.glob("recomp_*.*"))
                                             if path.suffix in (".c", ".h")}})
        meta_path.write_text(json.dumps(metadata, indent=2) + "\n")
        report["generation"] = {key: value for key, value in metadata.items() if key not in ("generator_sources", "generated_files")}
        trap_sites = []
        for path in generated.glob("recomp_funcs_*.c"):
            trap_sites.extend(re.findall(r'RECOMP_TODO\(0x([0-9A-Fa-f]+), "([^"]*)"\)', path.read_text()))
        report["unsupported_mnemonics"] = dict(collections.Counter(ins.split()[0].rstrip(":") for _, ins in trap_sites))
        if args.compile:
            config = json.loads((ROOT / "port.json").read_text())
            clang = Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
            objects = out / "objects"
            objects.mkdir(exist_ok=True)
            sources = sorted(generated.glob("recomp_funcs_*.c")) + [generated / "recomp_dispatch.c", generated / "recomp_imports.c"]
            flags = compile_flags(generated)
            compiled = []
            for source in sources:
                obj = objects / (source.stem + ".o")
                subprocess.run([str(clang), *flags, "-c", str(source), "-o", str(obj)], check=True)
                with obj.open("rb") as stream:
                    elf = stream.read(20)
                if elf[:6] != b"\x7fELF\x02\x01" or int.from_bytes(elf[18:20], "little") != 183:
                    raise ValueError("Dependency object is not ELF64 AArch64")
                compiled.append({"source": str(source.relative_to(ROOT)), "source_sha256": reference64.digest(source),
                                 "object": str(obj.relative_to(ROOT)), "object_sha256": reference64.digest(obj)})
            report["compilation"] = {"target": "aarch64-linux-android30", "success": True, "objects_only": True,
                                     "compiler_sha256": reference64.digest(clang), "flags": flags,
                                     "runtime_headers": {p.name: reference64.digest(p) for p in (ROOT / "native/runtime64").glob("*.h")},
                                     "objects": compiled}
        elif "compilation" in previous:
            config = json.loads((ROOT / "port.json").read_text())
            clang = Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
            reused, _ = checked_compilation(previous["compilation"], generated, clang)
            report["compilation"] = dict(previous["compilation"], objects_reused_after_verified_identical_generation=len(reused))
    target = ROOT / "research" / (name + "-dependency.json")
    target.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key not in ("exports", "imports", "game_imports", "compilation")}, indent=2))


if __name__ == "__main__":
    main()
