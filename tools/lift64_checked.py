"""Project-owned x64 lifter corrections; upstream pcrecomp stays unchanged.

CLI accepts the same arguments as pcrecomp generate64.py.
"""
from pathlib import Path
from functools import partial
import hashlib
import json
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "reference/pcrecomp/tools/lift"))
from lift64_cpu import Lifter, PACKED_CMP_RE, SSE_CMP_PRED
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_RSP

# x86 read-modify-writes that are atomic full barriers with a `lock` prefix (cpu64.h lock_<op><W>).
LOCKED_UNARY = ("inc", "dec", "neg", "not")
LOCKED_BINARY = ("add", "sub", "or", "and", "xor")


def locked_form(instruction):
    """'rmw' for `lock <op> [mem](, src)`, 'xchg' for `(lock) xchg` with one memory operand, 'fence' for
    MSVC's `lock or dword ptr [rsp], 0`; None for anything else (including lock cmpxchg/xadd, which the
    shared lifter already lowers to atomic_* helpers)."""
    mnemonic, operands = instruction.mnemonic, instruction.operands
    if mnemonic.startswith("lock ") and operands and operands[0].type == X86_OP_MEM \
            and operands[0].mem.segment == 0 and operands[0].size in (1, 2, 4, 8):
        operation = mnemonic[5:]
        if operation == "or" and operands[0].mem.base == X86_REG_RSP and operands[0].mem.index == 0 \
                and operands[0].mem.disp == 0 and operands[1].type == X86_OP_IMM and operands[1].imm == 0:
            return "fence"
        if operation in LOCKED_UNARY and len(operands) == 1:
            return "rmw"
        if operation in LOCKED_BINARY and len(operands) == 2:
            return "rmw"
    if mnemonic in ("xchg", "lock xchg") and len(operands) == 2 \
            and sum(op.type == X86_OP_MEM for op in operands) == 1 \
            and all(op.type in (X86_OP_MEM, X86_OP_REG) for op in operands):
        memory = operands[0] if operands[0].type == X86_OP_MEM else operands[1]
        if memory.mem.segment == 0 and memory.size in (1, 2, 4, 8):
            return "xchg"
    return None

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

    def _locked_rmw(self, instruction):
        """`lock op [mem](, src)` as one atomic helper call that returns the old value.

        The shared lifter drops `lock` and emits a plain load/op/store, which loses updates between
        threads.  The flags are computed from the old value exactly as the plain form computed them
        from its load.  `lock or [rsp], reg` (an MSVC fence with a register) takes this path too: the
        helper is a full barrier."""
        destination = instruction.operands[0]
        operation = instruction.mnemonic[5:]
        size = destination.size
        width = size * 8
        kind = "uint%d_t" % width
        address = self.addr_expr(instruction, destination)
        if operation in ("inc", "dec"):
            helper, flags = ("lock_add%d", "flags_incs") if operation == "inc" else ("lock_sub%d", "flags_decs")
            return ["{ %s _old = %s(%s, (%s)1u); (void)%s(c, _old, %d); }" % (kind, helper % width, address, kind, flags, size)]
        if operation == "not":
            return ["(void)lock_not%d(%s);" % (width, address)]
        if operation == "neg":
            return ["{ %s _old = lock_neg%d(%s); (void)flags_sub(c, 0, _old, %d); }" % (kind, width, address, size)]
        source = self.src_ext(instruction, instruction.operands[1], size)
        if operation in ("add", "sub"):
            return ["{ %s _v = (%s)(%s); %s _old = lock_%s%d(%s, _v); (void)flags_%s(c, _old, _v, %d); }"
                    % (kind, kind, source, kind, operation, width, address, operation, size)]
        symbol = {"or": "|", "and": "&", "xor": "^"}[operation]
        return ["{ %s _v = (%s)(%s); %s _old = lock_%s%d(%s, _v); (void)flags_logicz(c, (%s)(_old %s _v), %d); }"
                % (kind, kind, source, kind, operation, width, address, kind, symbol, size)]

    def _locked_xchg(self, instruction):
        """`xchg` with a memory operand is implicitly locked and a full barrier on x86."""
        first, second = instruction.operands
        memory, register = (first, second) if first.type == X86_OP_MEM else (second, first)
        width = memory.size * 8
        kind = "uint%d_t" % width
        address = self.addr_expr(instruction, memory)
        value = self.src(instruction, register)
        return ["{ %s _old = lock_xchg%d(%s, (%s)(%s)); %s }"
                % (kind, width, address, kind, value, self.dst_write(instruction, register, "_old"))]

    def locked(self, instruction):
        """The atomic lowering of a lock/xchg read-modify-write, or None (tools/atomic_sites.py uses it
        to prove and rewrite already generated trees)."""
        form = locked_form(instruction)
        if form == "fence":
            # MSVC's stack RMW is a full inter-thread fence, including on ARM64.
            return ["__atomic_thread_fence(__ATOMIC_SEQ_CST);"] + super().translate(instruction, set())
        if form == "rmw":
            return self._locked_rmw(instruction)
        if form == "xchg":
            return self._locked_xchg(instruction)
        return None

    def translate(self, instruction, labels):
        locked = self.locked(instruction)
        if locked is not None:
            return locked
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

    def _packed_cmp(self, instruction):
        """cmpps/cmppd: a mask per lane (4 floats or 2 doubles).

        lift64's sse() tests its scalar SSE_CMP_RE (`cmp<pred>(ss|sd|ps|pd)`) before PACKED_CMP_RE, so
        it lowered every packed compare as a float compare of lane 0 (cmppd on the low half of the
        double) and lanes 1-3 kept the destination's bits.  This is lift64's own packed branch, which
        that order made unreachable.  Trees lifted before are repaired by the AOT/Windows overlays
        (fp_aot_packed_compare, tools/generate_windows_packed_compare_overlay.py)."""
        match = PACKED_CMP_RE.match(instruction.mnemonic)
        predicate, width = match.group(1), match.group(2)
        destination, source = instruction.operands[0], instruction.operands[1]
        wide = width == "pd"
        field, ufield, lanes = ("f64", "u64", 2) if wide else ("f32", "u32", 4)
        ctype = "double" if wide else "float"
        ones = "~(uint64_t)0" if wide else "0xFFFFFFFFu"
        zero = "(uint64_t)0" if wide else "0u"
        n = self._xi(destination)
        body = ["{ %s _a = c->xmm[%d].%s[%d], _b = _s.%s[%d]; c->xmm[%d].%s[%d] = %s ? %s : %s; }"
                % (ctype, n, field, i, field, i, n, ufield, i, SSE_CMP_PRED[predicate], ones, zero) for i in range(lanes)]
        return ["{ XMM _s = %s; %s }" % (self._xm(instruction, source), " ".join(body))]

    def sse(self, instruction):
        if PACKED_CMP_RE.match(instruction.mnemonic):
            return self._packed_cmp(instruction)
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
    assert fence.count("__atomic_thread_fence(__ATOMIC_SEQ_CST)")==1 and "flags_logicz" in fence and "lock_or" not in fence
    # Locked read-modify-writes and xchg with memory are atomic helpers, not a plain load/op/store.
    locked = {
        "f0ff00c3": ("lock_add32((c->rax), (uint32_t)1u)", "flags_incs(c, _old, 4)"),          # lock inc dword ptr [rax]
        "f048ff00c3": ("lock_add64(", "flags_incs(c, _old, 8)"),                                # lock inc qword ptr [rax]
        "f0ff4908c3": ("lock_sub32((c->rcx + 0x8ull), (uint32_t)1u)", "flags_decs(c, _old, 4)"),  # lock dec dword ptr [rcx + 8]
        "f0fe0bc3": ("lock_sub8(", "flags_decs(c, _old, 1)"),                                   # lock dec byte ptr [rbx]
        "f066ff03c3": ("lock_add16(", "flags_incs(c, _old, 2)"),                                # lock inc word ptr [rbx]
        "f0834108ffc3": ("lock_add32(", "0xFFFFFFFFu", "flags_add(c, _old, _v, 4)"),           # lock add dword ptr [rcx + 8], -1
        "f04c2908c3": ("lock_sub64(", "flags_sub(c, _old, _v, 8)"),                            # lock sub qword ptr [rax], r9
        "f081483c00010000c3": ("lock_or32(", "flags_logicz(c, (uint32_t)(_old | _v), 4)"),     # lock or dword ptr [rax + 0x3c], 0x100
        "f0802001c3": ("lock_and8(", "flags_logicz(c, (uint8_t)(_old & _v), 1)"),              # lock and byte ptr [rax], 1
        "f03118c3": ("lock_xor32(", "flags_logicz(c, (uint32_t)(_old ^ _v), 4)"),              # lock xor dword ptr [rax], ebx
        "f0f718c3": ("lock_neg32(", "flags_sub(c, 0, _old, 4)"),                               # lock neg dword ptr [rax]
        "f048f710c3": ("lock_not64(",),                                                        # lock not qword ptr [rax]
        "f0090c24c3": ("lock_or32((c->rsp), _v)", "R32(c->rcx)"),                             # lock or dword ptr [rsp], ecx
        "8601c3": ("lock_xchg8((c->rcx), (uint8_t)(", "SET8L(c->rax, _old)"),                 # xchg byte ptr [rcx], al
        "8621c3": ("lock_xchg8(", "SET8H(c->rax, _old)"),                                      # xchg byte ptr [rcx], ah
        "668711c3": ("lock_xchg16(", "SET16(c->rdx, _old)"),                                   # xchg word ptr [rcx], dx
        "8783b0040000c3": ("lock_xchg32((c->rbx + 0x4B0ull)", "SET32(c->rax, _old)"),         # xchg dword ptr [rbx + 0x4b0], eax
        "488711c3": ("lock_xchg64((c->rcx), (uint64_t)(c->rdx))", "SET64(c->rdx, _old)"),     # xchg qword ptr [rcx], rdx
        "f0488711c3": ("lock_xchg64(",),                                                       # lock xchg qword ptr [rcx], rdx
    }
    for code, needles in locked.items():
        body = plain.lift_function(bytes.fromhex(code), 0x140001000)
        for needle in needles:
            assert needle in body, (code, needle, body)
        assert "RECOMP_TODO" not in body and "uint64_t _t = rd" not in body, (code, body)
        assert not any(("%s(c, rd" % name) in body for name in ("flags_incs", "flags_decs", "flags_add", "flags_sub")), (code, body)
        assert "wr8(" not in body and "wr16(" not in body and "wr32(" not in body and "wr64(" not in body, (code, body)
    # xchg between two registers stays a register move; lock cmpxchg/xadd keep their atomic_* lowering.
    assert "lock_xchg" not in plain.lift_function(bytes.fromhex("4887cac3"), 0x140001000)
    assert "atomic_cmpxchg32(" in plain.lift_function(bytes.fromhex("f00fb111c3"), 0x140001000)
    assert "atomic_xadd32(" in plain.lift_function(bytes.fromhex("f00fc101c3"), 0x140001000)
    # Packed compares write every lane (lift64 lowered them as a float compare of lane 0).
    packed = {
        "0fc2cc02c3": ("XMM _s = c->xmm[4];", "c->xmm[1].u32[3] = (_a <= _b)", "float _a = c->xmm[1].f32[3]"),  # cmpleps xmm1, xmm4
        "660fc2c501c3": ("c->xmm[0].u64[1] = (_a < _b)", "double _a = c->xmm[0].f64[1]"),                        # cmpltpd xmm0, xmm5
        "0fc20100c3": ("XMM _s = rdxm((c->rcx))", "c->xmm[0].u32[2] = (_a == _b)"),                               # cmpeqps xmm0, [rcx]
    }
    for code, needles in packed.items():
        body = plain.lift_function(bytes.fromhex(code), 0x140001000)
        for needle in needles:
            assert needle in body, (code, needle, body)
    # Scalar compares keep lane 0 only.
    scalar = plain.lift_function(bytes.fromhex("f30fc2cc02c3"), 0x140001000)   # cmpless xmm1, xmm4
    assert "c->xmm[1].u32[0] = (_a <= _b)" in scalar and "u32[1]" not in scalar, scalar


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
