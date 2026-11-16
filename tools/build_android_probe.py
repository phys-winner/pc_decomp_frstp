"""Build an isolated NativeActivity/Android-window/Vulkan/input fixture APK.

No game assets or successful game HWND APIs are included. --dxvk adds a tiny
offline-lifted Win64 fixture and the existing native COM bridge. --run only
installs, launches and stops this task's explicit diagnostic package.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "android/app"
OUT = ROOT / "build/app"
PACKAGE = "com.frostpunk.staticrecomp.probe"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checked(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command[0]} failed ({result.returncode})\n{result.stdout[-12000:]}\n{result.stderr[-8000:]}")
    return result.stdout.strip()


def generate_fixture(ndk, out):
    """Compile synthetic x64 assembly and statically lift its two functions."""
    llvm = ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
    obj = out / "guest_present.obj"
    raw = out / "guest_present.text"
    checked([llvm / "clang.exe", "--target=x86_64-pc-windows-msvc", "-c", SOURCE / "guest_present.S", "-o", obj])
    checked([llvm / "llvm-objcopy.exe", "--dump-section", ".text=" + str(raw), obj])
    listing = checked([llvm / "llvm-nm.exe", "--format=posix", "--defined-only", obj])
    symbols = {line.split()[0]: int(line.split()[2], 16) for line in listing.splitlines() if len(line.split()) >= 3}
    from lift64_checked import CheckedLifter
    lifter = CheckedLifter(image_size=0x10000, image_base=0x180000000)
    code = raw.read_bytes()
    start, draw = symbols["guest_create"], symbols["guest_draw"]
    if start != 0 or not 0 < draw < len(code):
        raise ValueError("Unexpected synthetic Win64 fixture function extents")
    bodies = [lifter.lift_function(code[start:draw], 0x180001000, name="guest_create_lifted"),
              lifter.lift_function(code[draw:], 0x180001000 + draw, name="guest_draw_lifted")]
    if any("RECOMP_TODO(" in body for body in bodies):
        raise ValueError("Synthetic fixture contains unimplemented x64 instruction")
    output = out / "guest_present_lifted.c"
    text = '#include "cpu64.h"\n' + "\n\n".join(bodies) + "\n"
    if not output.exists() or output.read_text() != text:
        output.write_text(text, encoding="utf-8")
    return {"object_sha256": sha(obj), "x64_text_sha256": sha(raw), "generated_c_sha256": sha(output),
            "functions": [{"name": "guest_create", "va": "0x180001000", "bytes": draw},
                          {"name": "guest_draw", "va": hex(0x180001000 + draw), "bytes": len(code) - draw}],
            "runtime_x64_execution": False, "runtime_translation": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--dxvk", action="store_true", help="Offline-lifted Win64 COM -> DXVK -> real Android Present")
    parser.add_argument("--native-present", action="store_true", help="With --dxvk: render960x540, present at actual Android window extent")
    parser.add_argument("--portrait-control", action="store_true", help="With --native-present: diagnostic control for Android transform hint")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.run and not args.serial:
        parser.error("--run requires --serial")
    if args.native_present and not args.dxvk:
        parser.error("--native-present requires --dxvk")
    if args.portrait_control and not args.native_present:
        parser.error("--portrait-control requires --native-present")
    global OUT
    if args.dxvk:
        OUT = ROOT / ("build/app-dxvk-native" if args.native_present else "build/app-dxvk")
        if args.portrait_control:
            OUT = ROOT / "build/app-dxvk-portrait"
    config = json.loads((ROOT / "port.json").read_text())
    sdk = Path(config["android_sdk"])
    ndk = sdk / "ndk" / config["android_ndk_version"]
    cmake = sdk / "cmake/3.22.1/bin/cmake.exe"
    build_tools = sdk / "build-tools/36.1.0"
    platform = sdk / "platforms/android-36/android.jar"
    java = Path("C:/Program Files/Android/Android Studio/jbr/bin")
    OUT.mkdir(parents=True, exist_ok=True)
    runtime = ROOT / "build/gpu-runtime/runtime"
    gpu_receipt = json.loads((ROOT / "research/gpu-runtime.json").read_text())
    libraries = ("libadrenotools.so", "libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so", "libgsl_alloc_hook.so")
    for name in (*libraries, "vulkan.ad07xx.so"):
        if sha(runtime / name) != gpu_receipt["runtime_sha256"][name]:
            raise ValueError("Independent native GPU runtime changed: " + name)
    paths = list(p for p in SOURCE.iterdir() if p.is_file()) + [Path(__file__).resolve(), ROOT / "android/gpu/driver.c", ROOT / "android/gpu/driver.h",
             ndk / "sources/android/native_app_glue/android_native_app_glue.c", ndk / "sources/android/native_app_glue/android_native_app_glue.h"]
    inputs = {str(p): sha(p) for p in paths}
    dxvk = None
    if args.dxvk:
        dependency_paths = [ROOT / p for p in ["android/dxvk/bridge.cpp", "android/dxvk/bridge.h", "native/runtime64/cpu64.h", "android/host/win64.h",
                            "tools/lift64_checked.py", "reference/pcrecomp/tools/lift/lift64_cpu.py", "reference/pcrecomp/tools/lift/lift32_cpu.py"]]
        inputs.update({str(p): sha(p) for p in dependency_paths})
        bridge = json.loads((ROOT / "build/dxvk-bridge/build-receipt.json").read_text())
        bridge_inputs=[ROOT/"build/dxvk-bridge"/name for name in ("build-receipt.json","libwin64_d3d11.a","bridge_metadata.h","bridge_dispatch.inc","bridge_slots.json")]
        inputs.update({str(p):sha(p) for p in bridge_inputs})
        for path, key in [("build/dxvk-bridge/libwin64_d3d11.a", "archive_sha256"), ("android/dxvk/bridge.cpp", "source_sha256"),
                          ("android/dxvk/bridge.h", "header_sha256"), ("native/runtime64/cpu64.h", "cpu_header_sha256"), ("android/host/win64.h", "win64_header_sha256")]:
            if sha(ROOT / path) != bridge[key]:
                raise ValueError("DXVK bridge receipt mismatch: " + path)
        dxvk = {"bridge_build": bridge, "fixture": generate_fixture(ndk, OUT), "libraries": {}}
        native_receipt = json.loads((ROOT / "build/dxvk-arm64/build-receipt.json").read_text())
        dxvk["native_build"] = native_receipt
        for item in native_receipt["outputs"]:
            path = ROOT / item["path"]
            if sha(path) != item["sha256"]:
                raise ValueError("Native DXVK library receipt mismatch: " + str(path))
            dxvk["libraries"][path.name] = {"path": str(path), "sha256": sha(path)}
        stdlib = ndk / "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
        dxvk["libraries"][stdlib.name] = {"path": str(stdlib), "sha256": sha(stdlib)}
    native = OUT / "native"
    configure = [cmake, "-S", SOURCE, "-B", native, "-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + str(cmake.parent / "ninja.exe"),
                 "-DCMAKE_TOOLCHAIN_FILE=" + str(ndk / "build/cmake/android.toolchain.cmake"), "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-30",
                 "-DANDROID_STL=" + ("c++_shared" if args.dxvk else "c++_static"), "-DCMAKE_BUILD_TYPE=Release", "-DGPU_RUNTIME=" + runtime.as_posix(),
                 "-DFP_DXVK_FIXTURE=" + ("ON" if args.dxvk else "OFF"), "-DFIXTURE_GENERATED=" + OUT.as_posix(), "-DDXVK_BRIDGE=" + (ROOT / "build/dxvk-bridge").as_posix()]
    configure.append("-DFP_NATIVE_PRESENT=" + ("ON" if args.native_present else "OFF"))
    configure.append("-DFP_GAME_RUNTIME=OFF")
    checked(configure)
    compilation = checked([cmake, "--build", native, "--parallel", "3"])
    (OUT / "compile.log").write_text(compilation + "\n", encoding="utf-8")
    classes = OUT / "classes"
    dex = OUT / "dex"
    classes.mkdir(exist_ok=True)
    dex.mkdir(exist_ok=True)
    checked([java / "javac.exe", "--release", "8", "-encoding", "UTF-8", "-classpath", platform, "-d", classes, SOURCE / "MainActivity.java", SOURCE / "TouchControls.java", SOURCE / "PortSupport.java", SOURCE / "PortProfile.java"])
    checked([java / "java.exe", "-cp", build_tools / "lib/d8.jar", "com.android.tools.r8.D8", "--min-api", "30", "--lib", platform, "--output", dex, *sorted(classes.rglob("*.class"))])
    assets = OUT / "assets"
    (assets / "turnip").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(runtime / "vulkan.ad07xx.so", assets / "turnip/vulkan.ad07xx.so")
    shutil.copytree(runtime / "licenses", assets / "licenses", dirs_exist_ok=True)
    # The early driver-query note described installed helper binaries. This APK
    # instead embeds our independently built, hash-verified upstream helpers.
    driver_origin = json.loads((ROOT / "reference/turnip-android/provenance.json").read_text())
    (assets / "licenses/Turnip-LICENSE-NOTE.txt").write_text(
        "This diagnostic embeds the unmodified community Android/Bionic Turnip asset:\n"
        + driver_origin["asset_url"] + "\nDriver SHA-256: " + gpu_receipt["runtime_sha256"]["vulkan.ad07xx.so"]
        + "\nMesa core is MIT-licensed; individual components can have other licenses.\n"
          "The downloaded ZIP contains no license file. Its full component license\n"
          "manifest has not been reconstructed: https://docs.mesa3d.org/license.html\n\n"
          "All native GPU helper libraries in this APK were independently built\n"
          "from the pinned BSD-2-Clause libadrenotools and liblinkernsbypass sources.\n"
          "No installed Winlator/Wine helper binary is packaged or used.\n"
        + "libadrenotools: " + gpu_receipt["adrenotools_revision"] + "\n"
        + "liblinkernsbypass: " + gpu_receipt["linkernsbypass_revision"] + "\n",
        encoding="utf-8")
    if args.dxvk:
        shutil.copyfile(ROOT / "reference/dxvk/LICENSE", assets / "licenses/DXVK-LICENSE.txt")
    base = OUT / "manifest.apk"
    manifest = SOURCE / "AndroidManifest.xml"
    if args.dxvk:
        manifest = OUT / "AndroidManifest.xml"
        manifest.write_text((SOURCE / "AndroidManifest.xml").read_text().replace("Frostpunk native window probe", "Frostpunk Win64 COM DXVK present probe"), encoding="utf-8")
        if args.portrait_control:
            text = manifest.read_text()
            updated = text.replace('android:screenOrientation="landscape"', 'android:screenOrientation="portrait"', 1)
            if updated == text:
                updated = text.replace('android:screenOrientation="sensorLandscape"', 'android:screenOrientation="portrait"', 1)
            manifest.write_text(updated, encoding="utf-8")
    compiled = OUT / "resources.zip"
    checked([build_tools / "aapt2.exe", "compile", "--dir", SOURCE / "res", "-o", compiled])
    checked([build_tools / "aapt2.exe", "link", "-I", platform, "--manifest", manifest, "-R", compiled, "-A", assets, "-o", base])
    unsigned = OUT / "probe-unsigned.apk"
    shutil.copyfile(base, unsigned)
    with zipfile.ZipFile(unsigned, "a") as package:
        package.write(dex / "classes.dex", "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
        package.write(native / "libfrostpunk_app.so", "lib/arm64-v8a/libfrostpunk_app.so", compress_type=zipfile.ZIP_STORED)
        for name in libraries:
            package.write(runtime / name, "lib/arm64-v8a/" + name, compress_type=zipfile.ZIP_STORED)
        if dxvk:
            for name, item in dxvk["libraries"].items():
                package.write(item["path"], "lib/arm64-v8a/" + name, compress_type=zipfile.ZIP_STORED)
    aligned = OUT / "probe-aligned.apk"
    checked([build_tools / "zipalign.exe", "-P", "16", "-f", "4", unsigned, aligned])
    keystore = ROOT / "build/app/probe-debug.keystore"
    keystore.parent.mkdir(parents=True, exist_ok=True)
    if not keystore.exists():
        checked([java / "keytool.exe", "-genkeypair", "-keystore", keystore, "-storepass", "android", "-keypass", "android", "-alias", "frostpunk-probe", "-dname", "CN=Frostpunk Native Probe", "-keyalg", "RSA", "-keysize", "2048", "-validity", "3650"])
    apk = OUT / "frostpunk-native-window-probe.apk"
    checked([java / "java.exe", "-jar", build_tools / "lib/apksigner.jar", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android", "--out", apk, aligned])
    signature = checked([java / "java.exe", "-jar", build_tools / "lib/apksigner.jar", "verify", "--verbose", "--print-certs", apk])
    checked([build_tools / "zipalign.exe", "-c", "-P", "16", "4", apk])
    for name, expected in inputs.items():
        if sha(Path(name)) != expected:
            raise RuntimeError("APK source changed during build: " + name)
    report = {"timestamp_utc": datetime.now(timezone.utc).isoformat(), "kind": "offline-lifted-win64-com-dxvk-present-fixture" if args.dxvk else "native-window-input-vulkan-fixture", "package": PACKAGE,
              "target": "aarch64-linux-android30", "target_sdk": 36, "sources": inputs, "configure": [str(x) for x in configure],
              "gpu_runtime_sha256": {name: gpu_receipt["runtime_sha256"][name] for name in (*libraries, "vulkan.ad07xx.so")},
              "apk": str(apk.relative_to(ROOT)), "apk_sha256": sha(apk), "apk_bytes": apk.stat().st_size, "signature_verification": signature,
              "app_library_sha256": sha(native / "libfrostpunk_app.so"), "game_assets_packaged": False, "original_game_loaded": False,
              "game_boot_proven": False, "internet_permission": False, "all_files_permission": False, "profile_app_private": True, "wine_box64": False}
    report["license_assets_sha256"] = {p.name: sha(p) for p in (assets / "licenses").iterdir() if p.is_file()}
    report["community_turnip_complete_license_manifest_verified"] = False
    report["orientation"] = "portrait-control" if args.portrait_control else "landscape"
    if dxvk:
        report["dxvk"] = dxvk
        report["presentation_policy"] = "dxgi960x540-native-android-present" if args.native_present else "forced-native960x540-diagnostic"
        report["factory2_setup"]="Explicit synthetic Win64 CPU frames in probe C++; unchanged offline-lifted guest_draw presents the real SwapChain1. No original game execution."
    if args.run:
        adb = [str(sdk / "platform-tools/adb.exe"), "-s", args.serial]
        checked(adb + ["install", "-r", apk])
        # A stale report from this same task's preceding fixture must not count.
        checked(adb + ["shell", "am", "force-stop", PACKAGE])
        subprocess.run(adb + ["shell", "run-as", PACKAGE, "rm", "-f", "files/probe-state.json"], check=False, capture_output=True)
        try:
            launch = checked(adb + ["shell", "am", "start", "-W", "-n", PACKAGE + "/.MainActivity"])

            def state():
                read = subprocess.run(adb + ["exec-out", "run-as", PACKAGE, "cat", "files/probe-state.json"], capture_output=True, timeout=5)
                try:
                    return json.loads(read.stdout)
                except (ValueError, UnicodeError):
                    return {}

            observed = {}
            for _ in range(40):
                observed = state()
                if observed.get("Vulkan13_BC") and observed.get("presented_frames", 0) > 2:
                    break
                time.sleep(0.25)
            if not observed.get("Vulkan13_BC") or observed.get("presented_frames", 0) <= 2:
                logs = checked(adb + ["exec-out", "run-as", PACKAGE, "cat", "files/native.log"])
                raise RuntimeError("Real window Vulkan fixture failed: " + json.dumps(observed) + "\n" + logs[-10000:])
            active = checked(adb + ["shell", "dumpsys", "activity", "activities"])
            if not any(PACKAGE in line and ("mResumedActivity" in line or "topResumedActivity" in line) for line in active.splitlines()):
                raise RuntimeError("Probe is not the resumed Activity; refusing to send input")
            before_touch = observed["touch_events"]
            before_keys = observed["key_events"]
            checked(adb + ["shell", "input", "tap", str(observed["physical_width"] // 2), str(observed["physical_height"] * 2 // 3)])
            checked(adb + ["shell", "input", "keyevent", "29"])
            for _ in range(20):
                observed = state()
                if observed.get("touch_events", 0) > before_touch and observed.get("key_events", 0) > before_keys:
                    break
                time.sleep(0.1)
            if observed.get("queue_overflow") or observed.get("touch_events", 0) <= before_touch or observed.get("key_events", 0) <= before_keys:
                raise RuntimeError("Actual Android input did not traverse the native queue: " + json.dumps(observed))
            if args.dxvk:
                data = observed.get("d3d11") or {}
                if not (data.get("factory2_setup_synthetic_CPU_frames") is True
                        and data.get("factory2_swapchain1_offline_draw") is True
                        and data.get("factory2_original_desc1") is True
                        and data.get("factory2_create_hresult")==0
                        and data.get("factory2_full_cpu_stack_canaries") is True
                        and data.get("factory2_native_held_lease_lifetime_pass") is True
                        and data.get("zero_size_descriptor_scope")=="legacy_regression"):
                    raise RuntimeError("Exact Factory2/SwapChain1 setup/inherited Present proof failed: "+json.dumps(observed))
                report["factory2_original_desc1_present_pass"]=True
                if not (data.get("explicit_guest_display") is True and data.get("zero_size_swapchain_descriptor") is True
                        and data.get("guest_display_width")==960 and data.get("guest_display_height")==540 and data.get("guest_refresh_hz")==60):
                    raise RuntimeError("Explicit logical-display/zero-size DXGI swapchain policy failed: "+json.dumps(observed))
                report["explicit_virtual_display_zero_size_swapchain_pass"] = True
                expected_width = observed.get("physical_width") if args.native_present else 960
                expected_height = observed.get("physical_height") if args.native_present else 540
                if not (observed.get("present_width") == expected_width and observed.get("present_height") == expected_height
                        and data.get("native_vk_presents", 0) > 0 and data.get("native_last_vk_present") in (0, 1000001003)
                        and data.get("create_hresult") == 0 and data.get("present_hresult") == 0
                        and all(data.get(key) is True for key in ("offline_lifted_x64_fixture", "getdesc_canary", "rsp_and_nonvolatile_canary", "stack_canary", "BC3_sample_supported"))):
                    raise RuntimeError("Real offline-lifted Win64 COM/DXVK Present failed: " + json.dumps(observed))
                if args.native_present:
                    compositor_rotation = data.get("surface_current_transform") != data.get("swapchain_pre_transform")
                    expected_status = 1000001003 if compositor_rotation else 0
                    stable = (data.get("dxgi_backbuffer_width") == 960 and data.get("dxgi_backbuffer_height") == 540
                              and data.get("native_last_vk_present") == expected_status
                              # Each start deliberately creates a legacy chain
                              # and a Factory2 chain, plus explicit lease-test
                              # replacements. One extra creation accommodates
                              # the native surface's initial lifecycle change.
                              and data.get("native_vk_swapchains_created", 100) <= 2 * observed.get("window_acquire_checks", 0) + 1 + data.get("lease_fixture_swapchains", 0))
                    if compositor_rotation:
                        stable = stable and bool(dxvk["native_build"].get("android_present_policy")) and data.get("native_suboptimal_presents", 0) > 0
                    if not stable:
                        raise RuntimeError("Native presentation still recreates swapchain repeatedly: " + json.dumps(observed))
                    report["native_presentation_stability_pass"] = True
                    report["android_compositor_rotation_path"] = compositor_rotation
                if not (data.get("strict_resolver_foreign_rejections", 0) >= 1 and data.get("native_held_lease_lifetime_pass") is True
                        and data.get("surface_acquires") == data.get("surface_releases", 0) + 1):
                    raise RuntimeError("Strict HWND/native surface lease proof failed: " + json.dumps(data))
                report["strict_hwnd_resolver_lifetime_pass"] = True
            screenshot = subprocess.run(adb + ["exec-out", "screencap", "-p"], capture_output=True, check=True, timeout=10).stdout
            (OUT / "fixture.png").write_bytes(screenshot)
            if args.dxvk:
                from PIL import Image
                image = Image.open(OUT / "fixture.png").convert("RGB")
                width, height = image.size
                checks = []
                corners = [("top_left", 64, 64, [255, 0, 0]), ("top_right", 896, 64, [0, 255, 0]),
                           ("bottom_left", 64, 476, [0, 0, 255]), ("bottom_right", 896, 476, [255, 255, 0])]
                for name, x, y, expected in corners:
                    point = (x * width // 960, y * height // 540)
                    actual = list(image.getpixel(point))
                    checks.append({"corner": name, "screenshot_xy": list(point), "expected_rgb": expected, "actual_rgb": actual,
                                   "match": all(abs(a - b) <= 5 for a, b in zip(actual, expected))})
                report["asymmetric_corner_screenshot"] = checks
                if not all(item["match"] for item in checks):
                    raise RuntimeError("Actual Android corner orientation mismatch: " + json.dumps(checks))
            report.update({"serial": args.serial, "activity_launch": launch, "device_fixture": observed, "screenshot": str((OUT / "fixture.png").relative_to(ROOT)), "screenshot_sha256": sha(OUT / "fixture.png"), "native_window_input_present_pass": True})
        except Exception as error:
            report.update(device_pass=False, failure=str(error))
            try:
                report["device_fixture"] = state()
                (OUT / "failed-native.log").write_text(checked(adb + ["exec-out", "run-as", PACKAGE, "cat", "files/native.log"]), encoding="utf-8")
            except Exception:
                pass
            prefix = "android-dxvk-portrait-control" if args.portrait_control else "android-dxvk-native-present-probe" if args.native_present else "android-dxvk-present-probe" if args.dxvk else "android-window-probe"
            (ROOT / ("research/" + prefix + ".json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            raise
        finally:
            checked(adb + ["shell", "am", "force-stop", PACKAGE])
            report["stopped_only_own_probe_after_test"] = True
            if report.get("failure"):
                (ROOT / ("research/" + prefix + ".json")).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    prefix = "android-dxvk-native-present-probe" if args.native_present else "android-dxvk-present-probe" if args.dxvk else "android-window-probe"
    if args.portrait_control:
        prefix = "android-dxvk-portrait-control"
    receipt = ROOT / ("research/" + prefix + (".json" if args.run else "-build.json"))
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"build_pass": True, "device_pass": args.run, "apk": str(apk), "apk_sha256": report["apk_sha256"], "receipt": str(receipt)}, indent=2))


if __name__ == "__main__":
    main()
