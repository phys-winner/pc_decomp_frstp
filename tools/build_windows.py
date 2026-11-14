"""Build the existing static Frostpunk C lift for native Windows x64.

Use --objects-only while the Windows host is being prepared. No guest code is
regenerated, translated at runtime, or executed by this builder.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

import pefile

import build_startup
from leaf_probe import windows_toolchain

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/windows"
GENERATED = ROOT / "generated/frostpunk-x64"
TARGET = "x86_64-pc-windows-msvc"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fingerprint(source_hash, options_hash):
    return hashlib.sha256((source_hash + "\n" + options_hash).encode()).hexdigest()


def manifest_toolchain(env):
    sdk_include = next(Path(value) for value in env["INCLUDE"].split(";") if Path(value).name == "um")
    sdk_bin = sdk_include.parents[2] / "bin" / sdk_include.parent.name / "x64"
    tools = {str(sdk_bin / name): digest(sdk_bin / name) for name in ("mt.exe", "rc.exe")}
    env = env.copy()
    env["PATH"] = str(sdk_bin) + os.pathsep + env.get("PATH", "")
    return env, tools


def manifest_settings(data):
    root = ET.fromstring(data)
    execution = root.findall(".//{urn:schemas-microsoft-com:asm.v3}requestedExecutionLevel")
    dpi = root.findall(".//{http://schemas.microsoft.com/SMI/2005/WindowsSettings}dpiAware")
    if len(execution) != 1 or len(dpi) != 1:
        raise ValueError("Application manifest must have one execution level and DPI policy")
    settings = dict(execution_level=execution[0].get("level"), ui_access=execution[0].get("uiAccess"),
                    dpi_aware=(dpi[0].text or "").strip())
    if settings != dict(execution_level="asInvoker", ui_access="false", dpi_aware="True/PM"):
        raise ValueError("Application manifest does not preserve original asInvoker and True/PM DPI policy")
    return settings


def embedded_manifest(path):
    with pefile.PE(str(path), fast_load=True) as pe:
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
        resources = []
        directory = getattr(pe, "DIRECTORY_ENTRY_RESOURCE", None)
        for kind in directory.entries if directory else ():
            if kind.id != 24:  # RT_MANIFEST
                continue
            for name in kind.directory.entries:
                if name.id != 1:  # CREATEPROCESS_MANIFEST_RESOURCE_ID
                    continue
                for language in name.directory.entries:
                    data = language.data.struct
                    resources.append((language.id, pe.get_data(data.OffsetToData, data.Size)))
    if len(resources) != 1:
        raise ValueError("Executable must contain one RT_MANIFEST resource with ID 1")
    language, data = resources[0]
    return dict(resource_id=1, language=language, sha256=hashlib.sha256(data).hexdigest(),
                settings=manifest_settings(data))


def packed_compare_overlay(config, pinned):
    manifest_path = ROOT / "research/windows-packed-compare-overlay.json"
    manifest = json.loads(manifest_path.read_text())
    validation = manifest.get("validation", {})
    site_count = {"hot9": 9, "all": 212}.get(manifest.get("scope"))
    if (manifest.get("success") is not True or site_count is None or
            manifest.get("original_exe_sha256") != config["original_exe_sha256"] or
            validation.get("success") is not True or validation.get("overlay_sites") != site_count or
            validation.get("case_count", 0) <= 0 or len(manifest.get("sites", [])) != site_count or
            len({row["va"] for row in manifest["sites"]}) != site_count):
        raise ValueError("Packed compare overlay has no successful approved native oracle")

    def checked_path(name, expected):
        relative = Path(name)
        path = (ROOT / relative).resolve()
        if relative.is_absolute() or not path.is_relative_to(ROOT.resolve()):
            raise ValueError("Packed compare provenance path escapes workspace")
        if digest(path) != expected:
            raise ValueError("Packed compare provenance mismatch: " + name)
        pinned[path] = expected
        return path

    units = manifest.get("units") if manifest["scope"] == "all" else [dict(canonical=manifest["canonical"], overlay=manifest["overlay"])]
    if (len(units) != (46 if manifest["scope"] == "all" else 1) or
            manifest["scope"] == "all" and manifest.get("occurrences") != 288):
        raise ValueError("Packed compare overlay unit/occurrence count is not approved")
    replacements = {}
    for unit in units:
        canonical = checked_path(unit["canonical"]["path"], unit["canonical"]["sha256"])
        replacement = checked_path(unit["overlay"]["path"], unit["overlay"]["sha256"])
        if manifest["scope"] == "hot9":
            if (canonical != (GENERATED / "recomp_funcs_0820.c").resolve() or
                    replacement != (OUT / "generated-overlay/recomp_funcs_0820.c").resolve()):
                raise ValueError("Packed compare hot9 overlay must replace only translation unit 0820")
        elif (canonical.parent != GENERATED.resolve() or replacement.parent != (OUT / "generated-overlay-all").resolve() or
              not re.fullmatch(r"recomp_funcs_[0-9]{4}\.c", canonical.name) or replacement.name != canonical.name):
            raise ValueError("Packed compare all overlay path is not an approved main unit")
        if canonical in replacements:
            raise ValueError("Duplicate canonical packed compare overlay unit")
        replacements[canonical] = replacement
    checked_path(manifest["generator"]["path"], manifest["generator"]["sha256"])
    dependencies = manifest.get("dependencies", {})
    required = {"native/windows/simd_float.c", "native/windows/simd_float.h",
                "native/windows/simd_float_ops.asm", "tests/windows_packed_compare_fixture.c",
                "tools/test_windows_packed_compare.py"}
    if not required <= dependencies.keys():
        raise ValueError("Packed compare oracle dependencies are incomplete")
    for name, expected in dependencies.items():
        checked_path(name, expected)
    receipt_path = checked_path(validation["receipt"], validation["receipt_sha256"])
    receipt = json.loads(receipt_path.read_text())
    if (receipt["result"].get("success") is not True or
            receipt["result"].get("native_full_CPU_cases") != validation["case_count"] or
            receipt["result"].get("overlay_sites") != site_count):
        raise ValueError("Packed compare native oracle receipt disagrees with manifest")
    for name, expected in receipt["source_hashes"].items():
        source = Path(name).resolve()
        if not source.is_relative_to(ROOT.resolve()):
            raise ValueError("Packed compare oracle source escapes workspace")
        checked_path(str(source.relative_to(ROOT.resolve())), expected)
    tested_sources = {Path(name).resolve() for name in receipt["source_hashes"]}
    if not (set(replacements) | set(replacements.values())) <= tested_sources:
        raise ValueError("Packed compare oracle did not pin all canonical and overlay units")
    pinned[manifest_path] = digest(manifest_path)
    return replacements, dict(scope=manifest["scope"], manifest=str(manifest_path.relative_to(ROOT)),
        manifest_sha256=pinned[manifest_path], units=units, site_count=site_count,
        native_full_CPU_cases=validation["case_count"], oracle_receipt_sha256=validation["receipt_sha256"])


def verified_direct_overlay(config, pinned, packed_replacements):
    """Exact Windows source map plus separately pinned native boundary oracle."""
    import generate_android_direct_dispatch as direct
    import generate_windows_direct_dispatch as windows_direct
    folder = ROOT / "build/windows-direct-dispatch-candidate"
    manifest_path = folder / "source-map.json"
    receipt_path = ROOT / "build/windows-direct-dispatch-runtime/result.json"
    manifest = json.loads(manifest_path.read_text())
    receipt = json.loads(receipt_path.read_text())
    coverage = ("full_CPU_stack_LastError_MXCSR", "recursive_depth_active_CPU", "null_fallback",
                "self_fallback", "nonzero_delta", "noncode_native_fallback", "shader_override_exclusions",
                'fixed_GVA_actual_generated_full_CPU_stack_globals_LastError_MXCSR', 'fixed_GVA_private_main_scope', 'fixed_GVA_actual_startup_nonzero_rejected')
    if (manifest.get("success") is not True or manifest.get("target") != TARGET or
            manifest.get("original_exe_sha256") != config["original_exe_sha256"] or
            manifest.get("runtime_qualified") is not False or manifest.get("source_map_only") is not True or
            len(manifest.get("units", [])) != 1000 or manifest.get("negative_source_mutations_rejected") != 4 or
            manifest.get("rounded_cvtt_quarantine") != windows_direct.rounded_cvtt_metadata() or
            manifest.get("e35a50") != windows_direct.e35a50_metadata() or
            receipt.get("success") is not True or
            any(receipt.get("coverage", {}).get(key) is not True for key in coverage)):
        raise ValueError("Direct dispatch has no complete source/native boundary proof")

    def checked(name, expected):
        relative = Path(name);path = (ROOT / relative).resolve()
        if relative.is_absolute() or not path.is_relative_to(ROOT.resolve()) or digest(path) != expected:
            raise ValueError("Direct provenance mismatch: " + name)
        pinned[path] = expected
        return path

    e35_receipt_path = ROOT / "build/windows-e35a50-test/result.json"
    e35 = json.loads(e35_receipt_path.read_text())
    if (e35.get("success") is not True or e35.get("contract") != windows_direct.e35a50_metadata() or
            e35.get("normal", {}).get("normal_cases") != 3456 or
            e35.get("normal", {}).get("fast") != 2688 or e35.get("normal", {}).get("fallback") != 768 or
            e35.get("native", {}).get("native_cases") != 1536 or
            e35.get("stack_fallback", {}).get("stack_fallback_cases") != 384 or
            e35.get("admission", {}).get("admission_cases") != 48 or
            any(e35.get(section, {}).get(metric) != 0 for section in ["normal", "native", "stack_fallback"]
                for metric in ["memory_mismatches", "environment_mismatches"]) or
            e35.get("native", {}).get("ordered_memory") != 0 or e35.get("native", {}).get("local_memory") != 0 or
            e35.get("negative_source_mutations_rejected") != 7 or e35.get("codegen_qualified") is not True):
        raise ValueError("E35 leaf has no complete current native oracle proof")
    e35_proven = set()
    for name, expected in e35.get("source_hashes", {}).items():
        path = Path(name).resolve()
        if not path.is_relative_to(ROOT.resolve()):raise ValueError("E35 oracle source escapes workspace")
        checked(str(path.relative_to(ROOT)), expected);e35_proven.add(path)
    e35_required = {ROOT / "tools/generate_windows_direct_dispatch.py", ROOT / "tools/generate_android_direct_dispatch.py", ROOT / "tools/build_windows.py",
        ROOT / "tools/test_windows_e35a50.py", ROOT / "tools/leaf_probe.py", ROOT / "port.json",
        ROOT / "tests/windows_e35a50_fixture.c", ROOT / "tests/windows_e35a50_fixture.asm",
        ROOT / "generated/frostpunk-x64/recomp_funcs_0804.c",
        *(ROOT / "native/runtime64" / name for name in ["cpu64.h", "sse_scalar.h", "eh64.h", "bswap.h"])}
    if not e35_required <= e35_proven:raise ValueError("E35 oracle source pins incomplete")
    pinned[e35_receipt_path] = digest(e35_receipt_path)
    for name, expected in manifest["inputs"].items():checked(name, expected)
    proven = {}
    for name, expected in receipt.get("source_hashes", {}).items():
        path = Path(name)
        path = path.resolve() if path.is_absolute() else (ROOT / path).resolve()
        if not path.is_relative_to(ROOT.resolve()):raise ValueError("Direct oracle source escapes workspace")
        proven[path] = expected;checked(str(path.relative_to(ROOT)), expected)
    host = folder / "host.direct.c";header = folder / "generated-overlay/aot_dispatch.h"
    required = {manifest_path, host, folder / "host.before.c", ROOT / "native/windows/host.c",
        ROOT / "tools/generate_windows_direct_dispatch.py", ROOT / "tools/generate_android_direct_dispatch.py",
        ROOT / "tools/test_windows_direct_dispatch.py", ROOT / "tests/windows_direct_dispatch_cases.inc",
        ROOT / "tests/windows_direct_dispatch_entry.c"}
    if not required <= proven.keys():raise ValueError("Direct oracle did not pin its source map and exact host")
    if digest(host) != manifest["host"]["host_candidate_sha256"]:
        raise ValueError("Direct host is not the oracle-qualified candidate")
    checked(str(host.relative_to(ROOT)), manifest["host"]["host_candidate_sha256"])
    if digest(ROOT / "native/windows/host.c") != manifest["host"]["host_before_sha256"]:
        raise ValueError("Canonical host differs from the qualified direct preimage")
    windows_direct.verify_fixed_image_contract((ROOT / 'native/windows/host.c').read_text())
    expected_header = '#pragma once\n#include "cpu64.h"\n/* Exact generated constant VA/function pairs only; dynamic calls use dispatch. */\nvoid fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU *));\n/* Windows startup rejects a nonzero image delta before entering generated code. */\n#undef GVA\n#define GVA(abs) ((uint64_t)(abs))\n'
    if (ROOT / manifest["header"]["path"]).resolve() != header.resolve() or header.read_bytes() != expected_header.encode():
        raise ValueError("Direct private entry declaration differs")
    checked(str(header.relative_to(ROOT)), manifest["header"]["sha256"])
    table = ROOT / "generated/frostpunk-x64/recomp_dispatch.c"
    declarations = ROOT / "generated/frostpunk-x64/recomp_funcs.h"
    metadata_path = ROOT / "generated/frostpunk-x64/recomp_meta.json"
    metadata = json.loads(metadata_path.read_text())
    if digest(metadata_path) != manifest["canonical_metadata_sha256"]:
        raise ValueError("Direct canonical metadata drift")
    for path in [table, declarations]:
        checked(str(path.relative_to(ROOT)), metadata["generated_files"][path.name])
    bindings = direct.binding_table(table.read_text(), declarations.read_text())
    replacements = {};targets = set();totals = dict(eligible=0, manual=0, openal=0, other=0)
    for row in manifest["units"]:
        canonical = checked(row["canonical"]["path"], row["canonical"]["sha256"])
        source = checked(row["input"]["path"], row["input"]["sha256"])
        output = checked(row["overlay"]["path"], row["overlay"]["sha256"])
        if (canonical.parent != GENERATED.resolve() or canonical in replacements or
                not re.fullmatch(r"recomp_funcs_[0-9]{4}\.c", canonical.name) or
                row["canonical"]["sha256"] != metadata["generated_files"][canonical.name] or
                source != packed_replacements.get(canonical, canonical) or
                output != (folder / "generated-overlay" / canonical.name).resolve()):
            raise ValueError("Direct source-map unit/path/upstream differs")
        expected, counts, rounded_calls = windows_direct.rewrite_windows_unit(source.read_bytes().decode(), canonical.name, bindings, targets)
        if (output.read_bytes() != expected.encode() or counts != row["counts"] or
                row.get("rounded_cvtt_calls") != rounded_calls):
            raise ValueError("Direct source edits extend beyond exact main call pairs")
        for key in totals:totals[key] += counts[key]
        replacements[canonical] = output
    if (set(replacements) != {p.resolve() for p in GENERATED.glob("recomp_funcs_*.c")} or
            totals != manifest["counts"] or len(targets) != manifest["unique_target_count"]):
        raise ValueError("Direct constant-call coverage differs")
    auxiliary = manifest.get("auxiliary_units", [])
    if len(auxiliary) != 1:raise ValueError("Expected one exact payload quarantine unit")
    row = auxiliary[0];payload = ROOT / "generated/payload/recomp"
    source = checked(row["canonical"]["path"], row["canonical"]["sha256"])
    output = checked(row["overlay"]["path"], row["overlay"]["sha256"])
    payload_header = checked(row["header"]["canonical"]["path"], row["header"]["canonical"]["sha256"])
    local_payload_header = checked(row["header"]["overlay"]["path"], row["header"]["overlay"]["sha256"])
    if (source != (payload / "recomp_funcs_0001.c").resolve() or
            output != (folder / "generated-overlay/payload/recomp_funcs_0001.c").resolve() or
            payload_header != (payload / "recomp_funcs.h").resolve() or local_payload_header != output.parent / payload_header.name):
        raise ValueError("Payload quarantine source/header path differs")
    expected, rounded_calls = windows_direct.quarantine_rounded_cvtt(source.read_text(), "payload/recomp_funcs_0001.c")
    if (output.read_bytes() != expected.encode() or rounded_calls != 2 or row.get("rounded_cvtt_calls") != 2 or
            local_payload_header.read_bytes() != payload_header.read_bytes()):
        raise ValueError("Payload edits extend beyond the two rounded CVT callees")
    replacements[source] = output
    pinned[manifest_path] = digest(manifest_path);pinned[receipt_path] = digest(receipt_path)
    pinned[Path(__file__).resolve()] = digest(Path(__file__))
    return replacements, dict(manifest=str(manifest_path.relative_to(ROOT)),
        manifest_sha256=digest(manifest_path), native_receipt=str(receipt_path.relative_to(ROOT)),
        native_receipt_sha256=digest(receipt_path), host=str(host.relative_to(ROOT)),
        host_sha256=digest(host), header=str(header.relative_to(ROOT)), counts=totals,
        unique_target_count=len(targets), coverage={key:True for key in coverage},
        e35a50_receipt=str(e35_receipt_path.relative_to(ROOT)), e35a50_receipt_sha256=digest(e35_receipt_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--objects-only", action="store_true")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    config = json.loads((ROOT / "port.json").read_text())
    original = Path(config["game_directory"]) / "Frostpunk.exe"
    if digest(original) != config["original_exe_sha256"]:
        raise ValueError("Original Frostpunk executable identity changed")
    clang = (Path(config["android_sdk"]) / "ndk" / config["android_ndk_version"] /
             "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe")
    binaries, env = windows_toolchain()
    metadata_path = GENERATED / "recomp_meta.json"
    metadata = json.loads(metadata_path.read_text())
    if (metadata.get("original_exe_sha256") != config["original_exe_sha256"]
            or not metadata.get("original_entry_preserved")):
        raise ValueError("Checked main generation identity or entry contract is missing")
    pinned = {metadata_path: digest(metadata_path)}
    pinned.update({ROOT / name: expected for name, expected in metadata["generator_sources"].items()})
    pinned[ROOT / metadata["catalog"]] = metadata["catalog_sha256"]
    bodies = sorted(GENERATED.glob("recomp_funcs_*.c"))
    if len(bodies) != metadata["translation_units"]:
        raise ValueError("Main generated translation-unit count is incomplete")
    for name, expected in metadata["generated_files"].items():
        if Path(name).name != name:
            raise ValueError("Invalid main generated filename")
        pinned[GENERATED / name] = expected
    supplemental, supplemental_pins, _ = build_startup.verified_supplemental_sources(
        config["original_exe_sha256"])
    pinned.update({path: digest(path) for path in supplemental_pins})
    payload_dir = ROOT / "generated/payload/recomp"
    payload_metadata_path = payload_dir / "recomp_meta.json"
    if not payload_metadata_path.exists():
        subprocess.run([sys.executable, str(ROOT / "tools/extract_bind_payload.py"), "--generate"],
                       check=True, stdout=subprocess.DEVNULL)
    payload_metadata = json.loads(payload_metadata_path.read_text())
    if (payload_metadata.get("symbol_prefix") != "payload_"
            or payload_metadata.get("hle_base") != "0xe5e60000"):
        raise ValueError("Payload generated dispatch namespace is invalid")
    pinned[payload_metadata_path] = digest(payload_metadata_path)
    pinned[ROOT / "generated/payload/input/SteamDRMP.dll"] = payload_metadata["payload_sha256"]
    pinned.update({ROOT / name: expected for name, expected in payload_metadata["generator_sources"].items()})
    for name, expected in payload_metadata["generated_files"].items():
        if Path(name).name != name:
            raise ValueError("Invalid payload generated filename")
        pinned[payload_dir / name] = expected
    for path, expected in pinned.items():
        if digest(path) != expected:
            raise ValueError("Generated provenance mismatch: " + str(path.relative_to(ROOT)))
    packed_replacements, packed_overlay = packed_compare_overlay(config, pinned)
    replacements, direct_overlay = verified_direct_overlay(config, pinned, packed_replacements)
    direct_host = ROOT / direct_overlay["host"]
    direct_header = ROOT / direct_overlay["header"]
    application_manifest = None
    if not args.objects_only:
        manifest_path = ROOT / "native/windows/host.manifest"
        env, manifest_tools = manifest_toolchain(env)
        application_manifest = dict(source=str(manifest_path.relative_to(ROOT)), source_sha256=digest(manifest_path),
            settings=manifest_settings(manifest_path.read_bytes()), original=embedded_manifest(original), tools=manifest_tools)
        pinned[manifest_path] = application_manifest["source_sha256"]
    bodies = [replacements.get(path.resolve(), path) for path in bodies]
    overlay = OUT / "include/cpu64.h"
    overlay.parent.mkdir(parents=True, exist_ok=True)
    cpu_header = ROOT / "native/runtime64/cpu64.h"
    cpu_text = cpu_header.read_text()
    marker = "#if defined(__aarch64__)"
    if marker not in cpu_text:
        raise ValueError("Canonical CPU header has no ARM64 guest TEB branch")
    overlay_text = cpu_text.replace(marker,
        "#if defined(__aarch64__) || defined(FROSTPUNK_WINDOWS_HOST)", 1)
    if not overlay.exists() or overlay.read_text() != overlay_text:
        overlay.write_text(overlay_text, encoding="utf-8", newline="\n")
    sources = bodies + [GENERATED / "recomp_dispatch.c", GENERATED / "recomp_imports.c",
                       ROOT / "native/runtime64/eh64.c", supplemental]
    payload_sources = sorted(payload_dir.glob("recomp_funcs_*.c"))
    if len(payload_sources) != payload_metadata["translation_units"]:
        raise ValueError("Payload generated translation-unit count is incomplete")
    sources += [replacements.get(path.resolve(), path) for path in payload_sources] + [payload_dir / "recomp_dispatch.c", payload_dir / "recomp_imports.c"]
    if not args.objects_only:
        host_sources = [direct_host if p.name == "host.c" else p
                        for p in sorted((ROOT / "native/windows").glob("*.c"))]
        if not host_sources:
            raise ValueError("Windows host sources are missing; use --objects-only during preparation")
        sources += host_sources + sorted((ROOT / "native/windows").glob("*.asm"))
    include_dirs = [overlay.parent, ROOT / "native/windows", ROOT / "native/runtime64", GENERATED,
                    supplemental.parent, payload_dir, ROOT / "generated/win64", ROOT / "android/host"]
    flags = ["--target=" + TARGET, "-O2", "-std=c11", "-fno-strict-aliasing", "-fwrapv",
             "-ffp-contract=off", "-fno-math-errno", "-DFROSTPUNK_WINDOWS_HOST=1",
             "-DFROSTPUNK_HAVE_SUPPLEMENTAL=1", "-DFROSTPUNK_HAVE_PAYLOAD=1",
             "-D_CRT_SECURE_NO_WARNINGS", "-w", "-include", str(overlay)]
    flags += ["-I" + str(directory) for directory in include_dirs]
    headers = sorted({path for directory in include_dirs if directory != ROOT / "native/windows"
                      for path in directory.glob("*.h")})
    host_headers = sorted((ROOT / "native/windows").glob("*.h"))
    host_headers += sorted((ROOT / "native/windows").glob("*.inc"))
    header_hashes = {str(path): digest(path) for path in headers}
    options = {"compiler_sha256": digest(clang), "flags": flags, "headers": header_hashes,
               "INCLUDE": env["INCLUDE"], "LIB": env["LIB"]}
    common_hash = hashlib.sha256(json.dumps(options, sort_keys=True).encode()).hexdigest()
    host_options = {"common": common_hash, "headers": {str(path): digest(path) for path in host_headers},
                    "assembler_sha256": digest(binaries / "ml64.exe")}
    host_hash = hashlib.sha256(json.dumps(host_options, sort_keys=True).encode()).hexdigest()
    # These are the overlay's only native headers; CPU/recomp headers are in common_hash.
    overlay_options = {"common": common_hash, "headers": {
        str(ROOT / "native/windows" / name): digest(ROOT / "native/windows" / name)
        for name in ("simd_float.h", "loader.h")}}
    overlay_options["headers"][str(direct_header)] = digest(direct_header)
    overlay_hash = hashlib.sha256(json.dumps(overlay_options, sort_keys=True).encode()).hexdigest()
    source_hashes = {str(path): digest(path) for path in sources}
    started = time.monotonic()

    def object_path(path):
        return (OUT / "objects" / path.relative_to(ROOT)).with_suffix(".obj")

    def compile_one(path):
        source_hash = source_hashes[str(path)]
        signature = fingerprint(source_hash, overlay_hash if path in replacements.values() else
                                host_hash if (path.parent == ROOT / "native/windows" or path == direct_host) else common_hash)
        obj = object_path(path)
        stamp = obj.with_suffix(".json")
        if obj.exists() and stamp.exists():
            cached = json.loads(stamp.read_text())
            if cached.get("fingerprint") == signature and cached.get("object_sha256") == digest(obj):
                return {"source": str(path.relative_to(ROOT)), "status": "reused"}
        obj.parent.mkdir(parents=True, exist_ok=True)
        temporary = obj.with_suffix(".tmp.obj")
        if path.suffix == ".asm":
            command = [binaries / "ml64.exe", "/nologo", "/c", "/Fo" + str(temporary), path]
        else:
            command = [clang, *flags, "-c", path, "-o", temporary]
        result = subprocess.run([str(value) for value in command], env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode or digest(path) != source_hash:
            temporary.unlink(missing_ok=True)
            return {"source": str(path.relative_to(ROOT)), "status": "failed",
                    "diagnostic": (result.stdout + result.stderr)[-8000:] or "Source changed during compilation"}
        with temporary.open("rb") as stream:
            if stream.read(2) != b"\x64\x86":
                raise ValueError("Compiler output is not AMD64 COFF: " + str(path))
        temporary.replace(obj)
        stamp.write_text(json.dumps({"fingerprint": signature, "object_sha256": digest(obj)}, indent=2) + "\n")
        return {"source": str(path.relative_to(ROOT)), "status": "compiled"}

    print(f"Building {len(sources)} Windows x64 sources with {args.jobs} jobs", flush=True)
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, path) for path in sources]
        for future in as_completed(futures):
            row = future.result()
            results.append(row)
            if row["status"] == "failed":
                print("FAIL " + row["source"] + "\n" + row["diagnostic"], flush=True)
            elif len(results) % 50 == 0:
                print(f"{len(results)}/{len(sources)} completed ({time.monotonic() - started:.1f}s)", flush=True)
    changed = [str(path.relative_to(ROOT)) for path, expected in pinned.items() if digest(path) != expected]
    changed += [str(path.relative_to(ROOT)) for path in sources if digest(path) != source_hashes[str(path)]]
    changed += [str(path.relative_to(ROOT)) for path in headers if digest(path) != header_hashes[str(path)]]
    if not args.objects_only:
        changed += [str(path.relative_to(ROOT)) for path in host_headers
                    if digest(path) != host_options["headers"][str(path)]]
    counts = {status: sum(row["status"] == status for row in results) for status in ("compiled", "reused", "failed")}
    report = {"target": TARGET, "objects_only": args.objects_only, "counts": counts,
              "source_hashes": source_hashes, "options": options, "host_options": host_options,
              "overlay_options": overlay_options,
              "provenance_inputs": {str(path.relative_to(ROOT)): expected for path, expected in pinned.items()},
              "generated_overlay": packed_overlay,
              "direct_dispatch_overlay": direct_overlay,
              "application_manifest": application_manifest,
              "inputs_changed_during_build": changed, "seconds": round(time.monotonic() - started, 2),
              "success": not counts["failed"] and not changed,
              "failures": [row for row in results if row["status"] == "failed"], "linked_game": False}
    if report["success"] and not args.objects_only:
        exe = OUT / "Frostpunk.exe"
        temporary = exe.with_suffix(".tmp.exe")
        response = OUT / "link.rsp"
        link_inputs = {object_path(path): digest(object_path(path)) for path in sources}
        response.write_text("\n".join('"' + str(object_path(path)) + '"' for path in sources) + "\n",
                            encoding="utf-8")
        command = [binaries / "link.exe", "/nologo", "/OUT:" + str(temporary),
                   "/MAP:" + str(OUT / "frostpunk-windows-x64.map"),
                   "/BASE:0x180000000", "/DYNAMICBASE:NO", "/LARGEADDRESSAWARE", "/STACK:16777216",
                   "/SUBSYSTEM:CONSOLE", "/ENTRY:wmainCRTStartup",
                   "/MANIFEST:EMBED", "/MANIFESTUAC:NO", "/MANIFESTINPUT:" + str(manifest_path),
                   "@" + str(response), "libcmt.lib", "oldnames.lib", "kernel32.lib", "user32.lib",
                   "advapi32.lib", "shell32.lib", "ole32.lib", "winmm.lib", "version.lib", "dbghelp.lib",
                   "d3d11.lib", "dxgi.lib", "dxguid.lib"]
        linked = subprocess.run([str(value) for value in command], env=env,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        (OUT / "link.log").write_text(linked.stdout + linked.stderr, encoding="utf-8")
        link_changed = [str(path.relative_to(ROOT)) for path, expected in link_inputs.items()
                        if digest(path) != expected]
        link_changed += [str(path.relative_to(ROOT)) for path in sources
                         if digest(path) != source_hashes[str(path)]]
        link_changed += [str(path.relative_to(ROOT)) for path in headers
                         if digest(path) != header_hashes[str(path)]]
        link_changed += [str(path.relative_to(ROOT)) for path in host_headers
                         if digest(path) != host_options["headers"][str(path)]]
        link_changed += [str(path.relative_to(ROOT)) for path, expected in pinned.items() if digest(path) != expected]
        report["inputs_changed_during_build"] += link_changed
        report["success"] = linked.returncode == 0 and not link_changed
        if report["success"]:
            try:
                application_manifest["embedded"] = embedded_manifest(temporary)
            except (ValueError, ET.ParseError, pefile.PEFormatError) as error:
                report["success"] = False
                report["manifest_error"] = str(error)
        if report["success"]:
            temporary.replace(exe)
            binary_hash = digest(exe)
            report.update(linked_game=True, executable=str(exe), executable_sha256=binary_hash,
                          binary=str(exe.relative_to(ROOT)), binary_sha256=binary_hash)
        else:
            print((linked.stdout + linked.stderr)[-8000:], flush=True)
    report["seconds"] = round(time.monotonic() - started, 2)
    (OUT / "build-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not args.objects_only:
        (ROOT / "research/windows-build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("target", "success", "counts", "linked_game", "seconds")}), flush=True)
    raise SystemExit(0 if report["success"] else 1)


if __name__ == "__main__":
    assert fingerprint("source", "options") != fingerprint("changed", "options")
    assert fingerprint("source", "options") != fingerprint("source", "changed")
    main()
