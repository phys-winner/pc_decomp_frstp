"""Build pinned independent adrenotools/hooks and a real BC3 GPU sampling proof.

    python tools/build_gpu_runtime.py
    python tools/build_gpu_runtime.py --run

--run uses only /data/local/tmp/frostpunk/gpu-runtime, with no installed-app
libraries, system driver changes, app launches/stops, CPU translator or turbo.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "reference/adrenotools"
REVISION = "8fae8ce254dfc1344527e05301e43f37dea2df80"
LINKER_REVISION = "aa3975893d83ef1bc84c321ec60c65fbf1287887"
DRIVER_SHA = "fdd378520022f88b0363dd1f77f6989332730271712621523075fe4eb4de2a09"
SM8250_DRIVER_SHA = "25b3757ebbf85cffd7358c8f370aa389c9fbab5e27327d97889695e4bc684569"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sm8250_driver():
    path = ROOT / 'reference/turnip-sm8250/vulkan.sm8250.so'
    if digest(path) != SM8250_DRIVER_SHA:
        raise ValueError('Pinned SM8250 Turnip identity changed')
    return path


def checked(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace", **kwargs).stdout.strip()


def git_sources(repo):
    result = {}
    for name in checked(["git", "-C", repo, "ls-files"]).splitlines():
        path = repo / name
        if path.is_file():
            result[str(path.relative_to(ROOT))] = digest(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    args = parser.parse_args()
    if checked(["git", "-C", VENDOR, "rev-parse", "HEAD"]) != REVISION:
        raise ValueError("adrenotools source revision differs from the pin")
    linker = VENDOR / "lib/linkernsbypass"
    if checked(["git", "-C", linker, "rev-parse", "HEAD"]) != LINKER_REVISION:
        raise ValueError("linkernsbypass source revision differs from the pin")
    for repo in (VENDOR, linker):
        if checked(["git", "-C", repo, "status", "--porcelain", "--untracked-files=no"]):
            raise ValueError(f"Pinned source modified: {repo}")
    config = json.loads((ROOT / "port.json").read_text())
    sdk = Path(config["android_sdk"])
    ndk = sdk / "ndk" / config["android_ndk_version"]
    cmake = sdk / "cmake/3.22.1/bin/cmake.exe"
    ninja = cmake.parent / "ninja.exe"
    glslc = ndk / "shader-tools/windows-x86_64/glslc.exe"
    spirv_val = ndk / "shader-tools/windows-x86_64/spirv-val.exe"
    clang = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
    readelf = clang.parent / "llvm-readelf.exe"
    cpp = clang.parent.parent / "sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
    out = ROOT / "build/gpu-runtime"
    generated = out / "generated"
    runtime = out / "runtime"
    generated.mkdir(parents=True, exist_ok=True)
    runtime.mkdir(parents=True, exist_ok=True)
    inputs = {str(p.relative_to(ROOT)): digest(p) for p in (ROOT / "android/gpu").glob("*") if p.is_file()}
    inputs[str(Path(__file__).resolve().relative_to(ROOT))] = digest(Path(__file__).resolve())
    inputs[str(cpp)] = digest(cpp)
    inputs.update(git_sources(VENDOR))
    inputs.update(git_sources(linker))
    shader = generated / "bc3_sample.spv"
    checked([glslc, "--target-env=vulkan1.0", "-O", ROOT / "android/gpu/bc3_sample.comp", "-o", shader])
    checked([spirv_val, "--target-env", "vulkan1.0", shader])
    words = struct.unpack("<" + "I" * (shader.stat().st_size // 4), shader.read_bytes())
    (generated / "bc3_sample.spv.h").write_text("#include <stdint.h>\nstatic const uint32_t bc3_sample_spv[]={\n" +
        ",".join(f"0x{word:08x}u" for word in words) + "\n};\n", encoding="ascii")
    build = out / "native"
    configure = [cmake, "-S", ROOT / "android/gpu", "-B", build, "-G", "Ninja",
                 "-DCMAKE_MAKE_PROGRAM=" + str(ninja),
                 "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"),
                 "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30", "-DANDROID_STL=c++_shared",
                 "-DCMAKE_BUILD_TYPE=Release", "-DGPU_GENERATED_DIR=" + str(generated),
                 "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384"]
    checked(configure)
    compiled = subprocess.run([str(cmake), "--build", str(build), "--parallel", "4"], capture_output=True,
                              text=True, encoding="utf-8", errors="replace", timeout=60)
    (out / "compile.log").write_text(compiled.stdout + compiled.stderr, encoding="utf-8")
    if compiled.returncode:
        raise RuntimeError(compiled.stdout + compiled.stderr)
    copies = {"libadrenotools.so": build / "adrenotools/libadrenotools.so",
              "libc++_shared.so": cpp,
              "frostpunk_gpu_probe": build / "frostpunk_gpu_probe"}
    copies.update({name: build / "adrenotools/src/hook" / name for name in
                   ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")})
    for name, source in copies.items():
        shutil.copyfile(source, runtime / name)
    driver = ROOT / "reference/turnip-android/vulkan.ad07xx.so"
    if digest(driver) != DRIVER_SHA:
        raise ValueError("Pinned Bionic Turnip identity changed")
    shutil.copyfile(driver, runtime / driver.name)
    licenses = runtime / "licenses"
    licenses.mkdir(exist_ok=True)
    shutil.copyfile(VENDOR / "LICENSE", licenses / "adrenotools.BSD-2-Clause.txt")
    shutil.copyfile(linker / "LICENSE", licenses / "linkernsbypass.BSD-2-Clause.txt")
    shutil.copyfile(ROOT / "reference/turnip-android/LICENSE-NOTE.txt", licenses / "Turnip-LICENSE-NOTE.txt")
    # All helpers share OpenAL/DXVK's STL to keep locale and vtable identities consistent.
    shutil.copyfile(ndk / "NOTICE.toolchain", licenses / "NDK-NOTICE.toolchain.txt")
    elf_dependencies = {name: checked([readelf, "-d", runtime / name]) for name in copies}
    if any("winlator" in text.lower() or "vortek" in text.lower() for text in elf_dependencies.values()):
        raise ValueError("Unexpected installed-app binary dependency")
    if "[libc++_shared.so]" not in elf_dependencies["libadrenotools.so"]:
        raise ValueError("adrenotools must use the shared C++ runtime")
    helper_cpp_exports = {}
    runtime_symbols = ("_ZNSt6__ndk16locale", "_ZNKSt6__ndk18ios_base6getloc",
                       "_ZNSt6__ndk18ios_base4init", "_ZTTNSt6__ndk1", "_ZTVNSt6__ndk1")
    for name in copies:
        if name == "libc++_shared.so":
            continue
        symbols = checked([readelf, "--dyn-syms", "--wide", runtime / name])
        helper_cpp_exports[name] = [line.split()[-1] for line in symbols.splitlines()
                                    if "__ndk1" in line and " UND " not in line]
        if any(symbol.startswith(runtime_symbols) for symbol in helper_cpp_exports[name]):
            raise ValueError("GPU helper exports conflicting STL locale/vtable objects: " + name)
    for name, expected in inputs.items():
        if digest(ROOT / name) != expected:
            raise ValueError("GPU source changed during compilation: " + name)
    hashes = {p.name: digest(p) for p in runtime.iterdir() if p.is_file()}
    report = {"recorded_at": datetime.now(timezone.utc).isoformat(), "serial": config["device"],
              "target": "aarch64-linux-android30", "adrenotools_url": "https://github.com/bylaws/libadrenotools",
              "adrenotools_revision": REVISION, "linkernsbypass_url": "https://github.com/bylaws/liblinkernsbypass",
              "linkernsbypass_revision": LINKER_REVISION, "licenses": "BSD-2-Clause for adrenotools and linkernsbypass; full texts copied with runtime",
              "sources": inputs, "compiler_sha256": digest(clang), "shader_compiler_sha256": digest(glslc),
              "ndk_revision": config["android_ndk_version"], "cmake_sha256": digest(cmake),
              "configure_command": [str(x) for x in configure],
              "shader_sha256": digest(shader), "runtime_sha256": hashes, "elf_dynamic_sections": elf_dependencies,
              "android_stl": "c++_shared", "helper_cpp_exports": helper_cpp_exports,
              "license_sha256": {p.name: digest(p) for p in licenses.iterdir() if p.is_file()},
              "installed_app_libraries_used": False, "original_game_rendered": False, "fps_measured": False,
              "executed": False}
    if args.run:
        prefix = [sdk / "platform-tools/adb.exe", "-s", config["device"]]
        if checked([*prefix, "get-state"]) != "device":
            raise RuntimeError("Configured phone unavailable")
        remote = "/data/local/tmp/frostpunk/gpu-runtime"
        checked([*prefix, "shell", "mkdir -p " + remote])
        for name, expected in hashes.items():
            checked([*prefix, "push", runtime / name, remote + "/" + name])
            if checked([*prefix, "shell", "sha256sum " + remote + "/" + name]).split()[0] != expected:
                raise ValueError("Uploaded GPU runtime identity mismatch: " + name)
        checked([*prefix, "shell", "chmod 700 " + remote + " " + remote + "/*"])
        command = "LD_LIBRARY_PATH=" + remote + " " + remote + "/frostpunk_gpu_probe " + remote + " " + remote + "/"
        result = subprocess.run([str(x) for x in prefix] + ["shell", command], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        try:
            output = json.loads(result.stdout)
        except json.JSONDecodeError:
            output = None
        report.update({"executed": True, "command": command, "returncode": result.returncode,
                       "stdout": result.stdout, "stderr": result.stderr, "gpu_result": output,
                       "gpu_sampling_passed": bool(result.returncode == 0 and output and output.get("success"))})
    receipt_name = "gpu-runtime.json" if args.run else "gpu-runtime-build.json"
    (ROOT / "research" / receipt_name).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (ROOT / "reference/adrenotools.provenance.json").write_text(json.dumps({
        "url": report["adrenotools_url"], "revision": REVISION, "license": "BSD-2-Clause",
        "submodule": {"url": report["linkernsbypass_url"], "revision": LINKER_REVISION, "license": "BSD-2-Clause"},
        "sources_sha256": {k:v for k,v in inputs.items() if k.startswith("reference/")}
    }, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items() if k not in ("sources", "elf_dynamic_sections")}, indent=2))
    if args.run and not report["gpu_sampling_passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
