"""Validate the Windows packed-compare overlay and its strict native intrinsic oracle."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import capstone
import pefile
from leaf_probe import ROOT, windows_toolchain
from generate_windows_packed_compare_overlay import generate


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", choices=["hot9", "all"], default="hot9")
    args = parser.parse_args()
    manifest, manifest_path = generate(args.scope)
    config = json.loads((ROOT / "port.json").read_text())
    out = ROOT / "build/windows-packed-compare-test"
    out.mkdir(parents=True, exist_ok=True)
    cases = []
    functions = []
    for site in manifest["sites"]:
        symbol = site["va"][2:]
        functions.append(f'static void repaired_{symbol}(CPU *c) {{ {site["new_code"]} }}')
        functions.append(f'static void legacy_{symbol}(CPU *c) {{ {site["old_code"]} }}')
        cases.append('    {UINT64_C(%s),%d,%d,%s,%d,%d,UINT64_C(%s),UINT64_C(%s),repaired_%s,legacy_%s},' %
                     (site["va"], site["destination"], site.get("source", 0), site["operation"],
                      site["source_kind"] == "memory", site.get("source_gpr", -2), hex(site.get("source_disp", 0)),
                      site.get("source_va") or "0x0", symbol, symbol))
    include = out / "packed_compare_overlay_cases.inc"
    include.write_text("\n".join(functions) + "\nstatic const compare_site cases[]={\n" + "\n".join(cases) + "\n};\n", encoding="utf-8", newline="\n")
    sources = [ROOT / "native/windows/simd_float.c", ROOT / "native/windows/loader.c",
               ROOT / "tests/windows_packed_compare_fixture.c"]
    asm_source = ROOT / "native/windows/simd_float_ops.asm"
    dependencies = [*sources, asm_source, ROOT / "native/windows/simd_float.h", ROOT / "native/windows/loader.h",
                    ROOT / "native/runtime64/cpu64.h", ROOT / manifest["generator"]["path"], Path(__file__).resolve()]
    watched = [*dependencies, *[ROOT / unit[key]["path"] for unit in manifest["units"] for key in ["canonical", "overlay"]], include]
    before = {str(p): digest(p) for p in watched}
    binaries, env = windows_toolchain()
    clang = Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    commands = []
    def run(command):
        commands.append(list(map(str, command)))
        p = subprocess.run(commands[-1], env=env, cwd=out, capture_output=True,
                           text=True, encoding="utf-8", errors="replace", timeout=60)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        return p.stdout
    objects = []
    for source in sources:
        obj = out / (source.stem + ".obj")
        objects.append(obj)
        run([clang, "--target=x86_64-pc-windows-msvc", "-O2", "-std=c11", "-fno-strict-aliasing", "-fwrapv",
             "-ffp-model=strict", "-Wall", "-Wextra", "-I" + str(ROOT / "native/runtime64"),
             "-I" + str(ROOT / "native/windows"), "-I" + str(out), "-c", source, "-o", obj])
    asm = out / "simd_float_ops.obj"
    objects.append(asm)
    run([binaries / "ml64.exe", "/nologo", "/c", "/Fo" + str(asm), asm_source])
    exe = out / "packed-compare.exe"
    map_path = out / "packed-compare.map"
    run([binaries / "link.exe", "/nologo", "/OUT:" + str(exe), "/MAP:" + str(map_path), *objects,
         "libcmt.lib", "oldnames.lib", "kernel32.lib"])
    native = pefile.PE(str(exe), fast_load=True)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    ordering = {};common_ordering = {}
    for suffix, operation in [("eqps", "cmpeqps"), ("ltps", "cmpltps"), ("eqpd", "cmpeqpd"),
                              ("ltpd", "cmpltpd"), ("nlepd", "cmpnlepd")]:
        name = "fp_compare_oracle_" + suffix
        match = re.search(r"\s" + name + r"\s+([0-9a-fA-F]{16})\s", map_path.read_text())
        assert match, name
        address = int(match[1], 16)
        instructions = []
        for ins in decoder.disasm(native.get_data(address - native.OPTIONAL_HEADER.ImageBase, 256), address):
            instructions.append(ins.mnemonic)
            if ins.mnemonic == "ret":
                break
        environment = [m for m in instructions if m in ["stmxcsr", "ldmxcsr", operation]]
        assert environment == ["stmxcsr", "ldmxcsr", operation, "stmxcsr", "ldmxcsr"], (name, instructions)
        ordering[name] = environment
    for suffix, operation in [("eqps", "cmpeqps"), ("ltps", "cmpltps")]:
        name = "fp_cmp" + suffix + "_native"
        match = re.search(r"\s" + name + r"\s+([0-9a-fA-F]{16})\s", map_path.read_text())
        assert match, name
        address = int(match[1], 16)
        fast = []
        for ins in decoder.disasm(native.get_data(address - native.OPTIONAL_HEADER.ImageBase, 256), address):
            fast.append(ins)
            if ins.mnemonic == "ret":break
        environment = [ins.mnemonic for ins in fast if ins.mnemonic in ["stmxcsr", "ldmxcsr", operation]]
        assert environment == [operation], (name, environment)
        assert not any(re.search(r"\bxmm(?:[6-9]|1[0-5])\b", ins.op_str) for ins in fast), name
        branches = [ins for ins in fast if ins.mnemonic.startswith("j")]
        assert len(branches) == 1 and branches[0].mnemonic == "jne", name
        fallback_address = int(branches[0].op_str, 16)
        assert fallback_address == fast[-1].address + fast[-1].size, name
        fallback = []
        for ins in decoder.disasm(native.get_data(fallback_address - native.OPTIONAL_HEADER.ImageBase, 128), fallback_address):
            fallback.append(ins.mnemonic)
            if ins.mnemonic == "ret":break
        environment_fallback = [m for m in fallback if m in ["stmxcsr", "ldmxcsr", operation]]
        assert environment_fallback == ["stmxcsr", "ldmxcsr", operation, "stmxcsr", "ldmxcsr"], name
        common_ordering[name] = dict(fast=environment, fallback=environment_fallback, volatile_XMM_only=True)
    result = json.loads(run([exe]))
    assert result["success"] and result["overlay_sites"] == len(manifest["sites"])
    assert result["legacy_wrong_sites"] == len(manifest["sites"])
    assert result["common_compare_cases"] > 0
    assert before == {p: digest(Path(p)) for p in before}
    report = dict(result=result, oracle_machine_ordering=ordering, common_path_machine_ordering=common_ordering, source_hashes=before,
                  commands=commands, executable_sha256=digest(exe))
    report_path = out / "result.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    manifest["success"] = True
    manifest["dependencies"] = {str(p.relative_to(ROOT)).replace("\\", "/"): digest(p) for p in dependencies}
    manifest["validation"] = dict(case_count=result["native_full_CPU_cases"],
                                  receipt=str(report_path.relative_to(ROOT)).replace("\\", "/"),
                                  receipt_sha256=digest(report_path), **result)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(**result, manifest_sha256=digest(manifest_path)), indent=2))


if __name__ == "__main__":
    main()
