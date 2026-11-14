"""Compile the checked Frostpunk x64 lift to Android ARM64 objects, without linking a game.

    python tools/build_lifted.py --jobs 8

Cached objects require identical source, headers, compiler and options. Generated
inputs must remain unchanged throughout the build; a concurrent regeneration is
rejected instead of producing a mixed result.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
import generate_android_aot_overlay as android_overlay
import android_direct_dispatch_contract as direct_dispatch
import android_resolution_config_contract as resolution_config
import android_eh_contract as android_eh

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / "generated/frostpunk-x64"
OUT = ROOT / "build/arm64"
FLAGS = ["--target=aarch64-linux-android30", "-O2", "-std=c11", "-fPIC", "-fvisibility=hidden",
         "-fno-strict-aliasing", "-fwrapv", "-ffp-contract=off", "-fno-math-errno", "-g0", "-w",
         "-I" + str(ROOT / "native/runtime64"), "-I" + str(GENERATED)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(source_hash, common_hash):
    return hashlib.sha256((source_hash + "\n" + common_hash).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--base-only", action="store_true", help="Compile only immutable units outside the reserved 51-unit repair scope")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    config = json.loads((ROOT / "port.json").read_text())
    sdk = Path(config["android_sdk"])
    clang = sdk / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    metadata_path = GENERATED / "recomp_meta.json"
    metadata_hash = digest(metadata_path)
    metadata = json.loads(metadata_path.read_text())
    if (metadata.get("original_exe_sha256") != config["original_exe_sha256"]
            or not metadata.get("original_entry_preserved")):
        raise ValueError("Checked generation input identity or entry contract is missing")
    pinned_inputs = {ROOT / name: expected for name, expected in metadata["generator_sources"].items()}
    pinned_inputs[Path(__file__).resolve()] = digest(Path(__file__))
    pinned_inputs[ROOT / metadata["catalog"]] = metadata["catalog_sha256"]
    if any(digest(path) != expected for path, expected in pinned_inputs.items()):
        raise ValueError("Generator or catalog changed; regenerate before building")
    bodies = sorted(GENERATED.glob("recomp_funcs_*.c"))
    if len(bodies) != metadata["translation_units"]:
        raise ValueError("Generated body count disagrees with metadata; finish generation first")
    sources = bodies + [GENERATED / "recomp_dispatch.c", GENERATED / "recomp_imports.c",
                       ROOT / "native/runtime64/eh64.c"]
    canonical_sources = list(sources)
    if any(path.stat().st_mtime_ns > metadata_path.stat().st_mtime_ns
           for path in sources if path.parent == GENERATED):
        raise ValueError("Generated sources are newer than final metadata; finish generation first")
    headers = sorted((ROOT / "native/runtime64").glob("*.h")) + sorted(GENERATED.glob("*.h"))
    source_hashes = {}
    checked_calls = 0
    for path in sources:
        raw = path.read_bytes()
        source_hashes[str(path)] = hashlib.sha256(raw).hexdigest()
        if path.parent == GENERATED and source_hashes[str(path)] != metadata["generated_files"].get(path.name):
            raise ValueError(f"Generated source changed: {path.name}; regenerate before building")
        checked_calls += raw.count(b"frostpunk_addss(") + raw.count(b"frostpunk_mulss(")
    if checked_calls == 0:
        raise ValueError("No checked ADDSS/MULSS calls found; run reference64.py --generate to completion first")
    header_hashes = {str(path): digest(path) for path in headers}
    if any(header_hashes[str(path)] != metadata["generated_files"].get(path.name)
           for path in headers if path.parent == GENERATED):
        raise ValueError("Generated headers changed; regenerate before building")
    options = {"compiler_sha256": digest(clang), "flags": FLAGS, "headers": header_hashes,
               "generator_metadata_sha256": metadata_hash}
    legacy_hash = hashlib.sha256(json.dumps(options, sort_keys=True).encode()).hexdigest()
    # Generator provenance is checked separately; unchanged C has unchanged machine code.
    del options["generator_metadata_sha256"]
    common_hash = hashlib.sha256(json.dumps(options, sort_keys=True).encode()).hexdigest()
    repairs = {}; repair_manifest = None; overlay_pins = {}
    if args.base_only:
        deferred = android_overlay.reserved_units()
        sources = [p for p in sources if p.name not in deferred]
    else:
        repairs, repair_manifest = resolution_config.verified_overlay(config, overlay_pins)
        sources = [repairs.get(p.resolve(),p) for p in sources]
    overlay_flags = ["-I"+str(ROOT/"android/host")]
    overlay_options = dict(common=common_hash,flags=overlay_flags,
        headers={str(ROOT/"android/host/aot_cpu.h"):digest(ROOT/"android/host/aot_cpu.h")}) if repairs else None
    overlay_hash = hashlib.sha256(json.dumps(overlay_options,sort_keys=True).encode()).hexdigest() if repairs else None
    eh_pins = {}
    eh_contract = android_eh.verified_contract(config, eh_pins)
    actual_hashes = {str(path):digest(path) for path in sources}
    canonical_for = {repairs.get(path.resolve(),path):path for path in canonical_sources}
    objects = OUT / "objects"
    objects.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()

    def compile_one(path):
        source_hash = actual_hashes[str(path)]
        repaired = path in repairs.values()
        option_hash = overlay_hash if repaired else common_hash
        eh_flags, option_hash = android_eh.unit_options(canonical_for[path], option_hash,
            eh_contract['selected_units'], eh_contract['header_sha256'])
        signature = fingerprint(source_hash, option_hash)
        obj = OUT / android_eh.object_directory(repaired, bool(eh_flags)) / (path.stem + ".o")
        obj.parent.mkdir(parents=True,exist_ok=True)
        stamp = obj.with_suffix(".json")
        identity = dict(canonical=str(canonical_for[path].relative_to(ROOT)),source=str(path.relative_to(ROOT)),
                        source_sha256=source_hash,options_sha256=option_hash,object=str(obj.relative_to(ROOT)))
        if obj.exists() and stamp.exists():
            cached = json.loads(stamp.read_text())
            if cached.get("fingerprint") == signature and cached.get("object_sha256") == digest(obj):
                return dict(identity,status="reused",object_sha256=cached["object_sha256"])
            if not repaired and not eh_flags and cached.get("fingerprint") == fingerprint(source_hash,legacy_hash) and cached.get("object_sha256") == digest(obj):
                stamp.write_text(json.dumps({"fingerprint":signature,"object_sha256":cached["object_sha256"]},indent=2)+"\n")
                return dict(identity,status="reused",object_sha256=cached["object_sha256"])
        temporary = obj.with_suffix(".tmp.o")
        result = subprocess.run([str(clang), *FLAGS, *(overlay_flags if repaired else []), *eh_flags, "-c", str(path), "-o", str(temporary)],
                                capture_output=True, text=True)
        if digest(path) != source_hash:
            temporary.unlink(missing_ok=True)
            return {"source": str(path.relative_to(ROOT)), "status": "failed", "diagnostic": "Source changed during compilation"}
        if result.returncode:
            temporary.unlink(missing_ok=True)
            return {"source": str(path.relative_to(ROOT)), "status": "failed", "diagnostic": result.stderr[-8000:]}
        with temporary.open("rb") as stream:
            elf=stream.read(20)
        if elf[:6]!=b"\x7fELF\x02\x01" or int.from_bytes(elf[16:18],"little")!=1 or int.from_bytes(elf[18:20],"little")!=183:
            temporary.unlink(missing_ok=True)
            return {"source":str(path.relative_to(ROOT)),"status":"failed","diagnostic":"Compiler output is not ELF64 AArch64 ET_REL"}
        temporary.replace(obj)
        stamp.write_text(json.dumps({"fingerprint": signature, "object_sha256": digest(obj)}, indent=2) + "\n")
        return dict(identity,status="compiled",object_sha256=digest(obj))

    results = []
    print(f"Compiling {len(sources)} sources with {args.jobs} jobs; checked SSE calls={checked_calls}", flush=True)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, path) for path in sources]
        for future in as_completed(futures):
            result = future.result()
            results.append(result)
            if result["status"] == "failed":
                print(f"FAIL {result['source']}\n{result['diagnostic']}", flush=True)
            elif len(results) % 25 == 0:
                print(f"{len(results)}/{len(sources)} completed ({time.monotonic() - start:.1f}s)", flush=True)
    changed = [str(path.relative_to(ROOT)) for path in canonical_sources if digest(path) != source_hashes[str(path)]]
    changed += [str(path.relative_to(ROOT)) for path in sources if digest(path) != actual_hashes[str(path)]]
    changed += [str(path.relative_to(ROOT)) for path in headers if digest(path) != header_hashes[str(path)]]
    if digest(metadata_path) != metadata_hash:
        changed.append(str(metadata_path.relative_to(ROOT)))
    changed += [str(path.relative_to(ROOT)) for path, expected in pinned_inputs.items() if digest(path) != expected]
    changed += [str(path.relative_to(ROOT)) for path,expected in overlay_pins.items() if digest(path) != expected]
    changed += [str(path) for path,expected in eh_pins.items() if digest(path) != expected]
    counts = {status: sum(row["status"] == status for row in results) for status in ("compiled", "reused", "failed")}
    report = {"target": "aarch64-linux-android30", "ndk_version": config["android_ndk_version"],
              "objects_only": True, "base_only": args.base_only, "linked_game": False, "generated_metadata": metadata,
              "checked_sse_calls": checked_calls, "source_manifest_sha256": hashlib.sha256(
                  json.dumps(source_hashes, sort_keys=True).encode()).hexdigest(),
              "common_options_sha256": common_hash, "counts": counts,
              "overlay_options": overlay_options, "generated_overlay": None if args.base_only else dict(
                  manifest=str(resolution_config.MANIFEST),sha256=digest(ROOT/resolution_config.MANIFEST)),
              "overlay_provenance_inputs": {str(p.relative_to(ROOT)) if p.is_relative_to(ROOT) else str(p):h for p,h in overlay_pins.items()},
              "android_eh_contract": eh_contract, "android_eh_provenance_inputs": android_eh.pin_manifest(eh_pins),
              "object_manifest": sorted((r for r in results if r["status"]!="failed"),key=lambda r:r["canonical"]),
              "deferred_units": sorted(deferred) if args.base_only else [],
              "inputs_changed_during_build": changed, "seconds": round(time.monotonic() - start, 2),
              "success": counts["failed"] == 0 and not changed,
              "failures": [row for row in results if row["status"] == "failed"]}
    (OUT / ("base-build-report.json" if args.base_only else "build-report.json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (ROOT / ("research/arm64-base-compile.json" if args.base_only else "research/arm64-compile.json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k:report[k] for k in ("target","success","base_only","counts","seconds")}))
    raise SystemExit(0 if report["success"] else 1)


if __name__ == "__main__":
    assert fingerprint("source", "headers") != fingerprint("changed", "headers")
    assert fingerprint("source", "headers") != fingerprint("source", "options")
    main()
