"""Differential floating SIMD with exact MXCSR ordering and original source byte gates."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

import capstone
import pefile
from leaf_probe import ROOT, windows_toolchain


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    config = json.loads((ROOT / "port.json").read_text())
    original = ROOT / "build/windows/game/Frostpunk.exe"
    assert digest(original) == config["original_exe_sha256"]
    pe = pefile.PE(str(original), fast_load=True)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    source = ROOT / "native/windows/simd_float.c"
    pattern = r'\{UINT64_C\((0x[0-9a-f]+)\), "([^"]+)", \{([^}]+)\}, (\w+), (\d+), (\d+)\}'
    records = re.findall(pattern, source.read_text())
    operations = {"CVTTPD2DQ": 0, "HADDPS": 1}
    assert {name: sum(r[3] == name for r in records) for name in operations} == {"CVTTPD2DQ": 13, "HADDPS": 6}
    assert [int(r[0], 16) for r in records] == sorted({int(r[0], 16) for r in records})
    texts = {index: (ROOT / f"generated/frostpunk-x64/recomp_funcs_{index:04}.c").read_text()
             for index in [965, 975, 982, 986]}
    cases = []
    identity = []
    for address, text, byte_text, operation, destination, src in records:
        va = int(address, 16)
        expected = bytes(int(x, 16) for x in byte_text.split(","))
        actual = pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 4)
        assert actual == expected
        instruction = next(decoder.disasm(actual, va))
        assert instruction.size == 4 and instruction.mnemonic.upper() == operation
        assert instruction.mnemonic + " " + instruction.op_str == text
        assert all(operand.type == capstone.CS_OP_REG for operand in instruction.operands)
        decoded_regs = [int(instruction.reg_name(op.reg)[3:]) for op in instruction.operands]
        assert decoded_regs == [int(destination), int(src)]
        assert any(f'RECOMP_TODO(0x{va:X}, "{text}")' in body for body in texts.values())
        cases.append('    {UINT64_C(%s), "%s", {%s}, %d, %d, %d},' %
                     (address, text, byte_text, operations[operation], *decoded_regs))
        identity.append(dict(va=address, bytes=actual.hex(), instruction=text))
    out = ROOT / "build/windows-simd-float-test"
    out.mkdir(parents=True, exist_ok=True)
    include = out / "simd_float_cases.inc"
    include.write_text("\n".join(cases) + "\n", encoding="utf-8", newline="\n")
    files = [source, ROOT / "native/windows/simd_float.h", ROOT / "native/windows/simd_float_ops.asm",
             ROOT / "native/windows/loader.c", ROOT / "native/windows/loader.h",
             ROOT / "native/runtime64/cpu64.h", ROOT / "tests/windows_simd_float_fixture.c",
             include, Path(__file__).resolve(),
             *[ROOT / f"generated/frostpunk-x64/recomp_funcs_{i:04}.c" for i in texts]]
    before = {str(p): digest(p) for p in files}
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
    for item in [source, ROOT / "native/windows/loader.c", ROOT / "tests/windows_simd_float_fixture.c"]:
        obj = out / (item.stem + ".obj")
        objects.append(obj)
        run([clang, "--target=x86_64-pc-windows-msvc", "-O2", "-std=c11", "-fno-strict-aliasing",
             "-fwrapv", "-ffp-model=strict", "-Wall", "-Wextra", "-I" + str(ROOT / "native/runtime64"),
             "-I" + str(ROOT / "native/windows"), "-I" + str(out), "-c", item, "-o", obj])
    asm = out / "simd_float_asm.obj"
    objects.append(asm)
    run([binaries / "ml64.exe", "/nologo", "/c", "/Fo" + str(asm), ROOT / "native/windows/simd_float_ops.asm"])
    exe = out / "simd-float.exe"
    map_path = out / "simd-float.map"
    run([binaries / "link.exe", "/nologo", "/OUT:" + str(exe), "/MAP:" + str(map_path), *objects,
         "libcmt.lib", "oldnames.lib", "kernel32.lib"])
    native = pefile.PE(str(exe), fast_load=True)
    oracle_ordering = {}
    for name, operation in [("fp_float_oracle_cvt", "cvttpd2dq"), ("fp_float_oracle_hadd", "haddps")]:
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
        oracle_ordering[name] = environment
    result = json.loads(run([exe]))
    assert result["success"] and result["sites"] == 19
    assert before == {p: digest(Path(p)) for p in before}
    report = dict(result=result, oracle_machine_ordering=oracle_ordering, source_identity=identity,
                  source_hashes=before, commands=commands, executable_sha256=digest(exe),
                  original_exe_sha256=config["original_exe_sha256"])
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
