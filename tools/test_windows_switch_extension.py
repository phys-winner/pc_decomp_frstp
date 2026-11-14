"""Compare five separate static lifts with the original generated blocks."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from leaf_probe import ROOT, windows_toolchain


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", action="store_true")
    args = parser.parse_args()
    source_dir = ROOT / "build/windows-switch-extension" if args.candidate else ROOT / "native/windows"
    test_dir = source_dir if args.candidate else ROOT / "tests"
    out = ROOT / ("build/windows-switch-extension-candidate-test" if args.candidate else "build/windows-switch-extension-test")
    out.mkdir(parents=True, exist_ok=True)
    fixture = ROOT / "tests/windows_switch_extension_fixture.c"
    inputs = [source_dir / name for name in ("extra_aot.c", "extra_aot_bodies.inc", "switch_aot_bodies.inc", "switch_extension_bodies.inc")]
    inputs += [test_dir / name for name in ("windows_extra_aot_cases.h", "windows_switch_aot_cases.h", "windows_switch_extension_cases.h", "windows_switch_extension_baseline.h")]
    inputs += [ROOT / name for name in ("native/windows/loader.c", "native/windows/extra_aot.h", "native/windows/loader.h", "native/runtime64/cpu64.h")]
    inputs += [fixture, Path(__file__).resolve()]
    hashes = {str(path): sha(path) for path in inputs}
    for path in [fixture, *[p for p in inputs if p.name.startswith("windows_") and p.suffix == ".h"]]:
        shutil.copyfile(path, out / path.name)
    binaries, env = windows_toolchain()
    exe = out / "fixture.exe"
    command = [str(binaries / "cl.exe"), "/nologo", "/O2", "/std:c11", "/W4", "/I" + str(ROOT / "native/runtime64"),
               "/I" + str(ROOT / "native/windows"), str(out / fixture.name), str(source_dir / "extra_aot.c"),
               str(ROOT / "native/windows/loader.c"), "/Fo" + str(out) + "/", "/Fe" + str(exe), "/link", "/BASE:0x180000000", "/DYNAMICBASE:NO"]
    result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    proof = json.loads(result.stdout)
    assert proof["success"] and proof["entries"] == 5 and proof["original_byte_mutations_rejected"] == 361
    assert hashes == {name: sha(Path(name)) for name in hashes}
    proof.update(sources=hashes, executable_sha256=sha(exe), command=command, game_boot_proven=False)
    (out / "result.json").write_text(json.dumps(proof, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({name: value for name, value in proof.items() if name not in ("sources", "command")}, indent=2))


if __name__ == "__main__":
    main()
