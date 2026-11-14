"""Differentially test an actual Frostpunk x64 SSE leaf on Windows and Android.

The oracle executes the unchanged original bytes natively on Windows. The same
generated C and oracle fixtures are then compiled for MSVC x64 and NDK ARM64.
This tests one leaf; it does not start the game or measure gameplay performance.
"""
import argparse
import ctypes
import hashlib
import itertools
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

import reference64

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/leaf"
VA = 0x1417B70F0
RAW = bytes.fromhex("f30f5809f30f5949040f28c1c3")


def prepare_runtime():
    source = ROOT / "reference/pcrecomp/runtime/recomp64_cpu"
    target = ROOT / "native/runtime64"
    target.mkdir(parents=True, exist_ok=True)
    text = (source / "cpu64.h").read_text()
    start = text.index("#if defined(_MSC_VER)\nstatic inline uint64_t rd_gs64")
    end = text.index("/* ---- atomics", start)
    original = text[start:end]
    gs = """#if defined(__aarch64__)
/* Android host must supply its actual per-thread guest TEB storage. */
uint64_t frostpunk_teb_read(uint64_t off, unsigned bytes);
void frostpunk_teb_write(uint64_t off, uint64_t value, unsigned bytes);
static inline uint64_t rd_gs64(uint64_t off) { return frostpunk_teb_read(off, 8); }
static inline uint32_t rd_gs32(uint64_t off) { return (uint32_t)frostpunk_teb_read(off, 4); }
static inline void wr_gs64(uint64_t off, uint64_t v) { frostpunk_teb_write(off, v, 8); }
static inline void wr_gs32(uint64_t off, uint32_t v) { frostpunk_teb_write(off, v, 4); }
#else
"""
    text = text[:start] + gs + original + "#endif\n\n" + text[end:]
    old = "static inline void do_cpuid(CPU *c)\n{\n    int r[4]"
    new = """void frostpunk_cpuid(CPU *c);
static inline void do_cpuid(CPU *c)
{
#if defined(__aarch64__)
    /* No invented x86 features: the Android host must choose its CPU contract. */
    frostpunk_cpuid(c);
#else
    int r[4]"""
    if text.count(old) != 1:
        raise ValueError("Unexpected upstream CPUID definition")
    text = text.replace(old, new)
    old = "    SET32(c->rdx, (uint32_t)r[3]);\n}"
    if text.count(old) != 1:
        raise ValueError("Unexpected upstream CPUID ending")
    text = text.replace(old, "    SET32(c->rdx, (uint32_t)r[3]);\n#endif\n}")
    text = text.replace("#define RECOMP_TODO(va, text) abort()",
                        "void frostpunk_unsupported(uint64_t va, const char *text);\n"
                        "#define RECOMP_TODO(va, text) frostpunk_unsupported((va), (text))")
    text = text.replace('#include "eh64.h"', '#include "eh64.h"\n#include "sse_scalar.h"\n#include "bswap.h"')
    (target / "cpu64.h").write_text(text, encoding="utf-8", newline="\n")
    shutil.copyfile(source / "eh64.h", target / "eh64.h")
    eh = (source / "eh64.c").read_text().replace("static __declspec(thread)", "static _Thread_local")
    marker = "static _Thread_local es3_ehframe_t *eh_top;"
    if eh.count(marker) != 1:
        raise ValueError("Unexpected upstream thread-local EH frame declaration")
    eh = eh.replace(marker, marker + "\n\n/* A native thread-boundary longjmp has discarded all frames on this thread. */\n"
                    "void es3_eh_thread_reset(void) { eh_top = NULL; }")
    (target / "eh64.c").write_text(eh, encoding="utf-8", newline="\n")
    shutil.copyfile(ROOT / "reference/pcrecomp/LICENSE", target / "LICENSE")
    (target / "provenance.json").write_text(json.dumps({
        "upstream_revision": reference64.REVISION, "license": "MIT",
        "changes": ["ARM64 TEB and CPUID explicit host callbacks; no fabricated defaults",
                    "Portable C11 thread-local EH frame head",
                    "Explicit per-thread EH reset after ExitThread boundary longjmp",
                    "Unsupported instructions require an explicit diagnostic host trap",
                    "Local ADDSS/MULSS helpers preserve x86 NaN propagation and indefinite result",
                    "Portable BSWAP32/64 helpers; CheckedLifter retains width-specific register writes"],
        "upstream_sha256": {p.name: reference64.digest(p) for p in source.iterdir() if p.is_file()},
    }, indent=2) + "\n", encoding="utf-8")


def oracle_cases():
    if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise RuntimeError("The original-byte oracle requires native x64 Windows")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.VirtualAlloc.restype = ctypes.c_void_p
    kernel.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
    kernel.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.FlushInstructionCache.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
    page = kernel.VirtualAlloc(None, 4096, 0x3000, 0x40)
    if not page:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        ctypes.memmove(page, RAW, len(RAW))
        # Win64 wrapper calls the unchanged original, then returns XMM0 bits in
        # EAX so Python never converts or canonicalizes the binary32 result.
        wrapper = (bytes.fromhex("4883ec2848b8") + struct.pack("<Q", page) +
                   bytes.fromhex("ffd0660f7ec04883c428c3"))
        ctypes.memmove(page + 256, wrapper, len(wrapper))
        if not kernel.FlushInstructionCache(kernel.GetCurrentProcess(), page, 4096):
            raise ctypes.WinError(ctypes.get_last_error())
        function = ctypes.CFUNCTYPE(ctypes.c_uint32, ctypes.POINTER(ctypes.c_float), ctypes.c_float)(page + 256)
        edge = [0, 0x80000000, 1, 0x80000001, 0x007FFFFF, 0x807FFFFF,
                0x00800000, 0x80800000, 0x3F000000, 0xBF000000, 0x3F800000,
                0xBF800000, 0x40000000, 0xC0000000, 0x7F7FFFFF, 0xFF7FFFFF,
                0x7F800000, 0xFF800000, 0x7FC12345, 0xFFC23456, 0x7F812345, 0xFF812345]
        inputs = list(itertools.product(edge, repeat=3))
        rng = random.Random(0xF2057)
        for _ in range(8192):
            row = []
            while len(row) < 3:
                bits = rng.getrandbits(32)
                if bits & 0x7F800000 != 0x7F800000:
                    row.append(bits)
            inputs.append(tuple(row))
        cases = []
        for a, b, x in inputs:
            memory = (ctypes.c_uint32 * 2)(a, b)
            x_arg = ctypes.c_float.from_buffer_copy(struct.pack("<I", x))
            result = function(ctypes.cast(memory, ctypes.POINTER(ctypes.c_float)), x_arg)
            cases.append((a, b, x, result))
        return cases
    finally:
        if not kernel.VirtualFree(page, 0, 0x8000):
            raise ctypes.WinError(ctypes.get_last_error())


def run(args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{args[0]} failed ({result.returncode})\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result.stdout.strip()


def windows_toolchain():
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    vs = Path(run([vswhere, "-latest", "-products", "*", "-property", "installationPath"]))
    msvc = sorted((vs / "VC/Tools/MSVC").iterdir())[-1]
    sdk = Path(r"C:\Program Files (x86)\Windows Kits\10")
    version = sorted((sdk / "Lib").iterdir())[-1].name
    env = os.environ.copy()
    env["INCLUDE"] = ";".join(str(p) for p in [msvc / "include", sdk / "Include" / version / "ucrt",
                                               sdk / "Include" / version / "shared", sdk / "Include" / version / "um"])
    env["LIB"] = ";".join(str(p) for p in [msvc / "lib/x64", sdk / "Lib" / version / "ucrt/x64",
                                           sdk / "Lib" / version / "um/x64"])
    return msvc / "bin/Hostx64/x64", env


def windows_build():
    binaries, env = windows_toolchain()
    exe = OUT / "frostpunk-leaf-x64.exe"
    run([binaries / "cl.exe", "/nologo", "/O2", "/fp:strict", "/std:c11", "/W3",
         "/I" + str(ROOT / "native/runtime64"), "/I" + str(OUT), ROOT / "native/leaf_probe.c",
         OUT / "leaf_lifted.c", "/Fe" + str(exe)], env=env, cwd=OUT)
    return run([exe])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--android", action="store_true")
    parser.add_argument("--serial")
    args = parser.parse_args()
    if args.android and not args.serial:
        parser.error("--android requires --serial")
    config = json.loads((ROOT / "port.json").read_text())
    exe = Path(config["game_directory"]) / "Frostpunk.exe"
    if reference64.digest(exe) != config["original_exe_sha256"]:
        raise ValueError("Original Frostpunk hash changed")
    reference64.prepare_reference()
    prepare_runtime()
    sys.path.insert(0, str(ROOT / "reference/pcrecomp/tools/lift"))
    import generate64
    data, base, image_size, _, sections, *_ = generate64.read_pe(exe)
    read = generate64.make_reader(data, base, sections)
    if read(VA, len(RAW)) != RAW:
        raise ValueError("Frostpunk leaf bytes differ from the audited candidate")
    from lift64_checked import CheckedLifter
    lifter = CheckedLifter(image_size=image_size, read_va=read, image_base=base)
    lifted = lifter.lift_function(RAW, VA, name="frostpunk_leaf")
    if "RECOMP_TODO" in lifted or "dispatch(" in lifted:
        raise ValueError("Leaf no longer lifts without unsupported paths")
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "leaf_lifted.c").write_text('#include "cpu64.h"\n' + lifted + "\n", encoding="utf-8")
    cases = oracle_cases()
    header = "struct leaf_case { unsigned a, b, x, result; };\nstatic const struct leaf_case leaf_cases[] = {\n"
    header += "".join("    {" + ", ".join(f"0x{v:08X}u" for v in row) + "},\n" for row in cases) + "};\n"
    (OUT / "leaf_cases.h").write_text(header, encoding="utf-8")
    report = {"original_exe_sha256": config["original_exe_sha256"], "leaf_va": hex(VA),
              "leaf_bytes": RAW.hex(), "signature": "float leaf(const float params[2], float x)",
              "contract": "Binary32 (x + params[0]) * params[1], separate roundings; finite, infinities, NaN inputs",
              "oracle": "Unmodified original x64 bytes executed natively on Windows through Win64 ABI",
              "cases": len(cases), "fixture_sha256": reference64.digest(OUT / "leaf_cases.h"),
              "windows": windows_build(), "android": None, "game_boot_proven": False,
              "limits": ["Default nearest rounding; MXCSR flags, other rounding modes, DAZ/FTZ not tested",
                         "Only this 13-byte leaf; startup, renderer, gameplay and FPS not tested"]}
    print(report["windows"])
    if args.android:
        sdk = Path(config["android_sdk"])
        clang = sdk / "ndk" / config["android_ndk_version"] / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
        binary = OUT / "frostpunk-leaf-arm64"
        run([clang, "--target=aarch64-linux-android30", "-O2", "-std=c11", "-ffp-contract=off",
             "-fno-strict-aliasing", "-fwrapv", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-sign-compare",
             "-I" + str(ROOT / "native/runtime64"), "-I" + str(OUT), ROOT / "native/leaf_probe.c",
             OUT / "leaf_lifted.c", "-lm", "-Wl,-z,max-page-size=16384", "-o", binary])
        adb = sdk / "platform-tools/adb.exe"
        remote = "/data/local/tmp/frostpunk/leaf_probe"
        run([adb, "-s", args.serial, "shell", "mkdir", "-p", "/data/local/tmp/frostpunk"])
        run([adb, "-s", args.serial, "push", binary, remote])
        run([adb, "-s", args.serial, "shell", "chmod", "700", remote])
        output = run([adb, "-s", args.serial, "shell", remote])
        if output != report["windows"]:
            raise RuntimeError(f"Device output differs from Windows: {output}")
        report["android"] = {"serial": args.serial, "result": output, "binary_sha256": reference64.digest(binary)}
        print(output)
    report_path = ROOT / "research/leaf-probe.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")


if __name__ == "__main__":
    main()
