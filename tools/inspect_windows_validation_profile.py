"""Identify sampled syscalls and dispatch call sites without changing the game."""
import ctypes
import hashlib
import json
from pathlib import Path
import re

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

ROOT = Path(__file__).resolve().parents[1]
PROFILE = ROOT / "build/windows/profiles/20261022T025528895069Z.json"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    profile = json.loads(PROFILE.read_text())
    executable = ROOT / "build/windows/Frostpunk.exe"
    map_path = ROOT / "build/windows/frostpunk-windows-x64.map"
    assert sha(executable) == profile["binary_sha256"] == "8bafc8e313ec98ce7c835a62264f66f011e8f60b31423f785043a9213f302ca3"
    pe = pefile.PE(str(executable), fast_load=True)
    pe.parse_data_directories(directories=[1])
    imports = {entry.address: entry.name.decode() for dll in pe.DIRECTORY_ENTRY_IMPORT for entry in dll.imports if entry.name}
    symbols = {name: int(address, 16) for name, address in re.findall(
        r"^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\b", map_path.read_text(), re.M)}
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    addresses = sorted(set(symbols.values()))

    def instructions(name):
        start = symbols[name]
        end = next(a for a in addresses if a > start)
        return list(decoder.disasm(pe.get_data(start - pe.OPTIONAL_HEADER.ImageBase, end - start), start))

    def record(ins):
        result = dict(address=hex(ins.address), bytes=ins.bytes.hex(), mnemonic=ins.mnemonic, operands=ins.op_str)
        if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_MEM and ins.operands[0].mem.base == X86_REG_RIP:
            slot = ins.address + ins.size + ins.operands[0].mem.disp
            result["import"] = imports.get(slot)
        return result

    dispatch = instructions("dispatch")
    target_valid = instructions("fp_native_target_valid")
    static_call = next(i for i in dispatch if i.address + i.size - symbols["dispatch"] == 0x28f)
    validation_call = next(i for i in dispatch if i.mnemonic == "call" and i.operands[0].type == X86_OP_IMM and
                           i.operands[0].imm == symbols["fp_native_target_valid"])
    native_call = next(i for i in dispatch if i.mnemonic == "call" and i.operands[0].type == X86_OP_IMM and
                       i.operands[0].imm == symbols["fp_native_call"])
    assert static_call.mnemonic == "call" and static_call.op_str == "r14"
    assert native_call.address + native_call.size - symbols["dispatch"] == 0x60d
    sample_offset = symbols["fp_native_target_valid"] + 0x1d
    preceding = next(i for i in target_valid if i.address + i.size == sample_offset)
    assert preceding.mnemonic == "lock cmpxchg"
    vq_call = next(i for i in target_valid if record(i).get("import") == "VirtualQuery")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    kernel.GetProcAddress.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel.GetProcAddress.restype = ctypes.c_void_p
    system = {}
    for filename, names in [("ntdll.dll", ["NtWaitForSingleObject", "NtQueryVirtualMemory"]),
                            ("kernelbase.dll", ["WaitForSingleObjectEx"])]:
        path = Path("C:/Windows/System32") / filename
        dll = pefile.PE(str(path), fast_load=True)
        dll.parse_data_directories(directories=[0])
        exports = {s.name.decode(): s.address for s in dll.DIRECTORY_ENTRY_EXPORT.symbols if s.name}
        module = kernel.GetModuleHandleW(filename)
        entries = []
        for name in names:
            rva = exports[name]
            loaded = kernel.GetProcAddress(module, name.encode())
            assert loaded - module == rva
            expected = dll.get_data(rva, 32)
            assert ctypes.string_at(loaded, len(expected)) == expected
            decoded = list(decoder.disasm(expected, rva))
            entries.append(dict(name=name, export_rva=hex(rva), loaded_address=hex(loaded), first_bytes=expected.hex(),
                                instructions=[record(i) for i in decoded]))
        system[filename] = dict(path=str(path), sha256=sha(path), loaded_current_process_base=hex(module), exports=entries)
    assert int(system["ntdll.dll"]["loaded_current_process_base"], 16) + 0x160404 in {
        int(pc, 16) for thread in profile["threads"] for pc in thread["raw_rip"]}
    host_path = ROOT / "native/windows/host.c"
    validation_path = ROOT / "native/windows/native_validation.c"
    report = dict(profile=PROFILE.relative_to(ROOT).as_posix(), profile_sha256=sha(PROFILE), pid=profile["pid"],
                  binary_sha256=sha(executable), map_sha256=sha(map_path), producer_sha256=sha(Path(__file__)),
                  source_sha256={path.relative_to(ROOT).as_posix(): sha(path) for path in (host_path, validation_path)},
                  system_dlls=system, hot_thread=profile["threads"][0],
                  dispatch_static_call=dict(instruction=record(static_call), return_offset="0x28f", role="selected static C fn(c) call"),
                  dispatch_validation_call=dict(instruction=record(validation_call), return_offset=hex(validation_call.address + validation_call.size - symbols["dispatch"])),
                  dispatch_native_bridge_call=dict(instruction=record(native_call), return_offset="0x60d"),
                  target_validation_sample=dict(offset="0x1d", immediately_preceding=record(preceding),
                                                role="epoch read, before cache check and VirtualQuery", VirtualQuery_call=record(vq_call)),
                  findings=["ntdll+160404 is the post-syscall RET of NtWaitForSingleObject, not NtQueryVirtualMemory",
                            "KERNELBASE+1c11f is inside WaitForSingleObjectEx; sampled native bridges can be genuine game/native DLL waits",
                            "NtQueryVirtualMemory export1607d0 returns1607e4; no current hot-thread location is1607e4",
                            "dispatch+28f is a static C call return; raw stack words may include several ancestor copies and are not query counts",
                            "fp_native_target_valid+1d is immediately after locked epoch read; its2samples do not show a cache miss or VirtualQuery",
                            "The only production fp_native_target_valid caller stops on a false result, so repeated negative validation cannot accumulate without STOP",
                            "Valid non-pinned executable private/writable-image targets, cache collisions or epoch changes can query repeatedly; this profile has no exact miss counters"],
                  production_modified=False, game_process_modified=False,
                  limits=["20 wall-time samples per thread, not CPU-cycle-weighted profiling or a full unwind",
                          "Existing profile has no full stacks or native call target arguments, so no exact wait handle or per-target query frequency can be inferred",
                          "Current own-process system DLL export bytes were checked against files; the target game process was not suspended or read by this inspection"])
    output = ROOT / "research/windows-validation-profile-analysis.json"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(report=str(output), sha256=sha(output), findings=report["findings"]), indent=2))


if __name__ == "__main__":
    main()
