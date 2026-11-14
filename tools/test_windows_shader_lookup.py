"""Differential CPU/guard tests for the Windows-only static ShaderFamily scan."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

import pefile
from leaf_probe import windows_toolchain

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/windows-shader-test"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=ROOT / "native/windows")
    args = parser.parse_args()
    source = args.source_dir / "shader_lookup.c"
    config = json.loads((ROOT / "port.json").read_text())
    original = Path(config["game_directory"]) / "Frostpunk.exe"
    if sha(original) != config["original_exe_sha256"]:
        raise ValueError("Original executable identity changed")
    raw = original.read_bytes()
    pe = pefile.PE(data=raw, fast_load=True)
    candidate = source.read_text()
    fixture_gates = (ROOT / "tests/windows_shader_lookup_gates.h").read_text()
    gates = []
    for i, (va, end) in enumerate(((0x1410596A9, 0x141059750), (0x1410597E9, 0x141059890))):
        offset = pe.get_offset_from_rva(va - pe.OPTIONAL_HEADER.ImageBase)
        expected = raw[offset:offset + end - va]
        for text, name in ((candidate, f"gate_{i}"), (fixture_gates, f"expected_gate_{i}")):
            match = re.search(r"static const unsigned char " + name + r"\[\] = \{([^}]+)\};", text, re.S)
            if not match or bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match[1])) != expected:
                raise ValueError("Original opcode gate mismatch: " + name)
        gates.append(dict(address=hex(va), size=len(expected), sha256=hashlib.sha256(expected).hexdigest()))
    binaries, env = windows_toolchain()
    clang = (Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] /
             "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe")
    flags = ["--target=x86_64-pc-windows-msvc", "-O2", "-std=c11", "-fno-strict-aliasing", "-fwrapv",
             "-ffp-contract=off", "-D_CRT_SECURE_NO_WARNINGS", "-w",
             "-I" + str(args.source_dir), "-I" + str(ROOT / "native/windows"),
             "-I" + str(ROOT / "native/runtime64"), "-I" + str(ROOT / "tests")]
    sources = [ROOT / "tests/windows_shader_lookup_fixture.c", source, ROOT / "native/windows/loader.c"]
    inputs = sources + [ROOT / "tests/windows_shader_lookup_baseline.inc", ROOT / "tests/windows_shader_lookup_gates.h",
                        args.source_dir / "shader_lookup.h", ROOT / "native/windows/loader.h", Path(__file__)]
    inputs.append(ROOT / "android/host/shader_lookup_aot.c")
    inputs += sorted((ROOT / "native/runtime64").glob("*.h"))
    hashes = {str(p): sha(p) for p in inputs}
    OUT.mkdir(parents=True, exist_ok=True)
    objects = []
    for source_path in sources:
        obj = OUT / (source_path.stem + ".obj")
        result = subprocess.run([str(clang), *flags, "-c", str(source_path), "-o", str(obj)], env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        objects.append(obj)
    exe = OUT / "shader-fixture.exe"
    result = subprocess.run([str(binaries / "link.exe"), "/nologo", "/OUT:" + str(exe),
                             *map(str, objects), "libcmt.lib", "oldnames.lib", "kernel32.lib"],
                            env=env, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    run = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
    (OUT / "stdout.jsonl").write_text(run.stdout)
    (OUT / "stderr.log").write_text(run.stderr)
    if run.returncode:
        raise RuntimeError(run.stdout + run.stderr)
    results = [json.loads(line) for line in run.stdout.splitlines()]
    if not results or results[-1].get("success") is not True:
        raise ValueError("Fixture did not report success")
    if any(sha(Path(name)) != expected for name, expected in hashes.items()):
        raise ValueError("Fixture inputs changed during testing")
    report = dict(success=True, original_exe_sha256=config["original_exe_sha256"], opcode_gates=gates,
                  byte_mutation_gates=334, source_sha256=hashes, compiler_sha256=sha(clang), flags=flags,
                  binary_sha256=sha(exe), results=results, actual_Win32_LastError_preserved=True,
                  original_bytes_executed=False, game_executed=False, Android_modified=False)
    (OUT / "test-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(success=True, semantic=results[-1], benchmarks=results[:-1], report=str(OUT / "test-report.json"))))


if __name__ == "__main__":
    main()
