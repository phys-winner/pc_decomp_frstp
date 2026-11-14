"""Build a real native Android D3D11/DXBC/BC3 readback probe, without deployment."""
import argparse
import ctypes
import datetime
import json
from pathlib import Path
import shutil
import subprocess
import uuid

import build_dxvk as dxvk

ROOT = dxvk.ROOT
SYSTEM = json.loads((ROOT / "port.json").read_text()).get("gpu_provider", "turnip") == "system"
OUT = ROOT / ("build/mali-d3d11-probe" if SYSTEM else "build/dxvk-probe")


def compile_dxbc():
    source = (ROOT / "tests/nativeD3D11.hlsl").read_bytes()
    compiler_path = Path("C:/Windows/System32/D3DCompiler_47.dll")
    compiler = ctypes.WinDLL(str(compiler_path))
    compile_ = compiler.D3DCompile
    compile_.restype = ctypes.c_int32
    compile_.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]

    def blob_data(blob):
        vtable = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])(blob)
        size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtable[4])(blob)
        return ctypes.string_at(pointer, size)

    def release(blob):
        if blob:
            vtable = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
            ctypes.WINFUNCTYPE(ctypes.c_uint32, ctypes.c_void_p)(vtable[2])(blob)

    outputs = {}
    declarations = []
    for name, entry, profile in [("vs", b"vs_main", b"vs_5_0"), ("ps", b"ps_main", b"ps_5_0")]:
        code, error = ctypes.c_void_p(), ctypes.c_void_p()
        try:
            hr = compile_(source, len(source), b"nativeD3D11.hlsl", None, None,
                          entry, profile, (1 << 11) | (1 << 15), 0, ctypes.byref(code), ctypes.byref(error))
            if hr < 0:
                detail = blob_data(error).decode(errors="replace") if error else "no error blob"
                raise RuntimeError(f"D3DCompile {profile!r} HRESULT {hr:#x}: {detail}")
            data = blob_data(code)
            if data[:4] != b"DXBC":
                raise ValueError("D3DCompile did not produce DXBC")
            p = OUT / f"native_{name}.dxbc"
            dxvk.write_changed(p, data)
            outputs[name] = {"bytes": len(data), "sha256": dxvk.sha(p), "profile": profile.decode()}
            declarations.append(f"static const unsigned char native_{name}[] = {{\n" +
                ",\n".join(",".join(str(v) for v in data[i:i+24]) for i in range(0, len(data), 24)) + "\n};\n")
        finally:
            release(code)
            release(error)
    dxvk.write_changed(OUT / "nativeD3D11_shaders.h", "\n".join(declarations))
    return {"source_sha256": dxvk.sha(ROOT / "tests/nativeD3D11.hlsl"),
            "compiler": str(compiler_path), "compiler_sha256": dxvk.sha(compiler_path), "outputs": outputs}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--run", action="store_true", help="Deploy and execute on the configured device after building")
    ap.add_argument("--run-only", action="store_true", help="Verify existing build hashes, deploy and execute")
    args = ap.parse_args()
    if args.run_only:
        run_probe()
        return
    OUT.mkdir(parents=True, exist_ok=True)
    source_paths = [ROOT / "tests/nativeD3D11.cpp", ROOT / "tests/nativeD3D11.hlsl", ROOT / "android/gpu/driver.c",
                    ROOT / "android/gpu/driver.h", Path(__file__), ROOT / "port.json", ROOT / "build/dxvk-arm64/build-receipt.json"]
    source_pins = {str(path): dxvk.sha(path) for path in source_paths}
    shaders = compile_dxbc()
    cfg = json.loads((ROOT / "port.json").read_text())
    ndk = Path(cfg["android_sdk"]) / "ndk" / cfg["android_ndk_version"]
    llvm = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    include = [ROOT / "reference/dxvk/include/native/windows", ROOT / "reference/dxvk/include/native/directx",
               ROOT / "reference/dxvk/include/vulkan/include", ROOT / "android/gpu",
               ROOT / "reference/adrenotools/include", OUT]
    common = ["--target=aarch64-linux-android30", "-O2", "-fPIC", "-Wall", "-Wextra"]
    if SYSTEM:
        common += ["-DFP_GPU_SYSTEM=1"]
    for p in include:
        common += ["-I", str(p)]
    dxvk.run([llvm / "clang.exe", *common, "-std=c11", "-c", ROOT / "android/gpu/driver.c", "-o", OUT / "driver.o"])
    dxvk.run([llvm / "clang++.exe", *common, "-std=c++17", ROOT / "tests/nativeD3D11.cpp", OUT / "driver.o",
              *([] if SYSTEM else ["-L", ROOT / "build/gpu-runtime/runtime", "-ladrenotools"]), "-ldl", "-Wl,--export-dynamic",
              "-Wl,-z,max-page-size=16384", "-o", OUT / "nativeD3D11"])
    # Materialize SONAMEs for adb push (Windows Meson uses text aliases rather
    # than Unix symlinks); resolve the real libraries from the build receipt.
    libs = []
    for module in ["dxgi", "d3d11"]:
        directory = ROOT / f"build/dxvk-arm64/src/{module}"
        files = [p for p in directory.glob("*.so*") if p.is_file() and p.read_bytes()[:4] == b"\x7fELF"]
        if len(files) != 1:
            raise RuntimeError(f"Expected exactly one ELF DXVK {module}, got {files}")
        real = files[0]
        readelf = subprocess.check_output([str(llvm / "llvm-readelf.exe"), "-d", str(real)], text=True)
        soname = next(line.split("[", 1)[1].split("]", 1)[0] for line in readelf.splitlines() if "(SONAME)" in line)
        target = OUT / soname
        shutil.copyfile(real, target)
        libs.append({"path": str(target.relative_to(ROOT)), "bytes": target.stat().st_size, "sha256": dxvk.sha(target)})
        if module == "d3d11":
            shutil.copyfile(real, OUT / "libdxvk_d3d11.so")
    # Shared NDK C++ runtime if selected by the linker.
    cpp_runtime = ndk / "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
    shutil.copyfile(cpp_runtime, OUT / cpp_runtime.name)
    if any(dxvk.sha(Path(path))!=expected for path,expected in source_pins.items()):
        raise ValueError("Native D3D11 probe inputs changed during build")
    receipt = {"target": "aarch64-linux-android30", "gpu_provider": "system" if SYSTEM else "turnip", "dxvk_revision": dxvk.REVISION,
               "source_sha256": dxvk.sha(ROOT / "tests/nativeD3D11.cpp"), "shaders": shaders,
               "build_tool_sha256": dxvk.sha(Path(__file__)), "compiler_sha256": dxvk.sha(llvm / "clang++.exe"),
               "provider_source_sha256": dxvk.sha(ROOT / "android/gpu/driver.c"),
               "binary_sha256": dxvk.sha(OUT / "nativeD3D11"), "libraries": libs,
               "inputs_sha256":source_pins,"dxvk_build_receipt_sha256":dxvk.sha(ROOT / "build/dxvk-arm64/build-receipt.json"),
               "compiled": True, "phone_executed": False, "game_rendered": False, "fps_measured": False}
    dxvk.write_changed(OUT / "build-receipt.json", json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))
    if args.run:
        run_probe()


def run_probe():
    cfg = json.loads((ROOT / "port.json").read_text())
    serial = cfg["device"]
    adb = str(Path(cfg["android_sdk"]) / "platform-tools/adb.exe")
    build = json.loads((OUT / "build-receipt.json").read_text())
    if build.get("gpu_provider", "turnip") != ("system" if SYSTEM else "turnip"):
        raise ValueError("Probe provider differs from current configuration")
    for path, expected in build["inputs_sha256"].items():
        if dxvk.sha(Path(path)) != expected:
            raise ValueError("Probe source or DXVK receipt changed: " + path)
    if dxvk.sha(OUT / "nativeD3D11") != build["binary_sha256"]:
        raise RuntimeError("Probe binary no longer matches build receipt")
    for lib in build["libraries"]:
        if dxvk.sha(ROOT / lib["path"]) != lib["sha256"]:
            raise RuntimeError("DXVK library no longer matches build receipt")
    base = "/data/local/tmp/frostpunk/mali-d3d11-probe" if SYSTEM else "/data/local/tmp/frostpunk/dxvk"
    gpu = "/data/local/tmp/frostpunk/gpu-runtime"
    files = ["nativeD3D11", "libdxvk_d3d11.so", "libdxvk_dxgi.so", "libc++_shared.so"]
    def adb_call(arguments, timeout=30):
        return subprocess.run([adb, "-s", serial, *arguments], capture_output=True,
                              text=True, timeout=timeout)
    adb_call(["shell", "mkdir", "-p", base]).check_returncode()
    for name in files:
        adb_call(["push", str(OUT / name), base + "/" + name]).check_returncode()
    adb_call(["shell", "chmod", "755", base + "/nativeD3D11"]).check_returncode()
    result = adb_call(["shell", "env", "LD_LIBRARY_PATH=" + base + ("" if SYSTEM else ":" + gpu),
                      base + "/nativeD3D11", gpu, gpu + "/", base + "/libdxvk_d3d11.so"], 90)
    stamp = datetime.datetime.now(datetime.timezone.utc)
    run_dir = OUT / "runs" / (stamp.strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])
    run_dir.mkdir(parents=True)
    (run_dir / "stdout.txt").write_text(result.stdout, encoding="utf-8")
    (run_dir / "stderr.txt").write_text(result.stderr, encoding="utf-8")
    receipt = {"recorded_at": stamp.isoformat(), "serial": serial, "build": build,
               "dxvk_build": json.loads((ROOT / "build/dxvk-arm64/build-receipt.json").read_text()),
               "provider": {"gpu_provider": "system"} if SYSTEM else json.loads((ROOT / "research/gpu-runtime.json").read_text()),
               "returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
               "phone_executed": True, "deployed": files, "game_rendered": False, "fps_measured": False}
    try:
        receipt["result"] = json.loads(result.stdout)
    except ValueError:
        receipt["result"] = None
    paths = [(base + "/" + n, OUT / n) for n in files]
    paths += [] if SYSTEM else [(gpu + "/" + n, ROOT / "build/gpu-runtime/runtime" / n) for n in
              ["libadrenotools.so", "libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so",
               "libgsl_alloc_hook.so", "vulkan.ad07xx.so"]]
    verification = adb_call(["shell", "sha256sum", *[p for p, _ in paths]])
    verification.check_returncode()
    hashes = {line.split()[1]: line.split()[0] for line in verification.stdout.splitlines()}
    checks = [{"device_path": remote, "host_path": str(local.relative_to(ROOT)),
               "sha256": dxvk.sha(local), "device_sha256": hashes.get(remote),
               "match": hashes.get(remote) == dxvk.sha(local)} for remote, local in paths]
    receipt["deployed_sha256_verified"] = checks
    data = receipt["result"] or {}
    receipt["checks_passed"] = (result.returncode == 0 and data.get("success") is True
        and data.get("feature_level") == 0xb000 and data.get("checked_pixels") == 16
        and data.get("matched_pixels") == 16 and all(c["match"] for c in checks))
    receipt["checks_passed"] = receipt["checks_passed"] and data.get("no_policy_no_outputs") is True and data.get("explicit_virtual_display_pass") is True
    if SYSTEM:
        receipt["checks_passed"] = receipt["checks_passed"] and data.get("unsupported_operations_rejected") is True
    text = json.dumps(receipt, indent=2) + "\n"
    dxvk.write_changed(run_dir / "receipt.json", text)
    dxvk.write_changed(ROOT / ("research/mali-d3d11-probe.json" if SYSTEM else "research/dxvk-native-probe.json"), text)
    print(result.stdout, end="")
    if not receipt["checks_passed"]:
        raise RuntimeError(f"Native DXVK probe failed; evidence preserved in {run_dir}")


if __name__ == "__main__":
    main()
