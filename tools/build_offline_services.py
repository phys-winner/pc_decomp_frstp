"""Build a pinned native Android local-only service backend and optional fixture.

Original vendor trees stay pristine. Android ABI and offline-policy changes are
recorded in a private build overlay. --run writes only its own CLI fixture root.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/offline-services"
VENDOR = ROOT / "reference/goldberg"
REVISION = "475342f0d8b2bd7eb0d93bd7cfdd61e3ae7cda24"
PROTO = ROOT / "reference/protobuf-21.12"
ARCHIVES = {"protobuf-cpp-3.21.12.zip": "c7db1d1fead682be24aa29477f5e224fdfc2bb1adeeaee1214f854a2076de71e",
            "protoc-21.12-win64.zip": "71852a30cf62975358edfcbbff93086e8857a079c8e4d6904881aa968d65c7f9"}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed ({result.returncode})\n{result.stdout[-10000:]}\n{result.stderr[-6000:]}")
    return result.stdout.strip()


def prepare_overlay():
    if checked(["git", "-C", VENDOR, "rev-parse", "HEAD"]) != REVISION:
        raise ValueError("Goldberg source pin changed")
    if checked(["git", "-C", VENDOR, "status", "--porcelain", "--untracked-files=no"]):
        raise ValueError("Original Goldberg source was modified")
    overlay = OUT / "source"
    inputs = {}
    modified = {"dll/common_includes.h", "sdk_includes/steamclientpublic.h", "dll/base.cpp",
                "dll/settings_parser.cpp", "dll/network.cpp", "dll/steam_remote_storage.h"}
    for name in checked(["git", "-C", VENDOR, "ls-files"]).splitlines():
        source = VENDOR / name
        if source.is_file():
            destination = overlay / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            if name not in modified and (not destination.exists() or sha(destination) != sha(source)):
                shutil.copyfile(source, destination)
            inputs[str(source.relative_to(ROOT))] = sha(source)
    patches = {}
    texts = {}

    def patch(relative, old, new, count=1):
        source = texts.get(relative, (VENDOR / relative).read_text(encoding="utf-8"))
        if source.count(old) != count:
            raise ValueError(f"Unexpected Android patch anchor in {relative}")
        texts[relative] = source.replace(old, new)
        entry = patches.setdefault(relative, {"original_sha256": sha(VENDOR / relative), "changes": []})
        entry["changes"].append({"before": old, "after": new})

    patch("dll/common_includes.h", "#if defined(__x86_64__)", "#if defined(__x86_64__) || defined(__aarch64__)")
    patch("sdk_includes/steamclientpublic.h", "#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)\n// The 32-bit", "#if defined(__ANDROID__)\n#define VALVE_CALLBACK_PACK_LARGE\n#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)\n// The 32-bit")
    patch("dll/base.cpp", "char *buffer = get_current_dir_name();", "char *buffer = getcwd(nullptr, 0);")
    patch("dll/base.cpp", "char *buffer = canonicalize_file_name(path.c_str());", "char *buffer = realpath(path.c_str(), nullptr);")
    patch("dll/settings_parser.cpp", "bool steam_offline_mode = false;\n    bool disable_networking = false;\n    bool disable_overlay = false;\n    bool disable_lobby_creation = false;", "bool steam_offline_mode = true;\n    bool disable_networking = true;\n    bool disable_overlay = true;\n    bool disable_lobby_creation = true;")
    patch("dll/settings_parser.cpp", "settings_client->unlockAllDLC(true);\n            settings_server->unlockAllDLC(true);", "settings_client->unlockAllDLC(false);\n            settings_server->unlockAllDLC(false);")
    patch("dll/settings_parser.cpp", "std::string path = canonical_path(program_path + rel_path);", "std::string path = canonical_path(!rel_path.empty() && rel_path[0] == '/' ? rel_path : program_path + rel_path);")
    patch("dll/network.cpp", "bool disable_sockets)\n{\n    tcp_port", "bool disable_sockets)\n{\n    disable_sockets = true; // Android local-only policy, even without configuration.\n    tcp_port")
    source = (VENDOR / "dll/steam_remote_storage.h").read_text(encoding="utf-8")
    original = sha(VENDOR / "dll/steam_remote_storage.h")
    for function in ("IsCloudEnabledForAccount", "IsCloudEnabledForApp", "IsCloudEnabledThisApp"):
        import re
        pattern = rf"(bool {function}\(\)\n\{{\n    PRINT_DEBUG\([^\n]+\n    )return [^;]+;"
        source, count = re.subn(pattern, r"\1return false;", source)
        if count != 1:
            raise ValueError("Unexpected cloud policy method: " + function)
    source = source.replace("steam_cloud_enabled = true;", "steam_cloud_enabled = false;")
    source = source.replace("steam_cloud_enabled = bEnabled;", "steam_cloud_enabled = false;")
    texts["dll/steam_remote_storage.h"] = source
    patches["dll/steam_remote_storage.h"] = {"original_sha256": original, "policy": "cloud status false, cloud enable ignored, local storage remains real"}
    for relative, source in texts.items():
        path = overlay / relative
        if not path.exists() or path.read_text(encoding="utf-8") != source:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
        patches[relative]["patched_sha256"] = sha(path)
    return overlay, inputs, patches


def verify_protobuf():
    trees = {}
    for name, expected in ARCHIVES.items():
        archive = PROTO / name
        if sha(archive) != expected:
            raise ValueError("Official protobuf release asset changed: " + name)
        destination = PROTO / ("source" if name.startswith("protobuf-") else "protoc-win64")
        tree = hashlib.sha256()
        with zipfile.ZipFile(archive) as package:
            members = sorted((m for m in package.infolist() if not m.is_dir()), key=lambda m: m.filename)
            for member in members:
                expected_file = hashlib.sha256(package.read(member)).hexdigest()
                actual = destination / member.filename
                if not actual.is_file() or sha(actual) != expected_file:
                    raise ValueError("Extracted official protobuf input differs: " + str(actual))
                tree.update((member.filename + "\0" + expected_file + "\n").encode("utf-8"))
        trees[name] = {"archive_sha256": expected, "verified_file_count": len(members), "extracted_tree_sha256": tree.hexdigest()}
    return trees


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.run and not args.serial:
        parser.error("--run requires --serial")
    OUT.mkdir(parents=True, exist_ok=True)
    protobuf_trees = verify_protobuf()
    overlay, inputs, patches = prepare_overlay()
    config = json.loads((ROOT / "port.json").read_text())
    sdk = Path(config["android_sdk"])
    ndk = sdk / "ndk" / config["android_ndk_version"]
    cmake = sdk / "cmake/3.22.1/bin/cmake.exe"
    ninja = cmake.parent / "ninja.exe"
    clang = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe"
    readelf = clang.parent / "llvm-readelf.exe"
    generated = OUT / "generated"
    generated.mkdir(exist_ok=True)
    protoc = PROTO / "protoc-win64/bin/protoc.exe"
    proto_identity = {"compiler_sha256": sha(protoc), "proto_sha256": sha(overlay / "dll/net.proto")}
    proto_receipt = generated / "protoc-inputs.json"
    previous = json.loads(proto_receipt.read_text()) if proto_receipt.exists() else {}
    outputs = (generated / "net.pb.cc", generated / "net.pb.h")
    if (previous.get("inputs") != proto_identity or any(not p.exists() or sha(p) != previous.get("outputs", {}).get(p.name) for p in outputs)):
        candidate = generated / "protoc-check"
        candidate.mkdir(exist_ok=True)
        checked([protoc, "--cpp_out=" + str(candidate), "-I" + str(overlay / "dll"), overlay / "dll/net.proto"])
        for path in outputs:
            if not path.exists() or sha(path) != sha(candidate / path.name):
                shutil.copyfile(candidate / path.name, path)
        proto_receipt.write_text(json.dumps({"inputs": proto_identity, "outputs": {p.name: sha(p) for p in outputs}}, indent=2) + "\n")
    protobuf = PROTO / "source/protobuf-3.21.12"
    build = OUT / "native"
    configure = [cmake, "-S", ROOT / "android/steam", "-B", build, "-G", "Ninja",
                 "-DCMAKE_MAKE_PROGRAM=" + str(ninja), "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
                 "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30", "-DANDROID_STL=c++_static",
                 "-DCMAKE_BUILD_TYPE=Release", "-DGOLDBERG_SOURCE=" + overlay.as_posix(), "-DPROTOBUF_SOURCE=" + protobuf.as_posix(), "-DGENERATED_DIR=" + generated.as_posix()]
    print("Configure native offline backend", flush=True)
    checked(configure)
    print("Compile Bionic ARM64 backend and protobuf-lite", flush=True)
    result = subprocess.run([str(cmake), "--build", str(build), "--target", "frostpunk_offline_services", "offline_services_probe", "--parallel", "3"], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    (OUT / "compile.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr)[-20000:])
    runtime = OUT / "runtime"
    runtime.mkdir(exist_ok=True)
    names = ("libfrostpunk_offline_services.so", "offline_services_probe")
    for name in names:
        shutil.copyfile(build / name, runtime / name)
    libraries = checked([readelf, "-d", runtime / names[0]])
    symbols = checked([readelf, "--dyn-syms", "--wide", runtime / names[0]])
    if "crack_SteamAPI_Init" in symbols or "GLIBC_" in symbols:
        raise ValueError("Forbidden experimental/glibc runtime symbol")
    import re
    if re.search(r"\bUND\s+socket(?:@|\s|$)", symbols):
        raise ValueError("Backend still imports an unwrapped network socket function")
    licenses = runtime / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(VENDOR / "LICENSE", licenses / "Goldberg-LGPL-3.0-or-later.txt")
    shutil.copyfile(protobuf / "LICENSE", licenses / "protobuf-BSD-3-Clause.txt")
    shutil.copyfile(ndk / "NOTICE.toolchain", licenses / "NDK-NOTICE.toolchain.txt")
    for name in ("CMakeLists.txt", "dlc_config.h.in", "offline_policy.cpp", "probe.cpp"):
        path = ROOT / "android/steam" / name
        inputs[str(path.relative_to(ROOT))] = sha(path)
    dlc = ROOT / "config/DLC.txt"
    inputs[str(dlc.relative_to(ROOT))] = sha(dlc)
    inputs[str(Path(__file__).resolve().relative_to(ROOT))] = sha(Path(__file__).resolve())
    report = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "target": "aarch64-linux-android30",
              "goldberg_commit": REVISION, "protobuf_release": "v21.12", "release_archives": ARCHIVES, "protobuf_verified_trees": protobuf_trees,
              "source_inputs": inputs, "private_overlay_patches": patches, "excluded_glibc_source": "dll/wrap.cpp",
              "experimental": False, "overlay": False, "offline": True, "network_hard_deny": "all DSO socket calls wrapped to EPERM",
              "cloud": False, "configured_DLC": [int(line.split("=", 1)[0]) for line in dlc.read_text(encoding="utf-8").splitlines()], "callback_pack": 8, "compiler_sha256": sha(clang),
              "storage_path_guard": "side-effect-free openat/O_NOFOLLOW preflight against existing parents and final file, not atomic with upstream fstream IO",
              "final_application_profile_requirement": "app-private profile with no untrusted concurrent filesystem writer",
              "unwrapped_socket_elf_import": False,
              "configure_command": [str(x) for x in configure], "needed_elf": libraries,
              "binaries": {name: {"sha256": sha(runtime / name), "bytes": (runtime / name).stat().st_size} for name in names},
              "native_implementation": True, "wine_box64": False, "game_boot_proven": False}
    # The bridge verifies this host build receipt even when --run also records
    # separate device evidence, so both receipts must describe the same DSO.
    (ROOT / "research/offline-services-build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.run:
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", args.serial]
        remote = "/data/local/tmp/frostpunk/offline-services"
        checked(adb + ["shell", "mkdir", "-p", remote + "/profile-fixture"])
        for name in names:
            checked(adb + ["push", runtime / name, remote + "/" + name])
            remote_hash = checked(adb + ["shell", "sha256sum", remote + "/" + name]).split()[0]
            if remote_hash != report["binaries"][name]["sha256"]:
                raise RuntimeError("Device hash mismatch: " + name)
        checked(adb + ["shell", "chmod", "755", remote + "/offline_services_probe"])
        runs = []
        for mode in ("write", "read"):
            command = adb + ["shell", "env", "LD_LIBRARY_PATH=" + remote, remote + "/offline_services_probe", remote + "/" + names[0], remote + "/profile-fixture", "/sdcard/Frostpunk/game", mode]
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            runs.append({"mode": mode, "command": command, "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
            if result.returncode or not json.loads(result.stdout).get("pass"):
                report["runs"] = runs
                (ROOT / "research/offline-services-failed.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
                raise RuntimeError("Native backend fixture failed: " + result.stdout + result.stderr)
        report["serial"] = args.serial
        report["runs"] = runs
        report["profile_persistence_fresh_process"] = True
    receipt = ROOT / ("research/offline-services.json" if args.run else "research/offline-services-build.json")
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"build_pass": True, "device_pass": args.run, "receipt": str(receipt), "binaries": report["binaries"]}, indent=2))


if __name__ == "__main__":
    main()
