"""Build the isolated typed Win64 -> native ARM64 OpenAL boundary.

The original PE is read-only. --run uses only this known-source CLI fixture;
there is no Activity, original x64 execution, or core runtime mutation.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import pefile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/openal-bridge"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(command, **kwargs):
    p = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", **kwargs)
    if p.returncode:
        raise RuntimeError(f"command failed {command}:\n{p.stdout[-8000:]}\n{p.stderr[-8000:]}")
    return p.stdout


def verify_native():
    receipt = json.loads((ROOT / "build/native-audio/build-receipt.json").read_text())
    if not receipt["AAudio_only"] or not receipt["NEON"] or receipt["capture_exposed"] or receipt["null_wave_OpenSL_available"]:
        raise ValueError("Physical AAudio-only native backend contract changed")
    for table, anchor in [(receipt["source_sha256"], ROOT),
                          (receipt["overlay_source_sha256"], ROOT / "build/native-audio"),
                          (receipt["runtime_files"], ROOT / "build/native-audio/runtime"),
                          (receipt["licenses"], ROOT / "build/native-audio/licenses")]:
        for name, digest in table.items():
            if sha(anchor / name) != digest:
                raise ValueError("Native backend pin changed: " + name)
    return receipt


def entries():
    original = json.loads((ROOT / "research/openal-contract.json").read_text())
    typed = json.loads((ROOT / "research/openal-typed-contract.json").read_text())
    pe_path = Path(original["original_exe"])
    if sha(pe_path) != original["original_exe_sha256"]:
        raise ValueError("Original game PE changed")
    pe = pefile.PE(str(pe_path), fast_load=True)
    pe.parse_data_directories(directories=[0])
    exports = {x.name.decode(): pe.OPTIONAL_HEADER.ImageBase + x.address
               for x in pe.DIRECTORY_ENTRY_EXPORT.symbols if x.name and x.name.startswith(b"al")}
    if exports != {x["name"]: int(x["va"], 16) for x in original["exports"]}:
        raise ValueError("Original public OpenAL exports changed")
    all_entries = sorted(set(exports.values()))
    routes = [(x["name"], exports[x["name"]]) for x in typed["APIs"]]
    if len(exports) != 159 or len(all_entries) != 157 or len(routes) != 30:
        raise ValueError("Sealed original guard/typed API counts changed")
    for row in typed["APIs"]:
        if exports[row["name"]] != int(row["original_va"], 16):
            raise ValueError("Typed original entry pin changed")
    header = "/* Generated only from sealed original PE exports + typed contract. */\n#pragma once\n"
    header += "#define FROSTPUNK_OPENAL_ROUTES(X) \\\n"
    header += " \\\n".join(f" X({name}, LP{name.upper()}, UINT64_C(0x{va:x}))" for name, va in routes) + "\n"
    header += f"#define FROSTPUNK_OPENAL_MIN_VA UINT64_C(0x{all_entries[0]:x})\n"
    header += f"#define FROSTPUNK_OPENAL_MAX_VA UINT64_C(0x{all_entries[-1]:x})\n"
    header += f"#define FROSTPUNK_OPENAL_ENTRY_COUNT {len(all_entries)}\n"
    header += "static const uint64_t original_openal_entries[]={\n" + ",\n".join(f" UINT64_C(0x{va:x})" for va in all_entries) + "\n};\n"
    (OUT / "openal_original_entries.h").write_text(header, newline="\n")
    return original, typed, routes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    native = verify_native()
    original, typed, routes = entries()
    cfg = json.loads((ROOT / "port.json").read_text())
    sdk = Path(cfg["android_sdk"])
    binaries = sdk / "ndk" / cfg["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    clang = binaries / "clang.exe"
    sources = [ROOT / "android/host/win64_openal.c", ROOT / "tests/win64_openal_fixture.c"]
    inputs = [*sources, ROOT / "android/host/win64_openal.h", ROOT / "android/host/win64.h",
              ROOT / "native/runtime64/cpu64.h", ROOT / "android/audio/metrics.h", Path(__file__),
              ROOT / "research/openal-contract.json", ROOT / "research/openal-typed-contract.json",
              ROOT / "build/native-audio/build-receipt.json", OUT / "openal_original_entries.h",
              ROOT / "reference/openal-soft/source/include/AL/al.h",
              ROOT / "reference/openal-soft/source/include/AL/alc.h", clang,
              ROOT / "research/winmm-monitor-callers.json", ROOT / "tools/inspect_winmm_monitor.py",
              ROOT / "research/winmm-query-oracle.json", ROOT / "tools/oracle_winmm_queries.py"]
    oracle=json.loads((ROOT / "research/winmm-query-oracle.json").read_text())
    for name,digest in oracle["inputs_pre_post_sha256"].items():
        path=Path(name)
        if sha(path)!=digest:raise ValueError("Native WinMM oracle input changed: "+name)
        inputs.append(path)
    pins = {str(p.relative_to(ROOT) if p.is_relative_to(ROOT) else p): sha(p) for p in inputs}
    common = [clang, "--target=aarch64-linux-android30", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
              "-Wno-misleading-indentation", "-Wno-sign-compare", "-I" + str(ROOT / "native/runtime64"),
              "-I" + str(ROOT / "android/host"), "-I" + str(ROOT / "android/audio"),
              "-I" + str(ROOT / "reference/openal-soft/source/include"), "-I" + str(OUT)]
    obj, archive, fixture = OUT / "win64_openal.o", OUT / "libwin64_openal.a", OUT / "openal_fixture"
    compile_command = [*common, "-fPIC", "-c", sources[0], "-o", obj]
    checked(compile_command)
    checked([binaries / "llvm-ar.exe", "rcs", archive, obj])
    fixture_command = [*common, sources[1], archive, "-ldl", "-lm", "-Wl,-z,max-page-size=16384",
                       "-Wl,--wrap=openat,--wrap=fstat,--wrap=read", "-o", fixture]
    checked(fixture_command)
    for name, digest in pins.items():
        if sha(ROOT / name) != digest:
            raise ValueError("Bridge input changed during build: " + name)
    verify_native()
    report = dict(timestamp_utc=datetime.now(timezone.utc).isoformat(), source_sha256=pins,
                  original_exe_sha256=original["original_exe_sha256"],
                  original_public_names=159, original_public_entries=157, typed_routes=routes,
                  compiler_sha256=sha(clang), compile_command=[str(x) for x in compile_command],
                  fixture_command=[str(x) for x in fixture_command],
                  archive_sha256=sha(archive), fixture_sha256=sha(fixture),
                  native_runtime_files=native["runtime_files"], native_licenses=native["licenses"],
                  winmm_policy="Actual native ALC output-list inventory only. No Windows wave playback driver: valid-ID or mapper caps return NODRIVER6 without writes; unknown IDs return BADDEVICEID2; cb0 returns0 first. No Windows hardware count/name/caps parity claimed. Guest LastError untouched; native errno restored. Import gate/common RSP is covered by the full-core self-test, not this direct helper fixture.",
                  game_boot_proven=False, original_x64_executed=False,
                  limitations=["Only the30 observed typed APIs; other original public entries stop73",
                               "Capture/EFX/callback/thread-local-context extensions are not advertised",
                               "Shutdown requires all guest workers stopped",
                               "Native source version1.25.2 differs from embedded1.18.2; tests bound observed ABI/error surface"])
    (OUT / "build-receipt.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.run:
        adb = [sdk / "platform-tools/adb.exe", "-s", cfg["device"]]
        remote = "/data/local/tmp/frostpunk/openal-bridge"
        checked([*adb, "shell", "mkdir", "-p", remote, remote + "/profile"])
        files = {"openal_fixture": fixture, **{name: ROOT / "build/native-audio/runtime" / name for name in native["runtime_files"]}}
        for name, path in files.items():
            checked([*adb, "push", path, remote + "/" + name])
            if checked([*adb, "shell", "sha256sum", remote + "/" + name]).split()[0] != sha(path):
                raise ValueError("Remote fixture pin mismatch")
        remote_before = {name: checked([*adb, "shell", "sha256sum", remote + "/" + name]).split()[0] for name in files}
        checked([*adb, "shell", "chmod", "755", remote + "/openal_fixture"])
        command = f"cd {remote} && LD_LIBRARY_PATH={remote} ./openal_fixture {remote}/libopenal.so {remote}/profile"
        p = subprocess.run([str(x) for x in [*adb, "shell", command]], capture_output=True, text=True, timeout=15)
        if p.returncode:
            raise RuntimeError(p.stdout + p.stderr)
        negatives = []
        for name in ["unsupported", "alias", "foreign", "released", "attr-overflow", "attr-unterminated", "readonly", "array-range", "null-sources", "null-buffers", "null-queue", "closed-context", "shutdown"]:
            q = subprocess.run([str(x) for x in [*adb, "shell", command + " " + name]], capture_output=True, text=True, timeout=15)
            expected = 74 if name in ["readonly", "array-range"] else 73
            if q.returncode != expected:
                raise RuntimeError(f"Negative {name} returned{q.returncode}: {q.stdout} {q.stderr}")
            negatives.append(dict(case=name, returncode=q.returncode, stdout=q.stdout, stderr=q.stderr))
        report.update(serial=cfg["device"], android_hardware_pass=True, android_command=command,
                      android_stdout=p.stdout, android_stderr=p.stderr, android_fixture=json.loads(p.stdout),
                      negative_children=negatives, remote_inputs_before=remote_before,
                      remote_inputs_after={name: checked([*adb, "shell", "sha256sum", remote + "/" + name]).split()[0] for name in files})
        if report["remote_inputs_after"] != remote_before:
            raise ValueError("Remote OpenAL fixture inputs changed during execution")
        for name, digest in pins.items():
            if sha(ROOT / name) != digest:
                raise ValueError("Bridge input changed during execution: " + name)
        if sha(archive) != report["archive_sha256"] or sha(fixture) != report["fixture_sha256"]:
            raise ValueError("Bridge outputs changed during execution")
        verify_native()
        (ROOT / "research/win64-openal.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(build_receipt=str(OUT / "build-receipt.json"), archive_sha256=sha(archive),
                          android=report.get("android_fixture"))))


if __name__ == "__main__":
    main()
