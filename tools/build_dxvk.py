"""Pinned DXVK Native -> Android/Bionic AArch64; no Wine/Box64 execution.

Pristine upstream inputs remain under reference/dxvk. Reproducible Android
overlays are applied only to build/dxvk-source. No phone commands here.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
import dxvk_mali_overlay
import dxvk_robustness_shader_overlay
import dxvk_robustness_backend_overlay
import dxvk_astc_overlay
import dxvk_present_overlay
import dxvk_scaling_overlay
import dxvk_pipeline_cache_overlay
import dxvk_tiler_overlay
import dxvk_mobile_overlay
import dxvk_fsr_overlay
import dxvk_fsr3_overlay
import dxvk_queue_overlay
import dxvk_latency_overlay
import dxvk_buffer_ownership_overlay
import dxvk_deferred_sequence_overlay
import dxvk_wireframe_fallback_overlay
import dxvk_device_lost_cache_overlay

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "reference/dxvk"
WORK = ROOT / "build/dxvk-source"
OUT = ROOT / "build/dxvk-arm64"
TOOLS = ROOT / "build/dxvk-tools"
REVISION = "0cf05780abd7250c2cd713b7749cf32180157cf5"
SUBMODULES = {
    "include/native/directx": "9df86f2341616ef1888ae59919feaa6d4fad693d",
    "include/spirv": "8b246ff75c6615ba4532fe4fde20f1be090c3764",
    "include/vulkan": "46dc0f6e514f5730784bb2cac2a7c731636839e8",
    "subprojects/libdisplay-info": "275e6459c7ab1ddd4b125f28d0440716e4888078",
}
GLSLANG_URL = "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip"
GLSLANG_SHA256 = "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b"
OVERLAYS = (dxvk_mali_overlay, dxvk_robustness_shader_overlay,
            dxvk_robustness_backend_overlay, dxvk_astc_overlay, dxvk_present_overlay,
            dxvk_scaling_overlay, dxvk_tiler_overlay, dxvk_mobile_overlay,
            dxvk_fsr_overlay, dxvk_fsr3_overlay, dxvk_pipeline_cache_overlay, dxvk_queue_overlay,
            dxvk_latency_overlay, dxvk_buffer_ownership_overlay, dxvk_deferred_sequence_overlay,
            dxvk_wireframe_fallback_overlay, dxvk_device_lost_cache_overlay)
OVERLAY_FILES = tuple(dict.fromkeys(name for overlay in OVERLAYS for name in overlay.FILES))


def run(args, **kw):
    print("RUN", subprocess.list2cmdline([str(a) for a in args]), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kw)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_changed(path, data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def replace(path, before, after):
    s = path.read_text(encoding="utf-8")
    if s.count(before) != 1:
        raise ValueError(f"Upstream overlay mismatch {path}: {before[:70]!r}")
    write_changed(path, s.replace(before, after))


def prepare_gpu_overlays():
    # Compose overlapping compiler edits before changing final source mtimes.
    with tempfile.TemporaryDirectory(prefix="frostpunk-dxvk-") as directory:
        candidate = Path(directory)
        for name in OVERLAY_FILES:
            path = SOURCE / name
            if path.exists():
                write_changed(candidate / name, path.read_bytes())
        for overlay in OVERLAYS:
            if overlay is dxvk_pipeline_cache_overlay:
                native_cache = overlay.apply(candidate, gpu_source=WORK)
            else:
                overlay.apply(candidate)
        for name in OVERLAY_FILES:
            write_changed(WORK / name, (candidate / name).read_bytes())
        return native_cache


def prepare():
    if not SOURCE.exists():
        run(["git", "clone", "--recursive", "--branch", "v2.4.1", "--depth", "1",
             "https://github.com/doitsujin/dxvk.git", SOURCE])
    for p, revision in {".": REVISION, **SUBMODULES}.items():
        actual = subprocess.check_output(["git", "-C", str(SOURCE / p), "rev-parse", "HEAD"], text=True).strip()
        if actual != revision:
            raise ValueError(f"Unexpected DXVK input revision {p}: {actual}")
        dirty = subprocess.check_output(["git", "-C", str(SOURCE / p), "status", "--porcelain", "--untracked-files=no"], text=True)
        if dirty.strip():
            raise ValueError(f"Refuse modified DXVK input: {p}")
    # Avoid changing source mtimes on every invocation, preserving Ninja cache.
    for base, dirs, files in os.walk(SOURCE):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            if name == ".git":
                continue
            p = Path(base) / name
            if p.relative_to(SOURCE).as_posix() in OVERLAY_FILES:
                continue
            write_changed(WORK / p.relative_to(SOURCE), p.read_bytes())
    replace(WORK / "meson.build", "  if (not lib_sdl2.found() and not lib_glfw.found())",
            "  if platform == 'android'\n    compiler_args += ['-DDXVK_WSI_ANDROID']\n    link_args += ['-landroid', '-ldl', '-Wl,-z,max-page-size=16384']\n  endif\n  if platform != 'android' and (not lib_sdl2.found() and not lib_glfw.found())")
    replace(WORK / "src/wsi/meson.build", "  'wsi_platform.cpp',", "  'wsi_platform.cpp',\n  'wsi_android.cpp',")
    replace(WORK / "src/wsi/meson.build", "if platform != 'windows'", "if platform != 'windows' and platform != 'android'")
    write_changed(WORK / "src/wsi/wsi_android.cpp", (ROOT / "android/dxvk/wsi_android.cpp").read_bytes())
    replace(WORK / "src/wsi/wsi_platform.h", "  void init();", "#if defined(DXVK_WSI_ANDROID)\n  extern WsiBootstrap AndroidWSI;\n#endif\n\n  void init();")
    replace(WORK / "src/wsi/wsi_platform.cpp", "  static const WsiBootstrap *wsiBootstrap[] = {", "  static const WsiBootstrap *wsiBootstrap[] = {\n#if defined(DXVK_WSI_ANDROID)\n    &AndroidWSI,\n#endif")
    # The embedding native process must export this provider callback. Refuse
    # stock-loader fallback, so an absent/custom-driver error cannot be hidden.
    replace(WORK / "src/vulkan/vulkan_loader.cpp", "namespace dxvk::vk {", "#if defined(__ANDROID__)\nextern \"C\" PFN_vkGetInstanceProcAddr frostpunk_dxvk_get_instance_proc_addr() __attribute__((weak));\n#endif\n\nnamespace dxvk::vk {")
    replace(WORK / "src/vulkan/vulkan_loader.cpp", "  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {", "  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {\n#if defined(__ANDROID__)\n    auto proc = frostpunk_dxvk_get_instance_proc_addr ? frostpunk_dxvk_get_instance_proc_addr() : nullptr;\n    if (!proc) Logger::err(\"Android: required native Vulkan provider is absent\");\n    return {nullptr, proc};\n#else")
    replace(WORK / "src/vulkan/vulkan_loader.cpp", "    return { };\n  }", "    return { };\n#endif\n  }")
    return prepare_gpu_overlays()


def host_tools():
    TOOLS.mkdir(parents=True, exist_ok=True)
    if not (TOOLS / "python/mesonbuild").exists():
        run([sys.executable, "-m", "pip", "install", "--target", TOOLS / "python", "meson==1.9.1"])
    archive = TOOLS / "glslang.zip"
    if not archive.exists():
        archive.write_bytes(urllib.request.urlopen(GLSLANG_URL, timeout=120).read())
    if sha(archive) != GLSLANG_SHA256:
        raise ValueError("Host glslang archive SHA-256 mismatch")
    dest = TOOLS / "glslang"
    if not (dest / "bin/glslang.exe").exists():
        with zipfile.ZipFile(archive) as z:
            for entry in z.infolist():
                resolved = (dest / entry.filename).resolve()
                if not resolved.is_relative_to(dest.resolve()):
                    raise ValueError("Unsafe glslang archive member")
            z.extractall(dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--configure-only", action="store_true")
    args = ap.parse_args()
    native_cache = prepare()
    host_tools()
    config = json.loads((ROOT / "port.json").read_text())
    sdk = Path(config["android_sdk"])
    ndk = sdk / "ndk" / config["android_ndk_version"]
    llvm = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    ninja = sdk / "cmake/3.22.1/bin/ninja.exe"
    cross = ROOT / "android/dxvk/aarch64-android.ini"
    write_changed(cross, f"""[binaries]
c = ['{llvm.as_posix()}/clang.exe', '--target=aarch64-linux-android30']
cpp = ['{llvm.as_posix()}/clang++.exe', '--target=aarch64-linux-android30']
ar = '{llvm.as_posix()}/llvm-ar.exe'
strip = '{llvm.as_posix()}/llvm-strip.exe'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
""")
    env = os.environ.copy()
    env["PYTHONPATH"] = str(TOOLS / "python")
    env["PATH"] = os.pathsep.join([str(TOOLS / "glslang/bin"), str(ninja.parent),
                                    "C:/Program Files/Git/usr/bin", env["PATH"]])
    meson = [sys.executable, "-m", "mesonbuild.mesonmain"]
    if not (OUT / "build.ninja").exists():
        run(meson + ["setup", OUT, WORK, "--cross-file", cross, "--buildtype", "release",
                     "-Denable_dxgi=true", "-Denable_d3d11=true", "-Denable_d3d10=false",
                     "-Denable_d3d9=false", "-Denable_d3d8=false"], env=env)
    if not args.configure_only:
        pinned = [ROOT / "android/dxvk/wsi_android.cpp", Path(__file__), cross,
                  ndk / "source.properties", llvm / "clang++.exe", TOOLS / "glslang/bin/glslang.exe"]
        pinned += [WORK / path for path in ["meson.build", "src/wsi/meson.build", "src/wsi/wsi_android.cpp",
                   "src/wsi/wsi_platform.h", "src/wsi/wsi_platform.cpp", "src/vulkan/vulkan_loader.cpp",
                   "src/d3d11/d3d11_swapchain.cpp"]]
        pinned += [Path(overlay.__file__) for overlay in OVERLAYS]
        pinned += [WORK / name for name in OVERLAY_FILES]
        pinned += [ROOT / name for name in native_cache['implementation_hashes']]
        pinned += [WORK / name for name in native_cache['gpu_source_hashes']]
        before = {str(path): sha(path) for path in pinned}
        run([ninja, "-C", OUT, "-j", str(args.jobs)], env=env)
        if any(sha(Path(path)) != expected for path, expected in before.items()):
            raise ValueError("DXVK compiler or Android overlay changed during build")
        outputs = [p for p in OUT.rglob("*.so*") if p.is_file() and p.read_bytes()[:4] == b"\x7fELF"]
        receipt = {"upstream": "https://github.com/doitsujin/dxvk", "revision": REVISION,
                   "submodules": SUBMODULES, "target": "aarch64-linux-android30", "ndk": config["android_ndk_version"],
                   "host_glslang_zip_sha256": GLSLANG_SHA256, "android_wsi_sha256": sha(ROOT / "android/dxvk/wsi_android.cpp"),
                   "build_tool_sha256": sha(Path(__file__)), "compiler_sha256": sha(llvm / "clang++.exe"),
                   "android_present_policy": "D3D11 accepts raw successful SUBOPTIMAL without redundant recreate; compositor handles IDENTITY rotation; error/resize paths unchanged",
                   "android_guest_display_policy": "Only explicit host callback enables one logical monitor; desktop/current/enumerated/window extent agree; physical Android surface and presentation stay native; exclusive mode changes unsupported",
                   "android_guest_display_callback": "int frostpunk_dxvk_get_display_mode(uint32_t*,uint32_t*,uint32_t*)",
                   "android_guest_monitor_id": 1, "build_inputs_sha256": before,
                   "android_present_overlay_sha256": sha(WORK / "src/d3d11/d3d11_swapchain.cpp"),
                   "ndk_source_properties_sha256": sha(ndk / "source.properties"),
                   "phone_executed": False, "game_rendered": False,
                   "android_mali_profile": "Bounded game profile; unsupported dual-source blend, wireframe, geometry streams, multiple viewports and clip/cull shaders rejected; core vertex/index robustness retained, full FL11 conformance not claimed",
                   "android_present_measurement": "Optional Android callback after successful unique nonzero-frame-id vkQueuePresentKHR; repeated frames, TEST and errors excluded; accepted submission cadence, not scanout",
                   "android_mali_robustness_abi": dxvk_robustness_backend_overlay.ABI,
                   "android_astc_private_formats": dxvk_astc_overlay.FORMATS,
                   "android_native_pipeline_cache": native_cache,
                   "outputs": [{"path": str(p.relative_to(ROOT)), "bytes": p.stat().st_size, "sha256": sha(p)} for p in outputs]}
        write_changed(OUT / "build-receipt.json", json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    main()
