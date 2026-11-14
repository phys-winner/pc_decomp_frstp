"""Project-owned x64 lifter corrections; upstream pcrecomp stays unchanged.

CLI accepts the same arguments as pcrecomp generate64.py.
"""
from pathlib import Path
from functools import partial
import hashlib
import json
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "reference/pcrecomp/tools/lift"))
from lift64_cpu import Lifter
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RSP

ROOT = Path(__file__).resolve().parents[1]
ORIGINAL_SHA256 = "45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc"
BIND_RANGES = ((0x144506310, 0x60), (0x1445063D0, 0x35A8))
BND_BRANCHES = {"call","ret","jmp","ja","jae","jb","jbe","je","jne","jg","jge","jl","jle","jo","jno","jp","jnp","js","jns"}


class UnprefixedBranch:
    def __init__(self, instruction):
        self.instruction=instruction
        self.mnemonic=instruction.mnemonic[4:]

    def __getattr__(self,name):
        return getattr(self.instruction,name)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class CheckedLifter(Lifter):
    def __init__(self, *args, original_sha256=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.verified_frostpunk_wrapper = original_sha256 == ORIGINAL_SHA256
        disassemble=self.md.disasm
        def without_mpx(*args,**kwargs):
            for instruction in disassemble(*args,**kwargs):
                # MPX is disabled in our guest CPUID contract; F2 preserves normal branch semantics.
                yield UnprefixedBranch(instruction) if instruction.mnemonic.startswith("bnd ") and instruction.mnemonic[4:] in BND_BRANCHES else instruction
        self.md.disasm=without_mpx

    def translate(self, instruction, labels):
        if instruction.mnemonic == "lock or":
            destination, source = instruction.operands
            if (destination.type == X86_OP_MEM and destination.mem.base == X86_REG_RSP
                    and source.type == X86_OP_IMM and source.imm == 0):
                # MSVC's stack RMW is a full inter-thread fence, including on ARM64.
                return ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"] + super().translate(instruction, labels)
        if self.verified_frostpunk_wrapper:
            if (instruction.address == BIND_RANGES[0][0] and instruction.mnemonic == "call"
                    and instruction.operands[0].type == X86_OP_IMM
                    and instruction.operands[0].imm == instruction.address + instruction.size):
                # This exact get-PC site consumes its pushed RIP as data.
                return [f"push64(c, 0x{instruction.address + instruction.size:X}ull);"]
            if instruction.address == 0x14450636F and instruction.mnemonic == "ret":
                # The supplied wrapper replaces this return slot with the OEP.
                return ["{ uint64_t target = pop64(c); dispatch_jmp(c, target); return; }"]
        if instruction.mnemonic == "bswap" and instruction.operands[0].size in (4, 8):
            operand = instruction.operands[0]
            value = f"frostpunk_bswap{operand.size * 8}({self.src(instruction, operand)})"
            return [self.dst_write(instruction, operand, value)]
        return super().translate(instruction, labels)

    def sse(self, instruction):
        if instruction.mnemonic in ("addss", "mulss"):
            destination, source = instruction.operands
            target = self._ss(instruction, destination)
            return [f"{target} = frostpunk_{instruction.mnemonic}({target}, {self._ss(instruction, source)});"]
        return super().sse(instruction)


def self_test():
    plain = CheckedLifter(image_size=0x4539000, image_base=0x140000000)
    pinned = CheckedLifter(image_size=0x4539000, image_base=0x140000000,
                           original_sha256=ORIGINAL_SHA256)
    site = BIND_RANGES[0][0]
    # A generic CALL-next;RET must still pop two slots through normal dispatch.
    assert "dispatch(c, 0x144506315ull)" in plain.lift_function(bytes.fromhex("e800000000c3"), site)
    out = pinned.lift_function(bytes.fromhex("e80000000058c3"), site)
    assert "push64(c, 0x144506315ull);" in out and "dispatch(c," not in out
    assert "dispatch(c, 0x144506415ull)" in pinned.lift_function(bytes.fromhex("e800000000c3"), site + 0x100)
    assert "dispatch_jmp(c, target)" in pinned.lift_function(b"\xc3", 0x14450636F)
    assert "dispatch_jmp(c, target)" not in plain.lift_function(b"\xc3", 0x14450636F)
    assert "dispatch_jmp(c, target)" not in pinned.lift_function(b"\xc3", 0x14450636E)
    branch=plain.lift_function(bytes.fromhex("83f900f27506b801000000c3b802000000c3"),0x140001000)
    assert "RECOMP_TODO" not in branch and "goto L_00014000100C" in branch and "L_00014000100C:" in branch
    assert "RECOMP_TODO" not in plain.lift_function(bytes.fromhex("f2c3"),0x140001000)
    fence=plain.lift_function(bytes.fromhex("f0830c2400c3"),0x140001000)
    assert fence.count("__atomic_thread_fence(__ATOMIC_SEQ_CST)")==1 and "flags_logicz" in fence


def main():
    import generate64
    self_test()
    arguments = sys.argv[1:]
    if "--split" in arguments:
        index = arguments.index("--split")
        del arguments[index:index + 2]
    if len(arguments) < 3:
        raise SystemExit("Usage: lift64_checked.py EXE CATALOG OUTPUT [--split COUNT]")
    exe, catalog, output = map(Path, arguments[:3])
    if digest(exe) != ORIGINAL_SHA256:
        raise ValueError("Original executable changed; rederive the exact wrapper rules")
    generator_paths = [Path(__file__).resolve(), ROOT / "tools/reference64.py",
                       ROOT / "reference/pcrecomp/tools/lift/generate64.py",
                       ROOT / "reference/pcrecomp/tools/lift/lift64_cpu.py",
                       ROOT / "reference/pcrecomp/tools/lift/lift32_cpu.py"]
    generator_sources = {str(path.relative_to(ROOT)): digest(path) for path in generator_paths}
    catalog_sha256 = digest(catalog)
    output.mkdir(parents=True, exist_ok=True)
    meta_path = output / "recomp_meta.json"
    meta_path.unlink(missing_ok=True)
    generate64.Lifter = partial(CheckedLifter, original_sha256=ORIGINAL_SHA256)
    generate64.main()
    metadata = json.loads(meta_path.read_text())
    # Upstream writes a completion marker before project provenance is attached.
    meta_path.unlink()
    if digest(exe) != ORIGINAL_SHA256 or digest(catalog) != catalog_sha256:
        raise ValueError("Original executable or catalog changed during generation")
    if any(digest(ROOT / name) != expected for name, expected in generator_sources.items()):
        raise ValueError("Generator sources changed during generation")
    expected = {f"recomp_funcs_{index:04d}.c" for index in range(metadata["translation_units"])}
    for path in output.glob("recomp_funcs_*.c"):
        if path.name not in expected:
            path.unlink()
    files = sorted(output.glob("recomp_*.c")) + sorted(output.glob("recomp_*.h"))
    metadata.update({
        "original_exe_sha256": ORIGINAL_SHA256,
        "catalog": str(catalog.relative_to(ROOT)), "catalog_sha256": catalog_sha256,
        "generator_sources": generator_sources,
        "generated_files": {path.name: digest(path) for path in files},
        "original_entry_preserved": True, "catalog_complete": False,
        "game_boot_proven": False, "no_runtime_cpu_translation": True,
        "wrapper_rules": [hex(BIND_RANGES[0][0]), "0x14450636f"],
    })
    meta_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print("[*] Checked source/catalog provenance sealed", flush=True)


if __name__ == "__main__":
    main()
