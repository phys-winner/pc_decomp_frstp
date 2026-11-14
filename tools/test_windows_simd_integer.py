"""Check pinned integer SIMD sites against compiled native intrinsics, never game bytes."""
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
    source = ROOT / "native/windows/simd_integer.c"
    pattern = r'\{UINT64_C\((0x[0-9a-f]+)\), "([^"]+)", \{([^}]+)\}, (\d+), (\w+), (\d+), (\d+)\}'
    records = re.findall(pattern, source.read_text())
    operations = {name: i for i, name in enumerate(["PMULLD", "PMOVZXBD", "PABSD", "PSADBW", "PMOVSXDQ"])}
    expected_counts = {"PMULLD": 25, "PMOVZXBD": 6, "PABSD": 4, "PSADBW": 2, "PMOVSXDQ": 8}
    assert {name: sum(r[4] == name for r in records) for name in operations} == expected_counts
    assert [int(r[0], 16) for r in records] == sorted({int(r[0], 16) for r in records})
    cases = []
    identity = []
    main_text = {}
    for address, text, byte_text, length, operation, destination, src in records:
        va = int(address, 16)
        expected = bytes(int(x, 16) for x in byte_text.split(","))
        actual = pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, len(expected))
        assert actual == expected and len(actual) == int(length)
        instruction = next(decoder.disasm(actual, va))
        assert instruction.size == len(actual)
        assert instruction.mnemonic.upper() == operation
        assert instruction.mnemonic + " " + instruction.op_str == text
        assert all(operand.type == capstone.CS_OP_REG for operand in instruction.operands)
        decoded_regs = [int(instruction.reg_name(op.reg)[3:]) for op in instruction.operands]
        assert decoded_regs == [int(destination), int(src)]
        candidates = [974, 977, 980, 985, 986]
        found = False
        for index in candidates:
            path = ROOT / f"generated/frostpunk-x64/recomp_funcs_{index:04}.c"
            if index not in main_text:
                main_text[index] = path.read_text()
            if f'RECOMP_TODO(0x{va:X}, "{text}")' in main_text[index]:
                found = True
                break
        assert found, hex(va)
        cases.append('    {UINT64_C(%s), "%s", {%s}, %d, %d, %d, %d},' %
                     (address, text, byte_text, instruction.size, operations[operation], *decoded_regs))
        identity.append(dict(va=address, bytes=actual.hex(), instruction=text))
    out = ROOT / "build/windows-simd-integer-test"
    out.mkdir(parents=True, exist_ok=True)
    include = out / "simd_integer_cases.inc"
    include.write_text("\n".join(cases) + "\n", encoding="utf-8", newline="\n")
    files = [source, ROOT / "native/windows/simd_integer.h",
             ROOT / "native/windows/loader.c", ROOT / "native/windows/loader.h",
             ROOT / "native/runtime64/cpu64.h", ROOT / "tests/windows_simd_integer_fixture.c",
             include, Path(__file__).resolve(),
             *[ROOT / f"generated/frostpunk-x64/recomp_funcs_{i:04}.c" for i in main_text]]
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
    for item in [source, ROOT / "native/windows/loader.c", ROOT / "tests/windows_simd_integer_fixture.c"]:
        obj = out / (item.stem + ".obj")
        objects.append(obj)
        run([clang, "--target=x86_64-pc-windows-msvc", "-O2", "-std=c11", "-fno-strict-aliasing",
             "-fwrapv", "-ffp-contract=off", "-Wall", "-Wextra", "-I" + str(ROOT / "native/runtime64"),
             "-I" + str(ROOT / "native/windows"), "-I" + str(out), "-c", item, "-o", obj])
    exe = out / "simd-integer.exe"
    run([binaries / "link.exe", "/nologo", "/OUT:" + str(exe), *objects,
         "libcmt.lib", "oldnames.lib", "kernel32.lib"])
    result = json.loads(run([exe]))
    assert result["success"] and result["sites"] == 45
    assert before == {p: digest(Path(p)) for p in before}
    report = dict(result=result, source_identity=identity, source_hashes=before,
                  commands=commands, executable_sha256=digest(exe),
                  original_exe_sha256=config["original_exe_sha256"])
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
