"""fastlift: re-emit the lifted Frostpunk units as whole functions with register-promotable state.

The canonical lift (generate64.py) keeps every guest register and flag in `CPU *c` memory, splits each
function at .pdata fragment boundaries and routes every transfer through a dispatcher.  The compiler
therefore reloads and stores the whole guest state around almost every instruction.

This tool rewrites the *already generated and field-proven* statements, one per instruction, into:

  * one C function per original function (boundaries from the IDA database), so fragment-to-fragment
    jumps become plain gotos;
  * flags in a local struct (dead flags vanish; flags that are live across a direct jump between
    functions are passed through `c` exactly as before);
  * `CPU *restrict c` + noescape callees, so guest memory stores no longer force the compiler to
    reload guest registers;
  * direct calls/tail calls for statically known targets outside the dispatcher's special list;
  * no `c->rip` bookkeeping stores;
  * volatile guest memory accesses: the original code re-reads shared memory on purpose (spin waits,
    lock-free lists), so every access stays exactly where the instruction had it;
  * try-functions translated like any other: the Android runtime cannot deliver a guest C++ throw
    (nothing links es3_eh_throw), so their setjmp landing pads were unreachable;
  * indirect calls through the dispatcher's lookup cache inline, import-table calls through the
    handler the dispatcher published for that import.

Every original fragment entry keeps its symbol (the dispatch table is unchanged): a fragment that is
not a function start gets a stand-alone copy, so an indirect transfer to it still works.  Units the
production build patches are taken from their patched source; wrapped originals keep their alias.

    python tools/fastlift.py stats
    python tools/fastlift.py host bootstrap win64          # the two runtime sources this needs
    python tools/fastlift.py build --jobs 15 --icall       # generate + compile all units
    python tools/fastlift.py link --name NAME --drop bootstrap --drop win64- \
        --add build/fastlift/host/bootstrap.o --add build/fastlift/host/win64.o
    python tools/test_fastlift_flags.py --serial SERIAL    # helper differential test on a device

Diagnostics: --flags all (every flag through `c` at every boundary), --no-merge, --no-lt,
--keep-eh, --keep-special, --units A-B.
"""
import argparse
import bisect
import collections
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GENERATED = ROOT / 'generated/frostpunk-x64'
OVERLAY = ROOT / 'build/arm64/generated-overlay'
OUT = ROOT / 'build/fastlift'
# 2026-11-21: lock/xchg-mem statements of the generated trees are rewritten to cpu64.h's atomic
# helpers in unit_text (tools/atomic_sites.py); FASTLIFT_ATOMICS=0 reproduces the earlier plain lowering.
ATOMIC_REWRITE = os.environ.get('FASTLIFT_ATOMICS', '1') != '0'
BASE_LINK = ROOT / 'build/perf/minimum60-20261113/visibility-loop-restrict/production/link.rsp'
BASE_APK = ROOT / 'build/perf/minimum60-20261113/visibility-loop-restrict/production/Frostpunk-visibility-loop.apk'
MAIN_BASE, MAIN_END = 0x140000000, 0x144539000
AUDIO_BEGIN, AUDIO_END = 0x141babd20, 0x141bc2d40
# Targets the dispatcher resolves itself (adapters, replacements, ring fences, WinMain note).
SPECIAL = {0x140e222e0, 0x140d95650, 0x140f499a0, 0x1410596a9, 0x1410597e9, 0x141250610, 0x14124c7b0,
           0x140ec6200, 0x140ec7240, 0x140e74b30, 0x140e26e40, 0x140e26f30, 0x140e26f90, 0x140f99f00,
           0x140f9d240, 0x140e02420, 0x141c5c490, 0x141c54e60, 0x141c54af0, 0x141c551d0}


# Units whose production source carries a functional patch (resolution policy, Switch graphics
# transaction, particle gate, detail threshold, depth hierarchy, port settings rows).
SOURCE_OVERRIDE = {
    807: 'build/fast/mobile-objects/recomp_funcs_0807.c',
    809: 'build/perf/switch-20261113/candidate/0809/recomp_funcs_0809.c',
    820: 'build/perf/switch-20261113/candidate/0820/recomp_funcs_0820.c',
    836: 'build/fast/depth-hierarchy/recomp_funcs_0836.c',
    959: 'build/perf/switch-20261113/candidate/0959/recomp_funcs_0959.c',
}
# Original functions that a host object wraps under their own symbol (objcopy --redefine-sym in the
# production build): the translated body keeps the alias, the wrapper owns L_<address>.
ALIASES = {va: 'fp_original_L_%012X' % va for va in (
    0x140D8DB10, 0x140D8E470, 0x140DFA630, 0x140E1E6C0, 0x140E26E40, 0x140E26F30, 0x140E26F90, 0x140E74B30,
    0x140EC6200, 0x140EC7240, 0x140F99F00, 0x140F9D240, 0x14124C7B0, 0x141250610, 0x1419EF190, 0x141A04EB0,
    0x141A5EE10, 0x141A62E90, 0x141AAAFB0, 0x14180A400,
    # UICanvas::Render (line charts of the Economy panel): mobile_controls.c corrects its scale.
    0x1410CCA60)}


ALIASES.update({0x140F752A0: 'fp_quality_original_L_000140F752A0', 0x1410343F0: 'fp_quality_original_L_0001410343F0',
                0x140E2C200: 'fp_bounds_original', 0x140E2C890: 'fp_frustum_original'})
# MeshEntity's global level of detail (meshentity.cpp): wrapped by android/host/mobile_lod.c.
ALIASES[0x140FBA350] = 'fp_lod_original_L_000140FBA350'
# Wrappers that must also see the references of their own unit.  SociotechTree's camera update
# (141A62E90) is only reached by tail jumps from 141A60220 (cancel) and 141A60530 (selection) in its
# own unit. The building painter also calls its aim routine directly in its own unit.
ALIAS_WRAP_OWN_UNIT = {0x141A62E90, 0x141AAAFB0, 0x14180A400}


# Production link rows of patched units that do not carry the unit number in their file name.
UNIT_ROWS = {812: 'build/perf/minimum60-20261113/walk-inline/candidate.o',
             822: 'build/perf/minimum60-20261113/visibility-loop-restrict/candidate.o',
             804: 'build/perf/minimum60-20261113/bounds-native/production-fallback.o'}


# Earlier per-unit flag pruning (optimize_live_flags64.py): the same helper with a liveness mask.
LIVE_FLAGS = re.compile(r'\bfp_flags_(add|sub|logicz)\(c, (.*?), \d+u\)')
DROP_PROLOGUE = {'#include "mobile_detail.h"', 'unsigned fp_test_detail_mode(CPU*);',
                 'float fp_test_detail_threshold(float,unsigned);','/* generated by generate64.py - do not edit */', '#include "cpu64.h"', '#include "recomp_funcs.h"',
                 '#include "aot_dispatch.h"', '#pragma once',
                 '/* Only exact producer-verified constant main VA/symbol pairs may pass linked. */',
                 '__attribute__((visibility("hidden"))) void fp_dispatch_main(CPU *c,uint64_t target,void (*linked)(CPU *));'}


# Software prefetch at the head of loops that walk an array of object pointers and miss the cache
# on every object (profile of the developed city: 50-80% of the samples of these functions sit on
# the first load from the object).  The statement runs in front of the instruction at the key
# address.  It reads the pointer `distance` elements ahead only while that element is inside the
# array (index + distance < count, both as the loop itself keeps them) and touches nothing else.
PREFETCH = {
    # EntityRenderer visibility query: rbx = element, r15 = index, r12 = count.
    # 2026-11-22 (Adreno 650, native Vulkan): the reject path is short (about 8 ns per element), eight
    # elements ahead were still missing (half of the function's samples on the +0x248/+0x170 loads):
    # sixteen ahead.  A parent prefetch needs a real load of the element's +0x40, which then missed
    # itself (a fifth of the function's samples), so the parent is left to the hardware.
    0x140F8F650: 'if (c->r15 + 16 < c->r12) { uint64_t fl_p = rd64(c->rbx + 128); FL_PREFETCH(fl_p + 0x248); '
                 'FL_PREFETCH(fl_p + 0x40); FL_PREFETCH(fl_p + 0x140); }',
    # Entity walk before the frame: rbx = element, rbp = index, r14 = count.
    0x140EB4642: 'if (c->rbp + 8 < c->r14) { uint64_t fl_p = rd64(c->rbx + 64); FL_PREFETCH(fl_p + 0x40); '
                 'FL_PREFETCH(fl_p); FL_PREFETCH(fl_p + 0xAC0); FL_PREFETCH(fl_p + 0x180); }',
}

FLAGS = ('cf', 'zf', 'sf', 'of', 'pf', 'af')
CF, ZF, SF, OF, PF, AF = (1 << i for i in range(6))
ALL = 63
FLAG_BIT = {name: 1 << i for i, name in enumerate(FLAGS)}
TAIL = re.compile(r'\s*/\* ([0-9A-F]{12}): (.*) \*/\s*$')
FUNC = re.compile(r'^void L_([0-9A-F]{12})\(CPU \*c\)$')
LABEL = re.compile(r'^L_([0-9A-F]{12}):$')
RIP = re.compile(r'^c->rip = 0x[0-9A-F]+ull;$')
CASE = re.compile(r'^case 0x([0-9A-F]+)ull: goto L_([0-9A-F]{12});$')
HEXV = r'0x([0-9A-Fa-f]+)ull'
DISP = r'(?:dispatch\(c, ' + HEXV + r'\)|fp_dispatch_main\(c, ' + HEXV + r', L_[0-9A-F]{12}\))'
LEAVE = r'(ES3_EH_LEAVE\(\); )?'
FALL = re.compile(r'^' + DISP + r'; ' + LEAVE + r'return;$')
P_RET = re.compile(r'^c->rsp \+= (\d+); ' + LEAVE + r'return;$')
P_CALL = re.compile(r'^push64\(c, ' + HEXV + r'\); ' + DISP + r'; c->rip = ' + HEXV + r';$')
P_ICALL = re.compile(r'^\{ uint64_t _ct = (.*); push64\(c, ' + HEXV + r'\); dispatch\(c, _ct\); \} c->rip = ' + HEXV + r';$')
P_JMP = FALL
P_JCC = re.compile(r'^if \((.*)\) \{ ' + DISP + r'; ' + LEAVE + r'return; \}$')
P_GOTO = re.compile(r'^goto L_([0-9A-F]{12});$')
P_GOTOCC = re.compile(r'^if \((.*)\) goto L_([0-9A-F]{12});$')
P_IJMP = re.compile(r'^\{ _ind = (.*); goto _ljump; \}$')
P_LOOP_IN = re.compile(r'^c->rcx--; if \((.*)\) goto L_([0-9A-F]{12});$')
P_LOOP_OUT = re.compile(r'^c->rcx--; if \((.*)\) \{ ' + DISP + r'; ' + LEAVE + r'return; \}$')
P_RETF = re.compile(r'^/\* retf: far frame unwound by the caller \*/ ' + LEAVE + r'return;$')
P_POPJMP = re.compile(r'^\{ uint64_t target = pop64\(c\); dispatch_jmp\(c, (.*)\); return; \}$')
CONTROL_WORDS = re.compile(r'\b(return|goto|dispatch|dispatch_jmp|fp_dispatch_main|_ljump|ES3_EH_LEAVE|longjmp|setjmp)\b')
OLD_DISPATCH = re.compile(r'\bdispatch\(c,\s*(0x([0-9A-Fa-f]+)ull)\)')


def first(*groups):
    for g in groups:
        if g is not None:
            return g
    return None


def eligible(va):
    return MAIN_BASE <= va < MAIN_END and not AUDIO_BEGIN <= va <= AUDIO_END and va not in SPECIAL


class Insn:
    __slots__ = ('addr', 'lines', 'asm', 'kind', 'target', 'cond', 'ret', 'expr', 'leave', 'use', 'must', 'may', 'k', 'ltgen')

    def __init__(self, addr, asm):
        self.addr, self.asm, self.lines = addr, asm, []
        self.kind, self.target, self.cond, self.ret, self.expr, self.leave = 'plain', None, None, None, None, False
        self.use = self.must = self.may = 0
        self.k = 8
        self.ltgen = None

    @property
    def code(self):
        return '\n'.join(self.lines)

    @property
    def terminator(self):
        return self.kind in ('ret', 'jmp', 'goto', 'ijmp', 'retf', 'popjmp')


class Chunk:
    __slots__ = ('va', 'unit', 'insns', 'labels', 'eh', 'ljump', 'cases', 'fall', 'foreign', 'text', 'reason')

    def __init__(self, va, unit):
        self.va, self.unit = va, unit
        self.insns, self.labels, self.cases = [], set(), []
        self.eh = self.ljump = self.foreign = False
        self.fall = None
        self.text = ''
        self.reason = ''


def classify(insn):
    """Recognise the control-transfer shape of one instruction; anything else is a plain statement."""
    if len(insn.lines) != 1:
        return not CONTROL_WORDS.search(insn.code)
    s = insn.lines[0]
    m = P_RET.match(s)
    if m:
        insn.kind, insn.k, insn.leave = 'ret', int(m[1]), bool(m[2]); return True
    m = P_CALL.match(s)
    if m:
        ret, t, back = int(m[1], 16), int(first(m[2], m[3]), 16), int(m[4], 16)
        if ret != back:
            return False
        insn.kind, insn.target, insn.ret = 'call', t, ret; return True
    m = P_ICALL.match(s)
    if m:
        if int(m[2], 16) != int(m[3], 16):
            return False
        insn.kind, insn.expr, insn.ret = 'icall', m[1], int(m[2], 16); return True
    m = P_JMP.match(s)
    if m:
        insn.kind, insn.target, insn.leave = 'jmp', int(first(m[1], m[2]), 16), bool(m[3]); return True
    m = P_JCC.match(s)
    if m:
        insn.kind, insn.cond, insn.target, insn.leave = 'jcc', m[1], int(first(m[2], m[3]), 16), bool(m[4]); return True
    m = P_GOTO.match(s)
    if m:
        insn.kind, insn.target = 'goto', int(m[1], 16); return True
    m = P_GOTOCC.match(s)
    if m:
        insn.kind, insn.cond, insn.target = 'gotocc', m[1], int(m[2], 16); return True
    m = P_IJMP.match(s)
    if m:
        insn.kind, insn.expr = 'ijmp', m[1]; return True
    m = P_LOOP_IN.match(s)
    if m:
        insn.kind, insn.cond, insn.target = 'loop_in', m[1], int(m[2], 16); return True
    m = P_LOOP_OUT.match(s)
    if m:
        insn.kind, insn.cond, insn.target, insn.leave = 'loop_out', m[1], int(first(m[2], m[3]), 16), bool(m[4]); return True
    m = P_RETF.match(s)
    if m:
        insn.kind, insn.leave = 'retf', bool(m[1]); return True
    if P_POPJMP.match(s):
        insn.kind = 'popjmp'; return True
    return not CONTROL_WORDS.search(s)


def parse_unit(text, unit):
    """Split one generated unit into fragment functions.  Returns (prologue_lines, [Chunk])."""
    lines = text.replace('\r\n', '\n').split('\n')
    chunks, prologue = [], []
    i, n = 0, len(lines)
    while i < n:
        m = FUNC.match(lines[i])
        if not m:
            if lines[i].strip():
                prologue.append(lines[i])
            i += 1
            continue
        chunk = Chunk(int(m[1], 16), unit)
        start = i
        if lines[i + 1] != '{':
            raise ValueError('unit %04d: no body for %s' % (unit, lines[i]))
        i += 2
        current = None
        pending = []
        state = 'body'
        while lines[i] != '}':
            raw = lines[i]
            s = raw.strip()
            i += 1
            t = TAIL.search(raw)
            if t and state == 'body':
                addr, code = int(t[1], 16), LIVE_FLAGS.sub(r'flags_\1(c, \2)', raw[:t.start()].strip())
                if current is None or current.addr != addr:
                    current = Insn(addr, t[2])
                    chunk.insns.append(current)
                    current.lines.extend(pending)
                    pending = []
                current.lines.append(code)
                continue
            if not s:
                continue
            if state == 'switch':
                c = CASE.match(s)
                if c:
                    if int(c[1], 16) != int(c[2], 16):
                        chunk.foreign, chunk.reason = True, 'case mismatch'
                    chunk.cases.append(int(c[1], 16)); continue
                if s in ('default: dispatch_jmp(c, _ind); ES3_EH_LEAVE(); return;', 'default: dispatch_jmp(c, _ind); return;'):
                    continue
                if s == '}':
                    state = 'done'; continue
                chunk.foreign, chunk.reason = True, 'switch line: ' + s[:60]; continue
            current = None
            if RIP.match(s):
                continue
            l = LABEL.match(s)
            if l:
                if pending:  # a patch statement between an instruction and a jump target: keep the text
                    chunk.foreign, chunk.reason = True, 'patch line before label: ' + pending[0][:60]
                    pending = []
                chunk.labels.add(int(l[1], 16)); continue
            if s == 'uint64_t _ind = 0;':
                chunk.ljump = True; continue
            if s.startswith('ES3_EH_ENTER('):
                chunk.eh = True; continue
            if s == '_ljump:':
                continue
            if s == 'switch (_ind) {':
                state = 'switch'; continue
            if s == '/* extent ends mid-function: fall through */':
                continue
            f = FALL.match(s)
            if f and state == 'body':
                chunk.fall = int(first(f[1], f[2]), 16); continue
            if state == 'body' and not CONTROL_WORDS.search(s) and not s.endswith(':') and '{' not in s and '}' not in s:
                pending.append(s)  # a patch statement in front of the next instruction
                continue
            chunk.foreign, chunk.reason = True, 'line: ' + s[:80]
        if pending:
            chunk.foreign, chunk.reason = True, 'trailing patch line: ' + pending[0][:60]
        chunk.text = '\n'.join(lines[start:i + 1])
        i += 1
        if not chunk.insns:
            if chunk.fall is None or chunk.eh:
                chunk.foreign, chunk.reason = True, chunk.reason or 'empty'
            else:  # a fragment that only continues at the next one
                jump = Insn(chunk.va, '(empty fragment)')
                jump.kind, jump.target = 'jmp', chunk.fall
                jump.lines.append('')
                chunk.insns.append(jump)
                chunk.fall = None
        for insn in chunk.insns:
            if insn.asm != '(empty fragment)' and not classify(insn):
                chunk.foreign, chunk.reason = True, 'insn %012X: %s' % (insn.addr, insn.code[:80])
                break
        chunks.append(chunk)
    return prologue, chunks


# ---------------------------------------------------------------------------------------------
# Flag effects of one generated statement.
HELPER_DEF = {
    'flags_logicz': CF | OF | SF | ZF | PF, 'flags_add': ALL, 'flags_sub': ALL, 'flags_adc': ALL, 'flags_sbb': ALL,
    'flags_incs': ZF | SF | OF | AF | PF, 'flags_decs': ZF | SF | OF | AF | PF,
    'op_bt': CF, 'op_bts': CF, 'op_btr': CF, 'op_btc': CF, 'op_bsf': ALL, 'op_bsr': ALL,
    'bitscan_undef_flags': CF | OF | SF | AF | PF, 'sse_compare': ALL, 'eflags_unpack': ALL,
}
HELPER_USE = {'flags_adc': CF, 'flags_sbb': CF, 'eflags_pack': ALL}
# Count-dependent helpers: flags change only for a non-zero (masked) count.
HELPER_SHIFT = {'op_shl': CF | OF | ZF | SF | PF, 'op_shr': CF | OF | ZF | SF | PF, 'op_sar': CF | OF | ZF | SF | PF,
                'op_rol': CF | OF, 'op_ror': CF | OF, 'op_shld': CF | ZF | SF | PF, 'op_shrd': CF | ZF | SF | PF}


FLAG_HELPERS = sorted(set(HELPER_DEF) | set(HELPER_USE) | set(HELPER_SHIFT))
LT_HELPERS = ('flags_sub', 'flags_logicz', 'flags_decs')
HELPER_CALL = re.compile(r'\b(' + '|'.join(FLAG_HELPERS) + r')\(c\b')
FLAG_TOKEN = re.compile(r'c->(cf|zf|sf|of|pf|af)\b')
ASSIGN_AFTER = re.compile(r'\s*=(?!=)')
CONDITIONAL = re.compile(r'\bif\s*\(|\belse\b|\bwhile\s*\(|\?')
WRITE_PREFIX = re.compile(r'[\s{]*(?:c->(?:cf|zf|sf|of|pf|af)\s*=\s*)*')
NUMBER = re.compile(r'^\(?\s*(0x[0-9A-Fa-f]+|\d+)(?:ull|u)?\s*\)?$')


def call_args(text, start):
    """Arguments of the call whose '(' is at text[start]."""
    depth, args, begin = 0, [], start + 1
    for j in range(start, len(text)):
        ch = text[j]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                args.append(text[begin:j].strip())
                return args
        elif ch == ',' and depth == 1:
            args.append(text[begin:j].strip()); begin = j + 1
    raise ValueError('unbalanced call: ' + text[start:start + 60])


def shift_definite(name, args):
    """True when the helper certainly rewrites its flags (constant count that survives masking)."""
    count, size = args[-2], args[-1]
    m = NUMBER.match(count)
    if not m or not size.isdigit():
        return False
    n, sz = int(m[1], 0), int(size)
    n &= 63 if sz == 8 else 31
    if name in ('op_rol', 'op_ror'):
        n %= sz * 8
        return n != 0
    if name in ('op_shld', 'op_shrd'):
        return 0 < n < sz * 8
    return n != 0


def flag_effect(insn):
    """Fill use / must-define / may-define bit sets.  Uncertain writes are never counted as kills."""
    code = insn.code
    use = must = may = 0
    if insn.cond:
        for m in FLAG_TOKEN.finditer(insn.cond):
            use |= FLAG_BIT[m[1]]
        if insn.kind in ('jcc', 'gotocc', 'loop_in', 'loop_out'):
            insn.use, insn.must, insn.may = use, 0, 0
            return
    helpers = 0
    direct_write = False
    for m in FLAG_TOKEN.finditer(code):
        bit = FLAG_BIT[m[1]]
        if ASSIGN_AFTER.match(code, m.end()):
            may |= bit
            direct_write = True
            statement = code[:m.start()].split(';')[-1]
            if not CONDITIONAL.search(code[:m.start()]) and WRITE_PREFIX.fullmatch(statement):
                must |= bit
        else:
            use |= bit
    lt = None
    for m in HELPER_CALL.finditer(code):
        name = m[1]
        helpers += 1
        use |= HELPER_USE.get(name, 0)
        conditional = bool(CONDITIONAL.search(code[:m.start()]))
        if name in HELPER_DEF:
            may |= HELPER_DEF[name]
            if not conditional:
                must |= HELPER_DEF[name]
                if name in LT_HELPERS:
                    lt = name
        elif name in HELPER_SHIFT:
            may |= HELPER_SHIFT[name]
            if not conditional and shift_definite(name, call_args(code, m.end() - 2)):
                must |= HELPER_SHIFT[name]
    # bsf/bsr: both arms of the generated if/else define every flag.
    if ('op_bsf(c' in code or 'op_bsr(c' in code) and 'else { c->zf = 1; bitscan_undef_flags(c); }' in code:
        must = may = ALL
    insn.use, insn.must, insn.may = use, must, may
    insn.ltgen = lt if (lt and helpers == 1 and not direct_write) else None


# ---------------------------------------------------------------------------------------------
# Global plan (cheap, picklable): every fragment start, its unit and kind; IDA functions; switches.
def unit_path(index):
    name = 'recomp_funcs_%04d.c' % index
    if index in SOURCE_OVERRIDE:
        return ROOT / SOURCE_OVERRIDE[index]
    overlay = OVERLAY / name
    return overlay if overlay.exists() else GENERATED / name


OUTPUT_LABEL_CALL = '    fp_output_labels(c,c->rdx);\n'
FRAME_CALL = '    fp_frame_options(c,c->rdx);\n'
FRAME_FN = 'static void fp_frame_options(CPU *cpu,uint64_t row) {'
GETTER_OPEN = 'void L_000141A17C60(CPU *c)\n{\n'
OUTPUT_LABEL_FN = '''static int fp_label_matches(uint64_t text, const char *want) {
    const unsigned char *bytes;
    size_t i;
    if (!text || !want) return 0;
    /* Option slots may hold a guest string object. Read only the label length. */
    bytes = (const unsigned char *)(uintptr_t)text;
    for (i = 0; want[i]; ++i) {
        if (i >= 47 || bytes[i] != (unsigned char)want[i]) return 0;
    }
    return bytes[i] == 0;
}
static void fp_output_labels(CPU *cpu, uint64_t row) {
    /* The closed combo reads strings installed by the option append (the same path as "60 FPS").
     * Asset strings are left allocated: the append overwrites the slot and must see a null there. */
    static int done, waits;
    if (done || !row || rd32(row + 0x20) != 21u) return;
    uint64_t options = rd64(row + 0x28);
    uint32_t count = rd32(row + 0x30);
    if (!options || !count || count > 16) return;
    char want[16][48];
    uint32_t values[16];
    unsigned same = 1;
    for (uint32_t i = 0; i < count; ++i) {
        values[i] = rd32(options + (uint64_t)i * 16);
        uint64_t text = rd64(options + (uint64_t)i * 16 + 8);
        if (!fp_output_resolution_label(values[i], want[i], sizeof want[i])) {
            if (waits < 4) fprintf(stderr, "FP_OUTPUT_LABEL wait i=%u value=%u\\n", i, values[i]);
            ++waits;
            return;
        }
        if (!fp_label_matches(text, want[i])) same = 0;
    }
    if (same) { done = 1; return; }
    CPU c = *cpu;
    c.rsp = (c.rsp - 512) & ~UINT64_C(15);
    for (uint32_t i = 0; i < count; ++i) wr64(options + (uint64_t)i * 16 + 8, 0);
    wr32(row + 0x30, 0);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t string = c.rsp + 0x60, chars = c.rsp + 0x80;
        wr64(string, 0);
        snprintf((char *)(uintptr_t)chars, sizeof want[i], "%s", want[i]);
        c.rcx = string;
        c.rdx = chars;
        push64(&c, UINT64_C(0xe5ee0010));
        dispatch(&c, UINT64_C(0x140E538C0));
        if (!rd64(string)) {
            fprintf(stderr, "FP_OUTPUT_LABEL create_failed i=%u\\n", i);
            return;
        }
        c.rcx = row;
        c.rdx = values[i];
        c.r8 = string;
        push64(&c, UINT64_C(0xe5ee0010));
        dispatch(&c, UINT64_C(0x141A14E20));
    }
    done = rd32(row + 0x30) == count;
    fprintf(stderr, "FP_OUTPUT_LABEL wrote count=%u first=%s\\n", rd32(row + 0x30), want[0]);
}

'''


def port_settings_patch(text):
    """Keep pixel output-resolution labels when unit 959 is regenerated.

    The closed combo shows strings installed by the option append. A raw pointer
    write does not: "60 FPS" is visible only because frame options use that append.
    """
    if FRAME_FN not in text:
        return text
    text = text.replace('uint32_t fp_port_frame_rates(uint32_t rates[32]);\n',
                        '#include "port_settings.h"\n', 1)
    text = text.replace('uint32_t rates[32],count=fp_port_frame_rates(rates);',
                        'uint32_t rates[FP_MAX_FRAME_LIMITS],count=fp_port_frame_rates(rates);', 1)
    text = text.replace('if(!options||!previous||previous>33)return;',
                        'if(!options||!previous||previous>FP_MAX_FRAME_LIMITS+1)return;', 1)
    if 'static void fp_output_labels' not in text:
        if text.count(FRAME_FN) != 1:
            raise ValueError('port settings frame helper drifted')
        text = text.replace(FRAME_FN, OUTPUT_LABEL_FN + FRAME_FN, 1)
    if '#include "port_settings.h"' not in text:
        raise ValueError('port settings declaration drifted')
    if 'fp_output_labels(c,c->rdx)' not in text:
        if text.count(FRAME_CALL) == 1:
            text = text.replace(FRAME_CALL, FRAME_CALL + OUTPUT_LABEL_CALL, 1)
        elif text.count(GETTER_OPEN) == 1:
            text = text.replace(GETTER_OPEN, GETTER_OPEN + FRAME_CALL + OUTPUT_LABEL_CALL, 1)
        else:
            raise ValueError('port settings getter drifted')
    # Row ids 31 and 32 are model detail and building quality. The guest compares
    # the setter index one below the row id, so 31 covers building quality.
    text = text.replace(
        'if(R32(c->rcx)>=21u&&R32(c->rcx)<=30u)',
        'if(R32(c->rcx)>=21u&&R32(c->rcx)<=32u)', 1)
    text = text.replace(
        'if(R32(c->rbx)>=20u&&R32(c->rbx)<=29u)',
        'if(R32(c->rbx)>=20u&&R32(c->rbx)<=31u)', 1)
    # Row 33 is the free monument (profile bool, getter/setter in mobile_controls.c); the setter's
    # effects hook also raises the original "requires restart" flag for row 30 (Switch interface).
    if 'fp_port_setting_effects' not in text:
        steps = (
            ('uint32_t fp_port_setting_get(uint32_t id);\n',
             'uint32_t fp_port_setting_get(uint32_t id);\n'
             'uint32_t fp_port_monument_get(CPU *c);\n'
             'void fp_port_setting_effects(CPU *c,uint32_t id,uint32_t value);\n'),
            ('if(R32(c->rcx)>=21u&&R32(c->rcx)<=32u) {\n        SET32(c->rdx,fp_port_setting_get(R32(c->rcx)));',
             'if(R32(c->rcx)>=21u&&R32(c->rcx)<=33u) {\n'
             '        SET32(c->rdx,R32(c->rcx)==33u?fp_port_monument_get(c):fp_port_setting_get(R32(c->rcx)));'),
            ('if(R32(c->rbx)>=20u&&R32(c->rbx)<=31u) {', 'if(R32(c->rbx)>=20u&&R32(c->rbx)<=32u) {'),
            ('        fp_port_setting_set(R32(c->rbx)+1u,rd32(c->rsp+0x50u));\n',
             '        fp_port_setting_set(R32(c->rbx)+1u,rd32(c->rsp+0x50u));\n'
             '        fp_port_setting_effects(c,R32(c->rbx)+1u,rd32(c->rsp+0x50u));\n'))
        for before, after in steps:
            if text.count(before) != 1:
                raise ValueError('port settings row 33 anchor drifted: ' + before[:60])
            text = text.replace(before, after, 1)
    return text


def unit_text(index):
    """Source text of a unit, with the functional patches that are applied as text overlays."""
    text = unit_path(index).read_text(encoding='utf-8').replace(chr(13) + chr(10), chr(10))
    if ATOMIC_REWRITE:
        # x86 lock RMW / xchg [mem] as atomic helpers and MSVC's `lock or [rsp], 0` as a fence, each
        # line proven against the instruction bytes of Frostpunk.exe (tools/atomic_sites.py).
        sys.path.insert(0, str(ROOT / 'tools'))
        import atomic_sites
        text, _ = atomic_sites.rewrite_text(text, 'main')
    if index == 822:
        sys.path.insert(0, str(ROOT / 'tools'))
        import mobile_detail_overlay
        text = mobile_detail_overlay.apply(text)  # mobile screen-size threshold of the entity renderer query
    return text


def special_units():
    """(plain overlay objects, patched unit objects, all rows) of the production link."""
    rows = [line.strip().strip('"') for line in BASE_LINK.read_text().splitlines() if line.strip()]
    standard, patched = {}, {}
    explicit = {(ROOT / path).as_posix(): unit for unit, path in UNIT_ROWS.items()}
    for row in rows:
        normal = row.replace('\\', '/')
        m = re.search(r'/build/arm64/(?:eh-)?overlay-objects/recomp_funcs_(\d{4})\.o$', normal)
        if m:
            standard[int(m[1])] = row
            continue
        m = re.search(r'/recomp_funcs_(\d{4})[^/]*\.o$', normal)
        if m and '/generated/' not in normal:  # the lifted dependencies have units of their own
            patched[int(m[1])] = row
        elif normal in explicit:
            patched[explicit[normal]] = row
    return standard, patched, rows


def scan_unit(index):
    """Pass A: light description of one unit."""
    path = unit_path(index)
    prologue, chunks = parse_unit(unit_text(index), index)
    rows = []
    for ch in chunks:
        rows.append((ch.va, ch.eh, ch.foreign, ch.ljump, len(ch.insns)))
    return index, rows


class Plan:
    """Everything the per-unit workers need; small enough to send to each worker once."""

    def __init__(self):
        self.unit_of = {}       # fragment start -> unit
        self.kind_of = {}       # fragment start -> 'eh' | 'foreign' | 'plain'
        self.special = set()    # unit indices kept as old objects
        self.intervals = []     # (begin, end, function start) from IDA
        self.starts = []
        self.fn_starts = set()
        self.switches = {}      # jump address -> [targets]
        self.live_in = {}       # fragment start -> flag bits (after the fixpoint)
        self.flags_mode = 'exact'
        self.merge = True
        self.lt = True
        self.icall = False      # inline lookup-cache fast path for indirect calls (needs the patched bootstrap.c)
        self.imports = {}       # import address table slot -> index in recomp_imports
        # Guest C++ throws are not supported by the Android runtime (nothing links es3_eh_throw), so
        # the setjmp landing pad of a try-function can never be entered: translate it like any other.
        self.elide_eh = True
        self.prefetch = True

    def owner(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.intervals[i][0] <= addr < self.intervals[i][1]:
            return self.intervals[i][2]
        return None

    def region_of(self, va):
        """Primary entry of the merged function this fragment belongs to, or None."""
        if not self.merge or self.kind_of.get(va) != 'plain' or self.unit_of[va] in self.special:
            return None
        f = self.owner(va)
        if f is None or self.kind_of.get(f) != 'plain' or self.unit_of[f] in self.special:
            return None
        return f


_IMPORT_ROW = re.compile(r'^\s*\{ 0x([0-9A-F]+)ull, "', re.M)
IAT_READ = re.compile(r'^rd64\(GVA\(0x([0-9A-Fa-f]+)\)\)$')


def make_plan(jobs, merge=True, keep_special=False, elide_eh=True):
    plan = Plan()
    plan.merge = merge
    plan.elide_eh = elide_eh
    standard, patched, _ = special_units()
    indices = sorted(int(p.stem[-4:]) for p in GENERATED.glob('recomp_funcs_*.c'))
    if set(standard) | set(patched) != set(indices) or set(standard) & set(patched):
        raise ValueError('production link rows do not cover every unit exactly once')
    plan.special = set(patched) if keep_special else set()
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
        for index, rows in pool.map(scan_unit, indices, chunksize=8):
            for va, eh, foreign, ljump, count in rows:
                if va in plan.unit_of:
                    raise ValueError('fragment %012X defined twice' % va)
                plan.unit_of[va] = index
                plan.kind_of[va] = 'foreign' if foreign else 'eh' if (eh and not elide_eh) else 'plain'
    functions = json.loads((ROOT / 'research/ida/functions.json').read_text())
    plan.intervals = sorted((a, b, start) for start, _flags, chunks in functions for a, b in chunks)
    plan.starts = [row[0] for row in plan.intervals]
    plan.fn_starts = {start for start, _flags, _chunks in functions}
    plan.switches = {int(k): v['targets'] for k, v in json.loads((ROOT / 'research/ida/switches.json').read_text()).items()}
    slots = _IMPORT_ROW.findall((GENERATED / 'recomp_imports.c').read_text())
    plan.imports = {int(slot, 16): index for index, slot in enumerate(slots)}
    return plan


# ---------------------------------------------------------------------------------------------
# One emitted function.
def sig(it):
    k = it.kind
    if k in ('goto', 'jmp'):
        return ('j', it.target)
    if k in ('gotocc', 'jcc'):
        return ('jc', it.cond, it.target)
    if k in ('loop_in', 'loop_out'):
        return ('loop', it.cond, it.target)
    return (k, it.code)


class Fn:
    __slots__ = ('entry', 'unit', 'members', 'order', 'insn', 'succ', 'source', 'why', 'live', 'after_call', 'lt_valid',
                 'nexts', 'outs')

    def __init__(self, entry, unit, members):
        self.entry, self.unit, self.members = entry, unit, members
        self.order, self.insn, self.succ, self.source = [], {}, {}, {}
        self.why = None
        self.live, self.after_call, self.lt_valid = {}, {}, {}
        self.nexts, self.outs = {}, {}


def build_fn(entry, unit, members, plan):
    """Union of the member fragments' instructions, keyed by address."""
    fn = Fn(entry, unit, members)
    mset = {ch.va for ch in members}
    insn, succ, source = fn.insn, fn.succ, fn.source
    for ch in members:
        ins = ch.insns
        index = {it.addr: i for i, it in enumerate(ins)}
        # Everything reachable from the fragment start without entering another function's fragment.
        work, seen = [ins[0].addr], set()
        while work:
            a = work.pop()
            idx = index.get(a)
            if idx is None or a in seen:
                continue
            if idx > 0 and a in plan.unit_of and a not in mset:
                continue  # another function's fragment starts here: reached through its own symbol
            seen.add(a)
            it = ins[idx]
            prev = insn.get(a)
            if prev is None:
                insn[a] = it
                source[a] = ch
            elif sig(prev) != sig(it):
                fn.why = 'instruction %012X differs between fragments' % a
                return fn
            if it.kind in ('goto', 'gotocc', 'loop_in'):
                work.append(it.target)
            elif it.kind == 'ijmp':
                work.extend(plan.switches.get(a, ()))
            if not it.terminator:
                nxt = ins[idx + 1].addr if idx + 1 < len(ins) else ch.fall
                if nxt is None:
                    fn.why = 'fragment %012X runs off its end' % ch.va
                    return fn
                if succ.setdefault(a, nxt) != nxt:
                    fn.why = 'successor of %012X differs between fragments' % a
                    return fn
                work.append(nxt)
    if entry not in insn:
        fn.why = 'entry has no instruction'
        return fn
    fn.order = sorted(insn)
    for a in fn.order:
        it = insn[a]
        if it.use == 0 and it.must == 0 and it.may == 0 and it.ltgen is None:
            flag_effect(it)
        # What the original text reached with a goto must still be inside the function, and a
        # fall-through may only leave it at another fragment's own symbol.
        if it.kind in ('goto', 'gotocc', 'loop_in') and it.target not in insn and it.target not in plan.unit_of:
            fn.why = 'local jump %012X -> %012X left the function' % (a, it.target)
            return fn
        if not it.terminator and succ[a] not in insn and succ[a] not in plan.unit_of:
            fn.why = 'fall-through %012X -> %012X has no symbol' % (a, succ[a])
            return fn
    return fn


def edges(fn, plan):
    """Successors inside the function and exits (target fragment or None) per instruction."""
    insn, succ = fn.insn, fn.succ
    for a in fn.order:
        it = insn[a]
        nexts, outs = [], []
        k = it.kind
        if k in ('goto', 'jmp', 'gotocc', 'jcc', 'loop_in', 'loop_out'):
            (nexts if it.target in insn else outs).append(it.target)
        if k == 'ijmp':
            internal, external = ijmp_targets(fn, a, plan)
            nexts.extend(internal)
            outs.extend(external)
        if not it.terminator:
            s = succ[a]
            (nexts if s in insn else outs).append(s)
        fn.nexts[a], fn.outs[a] = nexts, outs


def ijmp_targets(fn, a, plan):
    """Statically known targets of an indirect jump inside its own fragment's address range:
    the switch cases IDA found, and the fragment starts among the fragment's labels (a function
    pointer may name a small function lifted in the same fragment as the jump).  Returns the
    targets inside this function and those that are another function's symbol."""
    ch = fn.source[a]
    targets = set(plan.switches.get(a, ()))
    targets.update(t for t in ch.cases if t in plan.unit_of)
    internal = sorted(t for t in targets if t in fn.insn)
    external = sorted(t for t in targets if t not in fn.insn and t in plan.unit_of)
    return internal, external


def liveness(fn, plan):
    """Backward flag liveness; a call leaves every flag undefined (Win64 ABI)."""
    insn, live = fn.insn, fn.live
    live_in = plan.live_in
    order = fn.order[::-1]
    for a in order:
        live[a] = 0
    changed = True
    while changed:
        changed = False
        for a in order:
            it = insn[a]
            out = 0
            for s in fn.nexts[a]:
                out |= live[s]
            for t in fn.outs[a]:
                out |= live_in.get(t, 0)
            if it.kind == 'call':
                fn.after_call[a] = out
                new = live_in.get(it.target, 0)
            elif it.kind == 'icall':
                fn.after_call[a] = out
                new = 0
            else:
                new = it.use | (out & ~it.must)
            if new != live[a]:
                live[a] = new
                changed = True
    return live[fn.entry]


def lt_validity(fn):
    """Forward: is fl.lt == (sf != of) known before each instruction?"""
    insn, valid = fn.insn, fn.lt_valid
    preds = collections.defaultdict(list)
    for a in fn.order:
        for s in fn.nexts[a]:
            preds[s].append(a)
    for a in fn.order:
        valid[a] = True
    valid[fn.entry] = False

    def out(a):
        it = insn[a]
        if it.kind in ('call', 'icall'):
            return False
        if it.ltgen:
            return True
        if it.may & (SF | OF):
            return False
        return valid[a]
    changed = True
    while changed:
        changed = False
        for a in fn.order:
            if a == fn.entry:
                new = False
            else:
                new = all(out(p) for p in preds[a]) if preds[a] else True
            if new != valid[a]:
                valid[a] = new
                changed = True


# ---------------------------------------------------------------------------------------------
# Emission.
def label(va):
    return 'L_%012X' % va


def symbol(va):
    """The symbol under which the translated body of this fragment is defined."""
    return ALIASES.get(va, label(va))


def spill(bits):
    return ''.join('c->%s = fl.%s; ' % (n, n) for n in FLAGS if bits & FLAG_BIT[n])


def reload(bits):
    return ''.join('fl.%s = c->%s; ' % (n, n) for n in FLAGS if bits & FLAG_BIT[n])


def xf(code, lt_ok):
    """A generated statement with its flags moved into the local `fl`."""
    code = HELPER_CALL.sub(lambda m: m[1] + '_fl(&fl', code)
    code = FLAG_TOKEN.sub(lambda m: 'fl.' + m[1], code)
    if lt_ok:
        code = code.replace('fl.sf != fl.of', 'fl.lt').replace('fl.sf == fl.of', '!fl.lt')
    return code


class Emitter:
    def __init__(self, plan):
        self.plan = plan
        self.refs = set()
        self.alias_refs = set()
        self.slow = {}          # fragment va -> text of its slow indirect-jump body
        self.report = collections.Counter()
        self.notes = []

    def direct(self, t):
        return t in self.plan.unit_of and eligible(t)

    def name(self, t, unit):
        """A wrapped function is reached through its wrapper, except from its own unit, where the
        production objects bind the reference to the renamed original."""
        if t in ALIASES and self.plan.unit_of[t] == unit and t not in ALIAS_WRAP_OWN_UNIT:
            self.alias_refs.add(t)
            return ALIASES[t]
        self.refs.add(t)
        return label(t)

    def call(self, t, unit):
        if self.direct(t):
            return '%s(c);' % self.name(t, unit)
        return 'dispatch(c, 0x%Xull);' % t

    def jump_out(self, t, all_flags, unit):
        bits = ALL if all_flags else self.plan.live_in.get(t, 0)
        if bits and not all_flags:
            self.report['flag_spills'] += 1
        if self.direct(t):
            return '{ ' + spill(bits) + '__attribute__((musttail)) return %s(c); }' % self.name(t, unit)
        return '{ ' + spill(bits) + 'dispatch(c, 0x%Xull); return; }' % t

    def function(self, fn):
        plan, insn, order = self.plan, fn.insn, fn.order
        conservative = plan.flags_mode == 'all'
        edges(fn, plan)
        entry_live = liveness(fn, plan)
        if plan.lt and not conservative:
            lt_validity(fn)
        # A label is needed wherever control arrives other than by falling through in address order.
        labels = set()
        position = {a: i for i, a in enumerate(order)}
        for a in order:
            it = insn[a]
            if it.kind in ('gotocc', 'jcc', 'loop_in', 'loop_out', 'goto', 'jmp') and it.target in insn:
                labels.add(it.target)
            if it.kind == 'ijmp':
                labels.update(fn.nexts[a])
            if not it.terminator:
                s = fn.succ[a]
                if s in insn and position[s] != position[a] + 1:
                    labels.add(s)
        out = ['void %s(CPU *restrict c)' % symbol(fn.entry), '{', '    FL fl;']
        load_bits = ALL if conservative else entry_live
        if load_bits:
            out.append('    ' + reload(load_bits).rstrip())
            if (load_bits & (SF | OF)) == (SF | OF):
                out.append('    fl.lt = (fl.sf != fl.of);')
        if order[0] != fn.entry:
            labels.add(fn.entry)
            out.append('    goto %s;' % label(fn.entry))
        for a in order:
            it = insn[a]
            if a in labels:
                out.append('%s:' % label(a))
            if a in PREFETCH and plan.prefetch:
                out.append('    ' + PREFETCH[a])
                self.report['prefetch_sites'] += 1
            lt_ok = (not conservative) and plan.lt and fn.lt_valid.get(a, False)
            k = it.kind
            comment = '/* %012X: %s */' % (a, it.asm)
            all_flags = conservative
            unit = fn.source[a].unit
            if k == 'plain':
                code = xf(it.code, lt_ok)
                if it.ltgen and plan.lt and not conservative:
                    code = code.replace(it.ltgen + '_fl(&fl', it.ltgen + '_lt(&fl', 1)
                text = code
            elif k == 'ret':
                text = (spill(ALL) if conservative else '') + 'c->rsp += %d; return;' % it.k
            elif k == 'retf':
                text = (spill(ALL) if conservative else '') + 'return;'
            elif k == 'call':
                pre = spill(ALL if conservative else plan.live_in.get(it.target, 0))
                post = reload(ALL) if conservative else reload(fn.after_call.get(a, 0))
                if not conservative and fn.after_call.get(a, 0):
                    self.report['flags_read_after_call'] += 1
                    self.notes.append('flags %d read after call at %012X' % (fn.after_call[a], a))
                text = 'push64(c, 0x%Xull); %s%s %s' % (it.ret, pre, self.call(it.target, unit), post)
            elif k == 'icall':
                pre = spill(ALL) if conservative else ''
                post = reload(ALL) if conservative else reload(fn.after_call.get(a, 0))
                if not conservative and fn.after_call.get(a, 0):
                    self.report['flags_read_after_call'] += 1
                    self.notes.append('flags %d read after indirect call at %012X' % (fn.after_call[a], a))
                text = '{ uint64_t _ct = %s; push64(c, 0x%Xull); %s%s; } %s' % (
                    xf(it.expr, lt_ok), it.ret, pre, self.indirect(it.expr, '_ct'), post)
            elif k in ('goto', 'jmp'):
                text = ('goto %s;' % label(it.target)) if it.target in insn else self.jump_out(it.target, all_flags, unit)
            elif k in ('gotocc', 'jcc'):
                cond = xf(it.cond, lt_ok)
                if it.target in insn:
                    text = 'if (%s) goto %s;' % (cond, label(it.target))
                else:
                    text = 'if (%s) { %s }' % (cond, self.jump_out(it.target, all_flags, unit))
            elif k in ('loop_in', 'loop_out'):
                cond = xf(it.cond, lt_ok)
                if it.target in insn:
                    text = 'c->rcx--; if (%s) goto %s;' % (cond, label(it.target))
                else:
                    text = 'c->rcx--; if (%s) { %s }' % (cond, self.jump_out(it.target, all_flags, unit))
            elif k == 'ijmp':
                text = self.indirect_jump(fn, a, it, lt_ok)
            elif k == 'popjmp':
                text = (spill(ALL) if conservative else '') + it.code
            else:
                raise ValueError(k)
            first_line = True
            for line in text.split('\n'):
                out.append('    ' + line + ('  ' + comment if first_line else ''))
                first_line = False
            if not it.terminator:
                s = fn.succ[a]
                nxt = order[position[a] + 1] if position[a] + 1 < len(order) else None
                if s != nxt:
                    out.append('    ' + (('goto %s;' % label(s)) if s in insn else self.jump_out(s, all_flags, unit)))
        out.append('}')
        return '\n'.join(out)

    def indirect(self, expr, value):
        """Call through a value read from memory: an import slot names its handler statically."""
        plan = self.plan
        if not plan.icall:
            return 'dispatch(c, %s)' % value
        m = IAT_READ.match(expr)
        if m and int(m[1], 16) in plan.imports:
            return 'fl_import(c, %s, %du)' % (value, plan.imports[int(m[1], 16)])
        return 'fl_icall(c, %s)' % value

    def indirect_jump(self, fn, a, it, lt_ok):
        plan = self.plan
        ch = fn.source[a]
        internal, external = ijmp_targets(fn, a, plan)
        text = ['{ uint64_t _ind = %s;' % xf(it.expr, lt_ok)]
        every = internal + external + list(ch.cases)
        if every:
            # One range test keeps the usual case (a tail call into some other function) short.
            lo, hi = min(every), max(every)
            text.append('  if (_ind - 0x%Xull <= 0x%Xull) {' % (lo, hi - lo))
            if internal or external:
                text.append('    switch (_ind) {')
                for t in internal:
                    text.append('      case 0x%Xull: goto %s;' % (t, label(t)))
                for t in external:
                    text.append('      case 0x%Xull: %s' % (t, self.jump_out(t, plan.flags_mode == 'all', fn.source[a].unit)))
                text.append('      default: break;')
                text.append('    }')
            if ch.cases:
                # A label of the fragment nobody listed: the original all-labels body takes over.
                self.slow.setdefault(ch.va, slow_body(ch))
                text.append('    if (_ind - 0x%Xull <= 0x%Xull) { %sc->rip = _ind; __attribute__((musttail)) return LJ_%012X(c); }'
                            % (min(ch.cases), max(ch.cases) - min(ch.cases), spill(ALL), ch.va))
            text.append('  }')
        m = IAT_READ.match(it.expr)
        if plan.icall and m and int(m[1], 16) in plan.imports:
            text.append('  fl_import(c, _ind, %du); return; }' % plan.imports[int(m[1], 16)])
        else:
            text.append('  dispatch_jmp(c, _ind); return; }')
        return '\n'.join(text)


def slow_body(ch):
    """The fragment's original text as a cold function that enters at the address left in c->rip."""
    text = ch.text.replace('void %s(CPU *c)' % label(ch.va), 'static __attribute__((cold, noinline)) void LJ_%012X(CPU *c)' % ch.va, 1)
    head = '    c->rip = 0x%Xull;\n    uint64_t _ind = 0;' % ch.va
    if text.count(head) != 1:
        raise ValueError('slow body of %012X has no single entry' % ch.va)
    # The requested address must be read before the body's own first bookkeeping store replaces it.
    text = text.replace(head, '    uint64_t _ind = c->rip; goto _ljump;', 1)
    if ch.eh:  # entered past the landing-pad registration: drop it together with its release
        text = EH_ENTER.sub('', text).replace('ES3_EH_LEAVE(); ', '')
        if 'ES3_EH' in text:
            raise ValueError('slow body of %012X keeps exception bookkeeping' % ch.va)
    return text


EH_ENTER = re.compile(r'^    ES3_EH_ENTER\(0x[0-9A-F]+ull\);\n', re.M)
OLD_LINKED = re.compile(r'\bfp_dispatch_main\(c,\s*(0x([0-9A-Fa-f]+)ull),\s*L_[0-9A-F]{12}\)')


def old_text(ch, em):
    """An exception-entering or unparsed fragment, kept verbatim (direct-dispatch form as in production)."""
    plan = em.plan

    def replace(m):
        va = int(m[2], 16)
        if va in plan.unit_of and MAIN_BASE <= va < MAIN_END and not AUDIO_BEGIN <= va <= AUDIO_END \
                and va not in (0x140e222e0, 0x140d95650, 0x140f499a0, 0x1410596a9, 0x1410597e9):
            return 'fp_dispatch_main(c, %s, %s)' % (m[1], em.name(va, ch.unit))
        return 'dispatch(c, %s)' % m[1]
    text = OLD_LINKED.sub(replace, OLD_DISPATCH.sub(replace, ch.text))
    return text.replace('void %s(CPU *c)' % label(ch.va), 'void %s(CPU *c)' % symbol(ch.va), 1)


# ---------------------------------------------------------------------------------------------
# Per-unit work (runs in worker processes).
_PLAN = None
_CACHE = collections.OrderedDict()


def _init(plan):
    global _PLAN
    _PLAN = plan
    _CACHE.clear()


def parsed(index):
    hit = _CACHE.get(index)
    if hit is None:
        path = unit_path(index)
        prologue, chunks = parse_unit(unit_text(index), index)
        hit = (prologue, chunks, {ch.va: ch for ch in chunks})
        _CACHE[index] = hit
        if len(_CACHE) > 6:
            _CACHE.popitem(last=False)
    return hit


def fragment(va):
    return parsed(_PLAN.unit_of[va])[2][va]


_STARTS_SORTED = None


def members_of(entry):
    plan = _PLAN
    global _STARTS_SORTED
    if _STARTS_SORTED is None:
        _STARTS_SORTED = sorted(plan.unit_of)
    vas = []
    for begin, end, start in function_intervals(entry):
        lo = bisect.bisect_left(_STARTS_SORTED, begin)
        hi = bisect.bisect_left(_STARTS_SORTED, end)
        for va in _STARTS_SORTED[lo:hi]:
            if plan.region_of(va) == entry:
                vas.append(va)
    return [fragment(va) for va in sorted(set(vas))]


_FN_INTERVALS = None


def function_intervals(entry):
    global _FN_INTERVALS
    if _FN_INTERVALS is None:
        table = collections.defaultdict(list)
        for row in _PLAN.intervals:
            table[row[2]].append(row)
        _FN_INTERVALS = table
    return _FN_INTERVALS[entry]


def unit_functions(index):
    """(fragment, Fn or None) for every fragment defined in this unit, in file order."""
    plan = _PLAN
    prologue, chunks, _ = parsed(index)
    result = []
    for ch in chunks:
        if (ch.eh and not plan.elide_eh) or ch.foreign:
            result.append((ch, None))
            continue
        fn = None
        if plan.region_of(ch.va) == ch.va:
            members = members_of(ch.va)
            if len(members) > 1:
                fn = build_fn(ch.va, index, members, plan)
                if fn.why:
                    fn = None
        if fn is None:
            fn = build_fn(ch.va, index, [ch], plan)
            if fn.why:
                fn = None  # keep the original text; it stops in the dispatcher exactly as before
        result.append((ch, fn))
    return prologue, result


def live_pass(index):
    """Entry liveness of every function this unit defines, under the current plan.live_in."""
    plan = _PLAN
    _prologue, rows = unit_functions(index)
    out = {}
    for ch, fn in rows:
        if fn is None:
            # Old-style body: reads its flags from `c`; analyse the fragment alone.
            if ch.foreign:
                out[ch.va] = ALL
                continue
            fn = build_fn(ch.va, index, [ch], plan)
            if fn.why:
                out[ch.va] = ALL
                continue
        edges(fn, plan)
        out[ch.va] = liveness(fn, plan)
    return index, out


HEADER = '/* generated by tools/fastlift.py from %s - do not edit */\n#include "fastlift.h"\n'


def generate_unit(index):
    plan = _PLAN
    prologue, rows = unit_functions(index)
    em = Emitter(plan)
    bodies, old = [], []
    stats = collections.Counter()
    for ch, fn in rows:
        if fn is None:
            old.append(old_text(ch, em))
            stats['old_style'] += 1
            continue
        bodies.append(em.function(fn))
        stats['merged' if len(fn.members) > 1 else 'single'] += 1
        stats['instructions'] += len(fn.order)
        if len(fn.members) > 1:
            stats['merged_fragments'] += len(fn.members)
    path = unit_path(index)
    keep = [line for line in prologue if line.strip() not in DROP_PROLOGUE]
    defined = {ch.va for ch, _ in rows}
    names = {label(va) for va in em.refs} | {symbol(va) for va in defined} | {ALIASES[va] for va in em.alias_refs}
    decls = ['void %s(CPU *FL_NE c);' % name for name in sorted(names)]
    decls += ['static void LJ_%012X(CPU *c);' % va for va in sorted(em.slow)]
    slow = [em.slow[va] for va in sorted(em.slow)]
    # Text kept from the original lift (and the helpers patched units bring along) reads guest
    # memory through the canonical accessors, exactly as the production objects do.
    text = (HEADER % path.relative_to(ROOT).as_posix()) + '\n'.join(decls) + '\n#include "fastlift_canonical.h"\n' + \
        '\n'.join(keep) + '\n\n' + '\n\n'.join(old + slow) + '\n#include "fastlift_volatile.h"\n\n' + '\n\n'.join(bodies) + '\n'
    return index, text, dict(stats), dict(em.report), em.notes


# ---------------------------------------------------------------------------------------------
# Headers.
FLAG_HELPER_NAMES = ['flags_logicz', 'flags_add', 'flags_sub', 'flags_adc', 'flags_sbb', 'flags_incs', 'flags_decs',
                     'op_shl', 'op_shr', 'op_sar', 'op_rol', 'op_ror', 'op_shld', 'op_shrd', 'op_bt', 'op_bts', 'op_btr',
                     'op_btc', 'bitscan_undef_flags', 'op_bsf', 'op_bsr', 'sse_compare', 'eflags_pack', 'eflags_unpack']


def flag_helpers(cpu64):
    """The flag helpers of cpu64.h, re-targeted from `CPU *c` to the local flag struct.  Derived
    mechanically so they cannot drift from the canonical definitions."""
    lines = cpu64.replace('\r\n', '\n').split('\n')
    out = []
    names = '|'.join(FLAG_HELPER_NAMES)
    for name in FLAG_HELPER_NAMES:
        start = [i for i, line in enumerate(lines) if re.match(r'^static inline \w+\s+%s\(CPU \*c\b' % name, line)]
        if len(start) != 1:
            raise ValueError('cpu64.h helper %s not found exactly once' % name)
        i = start[0]
        if lines[i].rstrip().endswith('}'):
            body = [lines[i]]
        else:
            j = i
            while lines[j] != '}':
                j += 1
            body = lines[i:j + 1]
        text = '\n'.join(body)
        text = re.sub(r'\b(%s)\(' % names, lambda m: m[1] + '_fl(', text)
        text = text.replace('(CPU *c', '(FL *c', 1)
        text = text.replace('static inline', 'static inline __attribute__((always_inline))', 1)
        if 'CPU *' in text:
            raise ValueError('helper %s still mentions CPU' % name)
        out.append(text)
    return '\n'.join(out) + '\n'


FASTLIFT_H = r'''/* generated by tools/fastlift.py - do not edit */
#ifndef FASTLIFT_H
#define FASTLIFT_H
#if !defined(__ANDROID__) || !defined(__aarch64__)
#error fastlift units are Android AArch64 only
#endif
/* The canonical accessors keep their bodies under another name; guest memory gets volatile ones. */
#define rd8 cpu64_rd8
#define rd16 cpu64_rd16
#define rd32 cpu64_rd32
#define rd64 cpu64_rd64
#define wr8 cpu64_wr8
#define wr16 cpu64_wr16
#define wr32 cpu64_wr32
#define wr64 cpu64_wr64
#define rd128 cpu64_rd128
#define wr128 cpu64_wr128
#define rdss cpu64_rdss
#define rdsd cpu64_rdsd
#define rdxm cpu64_rdxm
#define rdf32 cpu64_rdf32
#define wrss cpu64_wrss
#define wrsd cpu64_wrsd
#define wrxm cpu64_wrxm
#define frostpunk_addss cpu64_addss
#define frostpunk_mulss cpu64_mulss
#include "fastlift_cpu64.h"
#include "aot_cpu.h"
_Static_assert(sizeof(XMM) == 16 && _Alignof(XMM) == 8, "XMM register layout");
_Static_assert(offsetof(CPU, xmm) == 184 && offsetof(CPU, mxcsr) == 440 && sizeof(CPU) == 448, "CPU layout of the runtime");
#undef rd8
#undef rd16
#undef rd32
#undef rd64
#undef wr8
#undef wr16
#undef wr32
#undef wr64
#undef rd128
#undef wr128
#undef rdss
#undef rdsd
#undef rdxm
#undef rdf32
#undef wrss
#undef wrsd
#undef wrxm
#undef frostpunk_addss
#undef frostpunk_mulss
/* The image is mapped at its preferred base on Android (bootstrap sets g_image_delta = 0). */
#undef GVA
#define GVA(abs) ((uint64_t)(abs))
/* Exception landing pads as in the production build (android/host/eh_entry_nomask.h). */
#undef ES3_EH_ENTER
#define ES3_EH_ENTER(va)                                                      \
    jmp_buf _ehjb;                                                            \
    es3_ehframe_t _ehf;                                                       \
    es3_eh_push(&_ehf, (va), &_ehjb);                                         \
    if (_setjmp(_ehjb)) { _ind = _ehf.resume; goto _ljump; }
#define FL_NE __attribute__((noescape))
#define FL_PREFETCH(address) __builtin_prefetch((const void *)(uintptr_t)(address), 0, 3)
#define FL_INLINE static inline __attribute__((always_inline))
/* Guest memory is shared with other guest threads and the original code re-reads it on purpose:
 * one access per instruction operand, in program order, never merged or hoisted. */
typedef uint16_t fl_u16 __attribute__((aligned(1)));
typedef uint32_t fl_u32 __attribute__((aligned(1)));
typedef uint64_t fl_u64 __attribute__((aligned(1)));
typedef float fl_f32 __attribute__((aligned(1)));
typedef double fl_f64 __attribute__((aligned(1)));
typedef uint64_t fl_v128 __attribute__((vector_size(16), aligned(1)));
FL_INLINE uint8_t  rd8 (uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
FL_INLINE uint16_t rd16(uint64_t a) { return *(volatile fl_u16 *)(uintptr_t)a; }
FL_INLINE uint32_t rd32(uint64_t a) { return *(volatile fl_u32 *)(uintptr_t)a; }
FL_INLINE uint64_t rd64(uint64_t a) { return *(volatile fl_u64 *)(uintptr_t)a; }
FL_INLINE void wr8 (uint64_t a, uint8_t  v) { *(volatile uint8_t *)(uintptr_t)a = v; }
FL_INLINE void wr16(uint64_t a, uint16_t v) { *(volatile fl_u16 *)(uintptr_t)a = v; }
FL_INLINE void wr32(uint64_t a, uint32_t v) { *(volatile fl_u32 *)(uintptr_t)a = v; }
FL_INLINE void wr64(uint64_t a, uint64_t v) { *(volatile fl_u64 *)(uintptr_t)a = v; }
FL_INLINE float  rdss(uint64_t a) { return *(volatile fl_f32 *)(uintptr_t)a; }
FL_INLINE double rdsd(uint64_t a) { return *(volatile fl_f64 *)(uintptr_t)a; }
FL_INLINE float  rdf32(uint64_t a) { return rdss(a); }
FL_INLINE void   wrss(uint64_t a, float  v) { *(volatile fl_f32 *)(uintptr_t)a = v; }
FL_INLINE void   wrsd(uint64_t a, double v) { *(volatile fl_f64 *)(uintptr_t)a = v; }
FL_INLINE XMM rdxm(uint64_t a) { fl_v128 v = *(volatile fl_v128 *)(uintptr_t)a; XMM x; __builtin_memcpy(&x, &v, 16); return x; }
FL_INLINE void wrxm(uint64_t a, XMM x) { fl_v128 v; __builtin_memcpy(&v, &x, 16); *(volatile fl_v128 *)(uintptr_t)a = v; }
FL_INLINE void rd128(uint64_t a, XMM *d) { *d = rdxm(a); }
FL_INLINE void wr128(uint64_t a, const XMM *s) { wrxm(a, *s); }
/* ADDSS/MULSS with the x86 NaN rules of sse_scalar.h: a NaN result is the only case that needs
 * the payload fix-up, and any NaN operand makes the result a NaN. */
FL_INLINE float frostpunk_addss(float a, float b) {
    float r = a + b;
    if (__builtin_expect(r != r, 0)) r = frostpunk_sse_result(a, b, r);
    return r;
}
FL_INLINE float frostpunk_mulss(float a, float b) {
    float r = a * b;
    if (__builtin_expect(r != r, 0)) r = frostpunk_sse_result(a, b, r);
    return r;
}
/* Flags of the function being executed.  lt mirrors (sf != of) where the generator proved it. */
typedef struct { uint64_t cf, zf, sf, of, pf, af, lt; } FL;
#include "fastlift_flags.h"
FL_INLINE uint64_t flags_sub_lt(FL *c, uint64_t a, uint64_t b, int sz) {
    uint64_t r = flags_sub_fl(c, a, b, sz);
    int shift = 64 - sz * 8;
    c->lt = (int64_t)(a << shift) < (int64_t)(b << shift);
    return r;
}
FL_INLINE uint64_t flags_logicz_lt(FL *c, uint64_t r, int sz) {
    r = flags_logicz_fl(c, r, sz);
    c->lt = c->sf;
    return r;
}
FL_INLINE uint64_t flags_decs_lt(FL *c, uint64_t a, int sz) {
    uint64_t r = flags_decs_fl(c, a, sz);
    int shift = 64 - sz * 8;
    c->lt = (int64_t)(a << shift) < (int64_t)((uint64_t)1 << shift);
    return r;
}
/* The same runtime entry points, declared so that passing `c` does not count as a capture. */
void fl_dispatch(CPU *FL_NE c, uint64_t target) __asm__("dispatch");
void fl_dispatch_jmp(CPU *FL_NE c, uint64_t target) __asm__("dispatch_jmp");
__attribute__((visibility("hidden"))) void fp_dispatch_main(CPU *FL_NE c, uint64_t target, void (*linked)(CPU *));
int fl_aot_cpu_instruction(CPU *FL_NE c, uint64_t original_va, const char *instruction) __asm__("fp_aot_cpu_instruction");
int fl_aot_packed_compare(CPU *FL_NE c, unsigned destination, const XMM *FL_NE source, unsigned operation) __asm__("fp_aot_packed_compare");
int fl_aot_packed_compare_memory(CPU *FL_NE c, unsigned destination, uint64_t address, unsigned operation) __asm__("fp_aot_packed_compare_memory");
#define dispatch fl_dispatch
#define dispatch_jmp fl_dispatch_jmp
/* Indirect guest call: the dispatcher's own lookup cache (android/host/bootstrap.c), read inline.
 * It only ever holds immutable main-table functions the dispatcher does not treat specially. */
typedef void (*fl_function)(CPU *FL_NE);
extern __attribute__((visibility("hidden"))) _Atomic uint64_t fp_lookup_cache[32768];
__attribute__((visibility("hidden"))) void fp_lookup_anchor(void);
FL_INLINE void fl_icall(CPU *FL_NE c, uint64_t target) {
    uint64_t tag = target - 0x140000000ull + 1;
    uint64_t entry = __atomic_load_n((uint64_t *)&fp_lookup_cache[((target >> 2) ^ (target >> 14)) & 32767u], __ATOMIC_RELAXED);
    if (__builtin_expect((entry >> 32) == tag && tag != 0, 1)) {
        ((fl_function)((intptr_t)(uintptr_t)&fp_lookup_anchor + (intptr_t)(int32_t)(uint32_t)entry))(c);
        return;
    }
    fl_dispatch(c, target);
}

/* Call through an import address table slot whose import is known statically: the handler the
 * dispatcher published for it (android/host/bootstrap.c), with the same return-address pop. */
typedef int (*fl_import_function)(CPU *FL_NE);
extern __attribute__((visibility("hidden"))) int (*_Atomic fp_import_fast[557])(CPU *);
FL_INLINE void fl_import(CPU *FL_NE c, uint64_t target, unsigned index) {
    fl_import_function fast = (fl_import_function)__atomic_load_n((void **)&fp_import_fast[index], __ATOMIC_ACQUIRE);
    if (__builtin_expect(fast != 0 && target == 0xE5E30000ull + (uint64_t)index * 8u, 1)) {
        fast(c);
        c->rsp += 8;
        return;
    }
    fl_dispatch(c, target);
}
#define fp_aot_cpu_instruction fl_aot_cpu_instruction
#define fp_aot_packed_compare fl_aot_packed_compare
#define fp_aot_packed_compare_memory fl_aot_packed_compare_memory
/* android/host/mobile_detail.c: reads the caller's return address through `c`, keeps nothing. */
unsigned fl_test_detail_mode(CPU *FL_NE c) __asm__("fp_test_detail_mode");
float fp_test_detail_threshold(float original, unsigned mode);
#define fp_test_detail_mode fl_test_detail_mode
#endif /* FASTLIFT_H */
'''
ACCESSORS = ('rd8', 'rd16', 'rd32', 'rd64', 'wr8', 'wr16', 'wr32', 'wr64', 'rd128', 'wr128', 'rdss', 'rdsd', 'rdxm',
             'rdf32', 'wrss', 'wrsd', 'wrxm')


XMM_LANES = '''#define FL_LANES(type, name) typedef type name __attribute__((vector_size(16), aligned(8)))
FL_LANES(float, fl_f32x4); FL_LANES(double, fl_f64x2); FL_LANES(uint32_t, fl_u32x4); FL_LANES(uint64_t, fl_u64x2);
FL_LANES(int32_t, fl_i32x4); FL_LANES(int64_t, fl_i64x2); FL_LANES(uint8_t, fl_u8x16); FL_LANES(int8_t, fl_i8x16);
FL_LANES(uint16_t, fl_u16x8); FL_LANES(int16_t, fl_i16x8);
/* The same sixteen bytes, lane names and alignment as the canonical union, but each lane access
 * is a whole-register load or store: the compiler then keeps a guest XMM register in one SIMD
 * value between calls instead of forwarding narrow stores through memory. */
typedef union {
    fl_f32x4 f32; fl_f64x2 f64; fl_u32x4 u32; fl_u64x2 u64; fl_i32x4 i32; fl_i64x2 i64;
    fl_u8x16 u8; fl_i8x16 i8; fl_u16x8 u16; fl_i16x8 i16;
} XMM;'''


def cpu64_with_vector_lanes(cpu64):
    """native/runtime64/cpu64.h with only the XMM union replaced (same include guard, so every
    later #include "cpu64.h" of the unit is satisfied by it)."""
    text = cpu64.replace(chr(13) + chr(10), chr(10))
    begin = text.index('typedef union {' + chr(10) + '    float    f32[4];')
    end = text.index('} XMM;', begin) + len('} XMM;')
    if text.count('} XMM;') != 1 or '#ifndef RECOMP_CPU64_H' not in text:
        raise ValueError('cpu64.h XMM definition changed')
    return ('/* generated by tools/fastlift.py from native/runtime64/cpu64.h - do not edit */' + chr(10)
            + text[:begin] + XMM_LANES + text[end:])


def write_headers(folder):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'fastlift_cpu64.h').write_text(cpu64_with_vector_lanes(
        (ROOT / 'native/runtime64/cpu64.h').read_text(encoding='utf-8')), encoding='utf-8', newline=chr(10))
    (folder / 'fastlift_canonical.h').write_text(
        '/* generated by tools/fastlift.py: original-text region, canonical guest memory accessors */\n'
        + ''.join('#define %s cpu64_%s\n' % (n, n) for n in ACCESSORS)
        + '#define frostpunk_addss cpu64_addss\n#define frostpunk_mulss cpu64_mulss\n', encoding='utf-8', newline='\n')
    (folder / 'fastlift_volatile.h').write_text(
        '/* generated by tools/fastlift.py: end of the original-text region */\n'
        + ''.join('#undef %s\n' % n for n in ACCESSORS) + '#undef frostpunk_addss\n#undef frostpunk_mulss\n',
        encoding='utf-8', newline='\n')
    cpu64 = (ROOT / 'native/runtime64/cpu64.h').read_text(encoding='utf-8')
    (folder / 'fastlift_flags.h').write_text('/* generated by tools/fastlift.py from native/runtime64/cpu64.h - do not edit */\n'
                                             + flag_helpers(cpu64), encoding='utf-8', newline='\n')
    (folder / 'fastlift.h').write_text(FASTLIFT_H, encoding='utf-8', newline='\n')


# ---------------------------------------------------------------------------------------------
# Driver.
def toolchain():
    config = json.loads((ROOT / 'port.json').read_text())
    return Path(config['android_sdk']) / 'ndk' / config['android_ndk_version'] / 'toolchains/llvm/prebuilt/windows-x86_64/bin'


CFLAGS = ['--target=aarch64-linux-android30', '-O2', '-std=c11', '-fPIC', '-fvisibility=hidden', '-fno-strict-aliasing',
          '-fwrapv', '-ffp-contract=off', '-fno-math-errno', '-g0', '-w',
          '-mllvm', '-capture-tracking-max-uses-to-explore=4000000']


def compile_unit(job):
    source, obj, include = job
    clang = toolchain() / 'clang.exe'
    command = [str(clang), *CFLAGS, '-I' + str(include), '-I' + str(ROOT / 'native/runtime64'), '-I' + str(ROOT / 'android/host'),
               '-c', str(source), '-o', str(obj)]
    started = time.monotonic()
    p = subprocess.run(command, capture_output=True, text=True)
    return str(source), p.returncode, p.stderr[-6000:], time.monotonic() - started


def parse_units_arg(text, indices):
    if not text:
        return indices
    chosen = set()
    for part in text.split(','):
        if '-' in part:
            a, b = part.split('-')
            chosen.update(range(int(a), int(b) + 1))
        else:
            chosen.add(int(part))
    return [i for i in indices if i in chosen]


def build(args):
    out = Path(args.out) if args.out else OUT
    src, obj, include = out / 'src', out / 'obj', out / 'include'
    for folder in (src, obj):
        folder.mkdir(parents=True, exist_ok=True)
    write_headers(include)
    started = time.monotonic()
    plan = make_plan(args.jobs, merge=not args.no_merge, keep_special=args.keep_special, elide_eh=not args.keep_eh)
    plan.flags_mode = args.flags
    plan.lt = not args.no_lt
    plan.icall = args.icall
    plan.prefetch = not args.no_prefetch
    indices = sorted(set(plan.unit_of.values()))
    todo = [i for i in indices if i not in plan.special]
    print('plan: %d fragments, %d units (%d kept as existing objects) in %.1fs'
          % (len(plan.unit_of), len(indices), len(plan.special), time.monotonic() - started), flush=True)
    # Flags that cross a direct jump between functions: least fixpoint over all units.
    rounds = 0
    while True:
        rounds += 1
        changed = 0
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs, initializer=_init, initargs=(plan,)) as pool:
            for index, live in pool.map(live_pass, indices, chunksize=4):
                for va, bits in live.items():
                    if index in plan.special and va not in plan.fn_starts:
                        bits = ALL  # an old object entered mid-function: every flag as before
                    if plan.live_in.get(va, 0) | bits != plan.live_in.get(va, 0):
                        plan.live_in[va] = plan.live_in.get(va, 0) | bits
                        changed += 1
        print('liveness round %d: %d entries changed (%.1fs)' % (rounds, changed, time.monotonic() - started), flush=True)
        if not changed:
            break
    nonzero = {va: bits for va, bits in plan.live_in.items() if bits}
    (out / 'live-in.json').write_text(json.dumps({'%012X' % va: bits for va, bits in sorted(nonzero.items())}, indent=0))
    at_function_start = [va for va in nonzero if va in plan.fn_starts and plan.unit_of[va] not in plan.special]
    print('entries with live-in flags: %d (function starts: %d)' % (len(nonzero), len(at_function_start)), flush=True)
    selected = parse_units_arg(args.units, todo)
    totals, reports, notes = collections.Counter(), collections.Counter(), []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs, initializer=_init, initargs=(plan,)) as pool:
        for index, text, stats, report, unit_notes in pool.map(generate_unit, selected, chunksize=2):
            if index == 959:
                text = port_settings_patch(text)
            (src / ('recomp_funcs_%04d.c' % index)).write_text(text, encoding='utf-8', newline='\n')
            totals.update(stats); reports.update(report); notes.extend(unit_notes)
    print('generated %d units in %.1fs: %s %s' % (len(selected), time.monotonic() - started, dict(totals), dict(reports)), flush=True)
    (out / 'notes.txt').write_text('\n'.join(notes) + '\n')
    if args.generate_only:
        return
    jobs = [(src / ('recomp_funcs_%04d.c' % i), obj / ('recomp_funcs_%04d.o' % i), include) for i in selected]
    failed = 0
    slowest = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for n, (source, code, err, seconds) in enumerate(pool.map(compile_unit, jobs), 1):
            slowest.append((seconds, Path(source).name))
            if code:
                failed += 1
                print('FAIL %s\n%s' % (source, err), flush=True)
            elif n % 100 == 0:
                print('compiled %d/%d (%.1fs)' % (n, len(jobs), time.monotonic() - started), flush=True)
    slowest.sort(reverse=True)
    print('compile done: %d failed, slowest %s, total %.1fs' % (failed, slowest[:5], time.monotonic() - started), flush=True)
    if failed:
        raise SystemExit(1)
    manifest = dict(units=selected, special=sorted(plan.special), flags=args.flags, merge=not args.no_merge, lt=not args.no_lt,
                    totals=dict(totals), reports=dict(reports), live_in_entries=len(nonzero))
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')


def link(args):
    """Relink the production runtime with the regenerated unit objects and package the APK."""
    out = Path(args.out) if args.out else OUT
    sys.path.insert(0, str(ROOT / 'tools'))
    sys.path.insert(0, str(ROOT / 'build/perf/renderer-20261112'))
    import fastbuild as fb
    from pack_runtime import package
    standard, patched, rows = special_units()
    replaced = 0
    selected = []
    by_row = {row: index for index, row in standard.items()}
    by_row.update({row: index for index, row in patched.items()})
    for row in rows:
        index = by_row.get(row)
        new = out / 'obj' / ('recomp_funcs_%04d.o' % index) if index is not None else None
        if new is not None and new.exists():
            selected.append(new.as_posix()); replaced += 1
        else:
            selected.append(row)
    extra = [Path(p).as_posix() for p in (args.add or [])]
    drop = set(args.drop or [])
    selected = [row for row in selected if not any(Path(row).name.startswith(d) for d in drop)] + extra
    folder = out / args.name
    folder.mkdir(parents=True, exist_ok=True)
    rsp = folder / 'link.rsp'
    rsp.write_text('\n'.join('"' + p + '"' for p in selected))
    runtime = folder / 'libfrostpunk_runtime.so'
    command = [str(fb.CLANG), '--target=aarch64-linux-android30', '@' + str(rsp), *fb.LINK_FLAGS, '-o', str(runtime)]
    started = time.monotonic()
    p = subprocess.run(command, capture_output=True, text=True)
    if p.returncode:
        print(p.stderr[-6000:])
        raise SystemExit(1)
    print('linked %s with %d regenerated units in %.1fs (%d bytes)' % (runtime, replaced, time.monotonic() - started, runtime.stat().st_size), flush=True)
    apk = folder / ('Frostpunk-%s.apk' % args.name)
    package(runtime, BASE_APK, apk, folder, 'fastlift: ' + args.name)
    print(apk)


def host(args):
    """Compile host sources exactly as tools/fastbuild.py does, for swapping into the link."""
    out = (Path(args.out) if args.out else OUT) / 'host'
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT / 'tools'))
    import fastbuild as fb
    for name in args.names:
        source = ROOT / 'android/host' / (name + '.c')
        command = [str(fb.CLANG), *fb.COMPILE_FLAGS, '-w', *['-I' + str(ROOT / d) for d in fb.INCLUDES], '-c', str(source),
                   '-o', str(out / (name + '.o'))]
        p = subprocess.run(command, capture_output=True, text=True)
        if p.returncode:
            print(p.stderr[-4000:])
            raise SystemExit(1)
        print(out / (name + '.o'))


OPENAL_OBJECT_SHA256 = '8f8badfd8744a05df0851ef9f6a3f5803b7effe29646c0f1bfa31f31503481f1'


def openal(args):
    """Compile android/host/win64_openal.c with the recipe of tools/build_openal_bridge.py into
    host/win64_openal.o, for `link --drop libwin64_openal --add ...`.  --verify first rebuilds the
    pinned source from the given folder and requires the object of the production archive
    (build/openal-bridge/libwin64_openal.a) byte for byte."""
    out = (Path(args.out) if args.out else OUT) / 'host'
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT / 'tools'))
    import fastbuild as fb

    def compile_openal(source, target):
        command = [str(fb.CLANG), '--target=aarch64-linux-android30', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                   '-Wno-misleading-indentation', '-Wno-sign-compare',
                   *['-I' + str(ROOT / d) for d in ('native/runtime64', 'android/host', 'android/audio',
                                                    'reference/openal-soft/source/include', 'build/openal-bridge')],
                   '-fPIC', '-c', str(source), '-o', str(target)]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode:
            print(done.stderr[-4000:])
            raise SystemExit(1)

    if args.verify:
        check = out / 'win64_openal-production-check.o'
        compile_openal(Path(args.verify) / 'win64_openal.c', check)
        digest = hashlib.sha256(check.read_bytes()).hexdigest()
        if digest != OPENAL_OBJECT_SHA256:
            raise SystemExit('the pinned source does not rebuild the production OpenAL bridge object: ' + digest)
        print('production OpenAL bridge object reproduced', digest)
    target = out / 'win64_openal.o'
    compile_openal(ROOT / 'android/host/win64_openal.c', target)
    print(target, hashlib.sha256(target.read_bytes()).hexdigest())


# The production D3D11 bridge archive and the generated COM tables it was compiled against.
BRIDGE_PRODUCTION = ROOT / 'build/perf/native-cpu-20261113/bridge'
BRIDGE_SOURCE_SHA256 = '7e947eaa67479d87bf1b2c4fa92a28aede7d17d687a003b41b326b61eb353bdb'
BRIDGE_OBJECT_SHA256 = '60de317f5bd5fd930fa6e4b9f512fc0389eb26450d6d963963a191acf509aa15'
MESHOPTIMIZER = ROOT / 'reference/meshoptimizer/src'   # v0.25, reference/meshoptimizer.provenance.json


def bridge(args):
    """Compile android/dxvk/bridge.cpp with the recipe of the production archive
    (build/perf/native-cpu-20261113/build_bridge.py) into host/libwin64_d3d11-fastlift.a, for
    `link --drop libwin64_d3d11 --add ...`.  --verify first rebuilds the pinned production
    sources (bridge.cpp and buffer_map_count.h from the given folder) and requires the
    production object byte for byte, which proves the recipe before any change is measured."""
    out = (Path(args.out) if args.out else OUT) / 'host'
    out.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT / 'tools'))
    import fastbuild as fb
    includes = ['android/dxvk', 'reference/dxvk/include/native/windows', 'reference/dxvk/include/native/directx',
                'reference/dxvk/include/vulkan/include', 'native/runtime64', 'android/host', 'android/dxvk', 'android/gpu',
                'reference/adrenotools/include', 'tests', 'build/mali-d3d11-probe']

    def compile_bridge(source, extra, target):
        command = [str(fb.BIN / 'clang++.exe'), '--target=aarch64-linux-android30', '-std=c++17', '-O2', '-fPIC', '-Wall', '-Wextra',
                   '-Wno-extern-c-compat', '-Wno-missing-field-initializers', *extra, *['-I' + str(ROOT / d) for d in includes],
                   '-I' + str(BRIDGE_PRODUCTION), '-c', str(source), '-o', str(target)]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode:
            print(done.stderr[-4000:])
            raise SystemExit(1)

    if args.verify:
        pinned = Path(args.verify)
        check = out / 'bridge-production-check.o'
        # The pinned folder comes first on the include path, so its buffer_map_count.h wins.
        compile_bridge(pinned / 'bridge.cpp', ['-I' + str(pinned)], check)
        digest = hashlib.sha256(check.read_bytes()).hexdigest()
        if hashlib.sha256((pinned / 'bridge.cpp').read_bytes()).hexdigest() != BRIDGE_SOURCE_SHA256 or digest != BRIDGE_OBJECT_SHA256:
            raise SystemExit('the pinned sources do not rebuild the production bridge object: ' + digest)
        print('production bridge object reproduced', digest)
    target = out / 'bridge.o'
    compile_bridge(ROOT / 'android/dxvk/bridge.cpp', ['-I' + str(MESHOPTIMIZER)], target)
    members = [target]
    # The simplifier of the mobile geometry module (android/dxvk/mobile_geometry.inc), MIT licence.
    for name in ('simplifier', 'allocator'):
        member = out / ('meshoptimizer-%s.o' % name)
        command = [str(fb.BIN / 'clang++.exe'), '--target=aarch64-linux-android30', '-std=c++17', '-O2', '-fPIC', '-fvisibility=hidden',
                   '-fno-exceptions', '-fno-rtti', '-w', '-c', str(MESHOPTIMIZER / (name + '.cpp')), '-o', str(member)]
        done = subprocess.run(command, capture_output=True, text=True)
        if done.returncode:
            print(done.stderr[-4000:])
            raise SystemExit(1)
        members.append(member)
    archive = out / 'libwin64_d3d11-fastlift.a'
    if archive.exists():
        archive.unlink()
    subprocess.run([str(fb.BIN / 'llvm-ar.exe'), 'rcs', str(archive), *map(str, members)], check=True)
    print(archive, hashlib.sha256(target.read_bytes()).hexdigest())


def stats(args):
    plan = make_plan(args.jobs, keep_special=True)
    kinds = collections.Counter(plan.kind_of.values())
    regions = collections.Counter()
    for va in plan.unit_of:
        r = plan.region_of(va)
        if r is not None:
            regions[r] += 1
    multi = sum(1 for n in regions.values() if n > 1)
    print('fragments', len(plan.unit_of), dict(kinds), 'special units', sorted(plan.special))
    print('IDA functions', len(plan.fn_starts), 'regions', len(regions), 'with several fragments', multi,
          'fragments in regions', sum(regions.values()))
    print('largest regions', sorted(regions.values(), reverse=True)[:10])


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    p = sub.add_parser('stats'); p.add_argument('--jobs', type=int, default=14)
    p = sub.add_parser('build')
    p.add_argument('--jobs', type=int, default=14)
    p.add_argument('--out')
    p.add_argument('--units', help='only these units, e.g. 793-999 or 5,812')
    p.add_argument('--flags', choices=('exact', 'all'), default='exact',
                   help='all = pass every flag through c at every boundary (diagnostic)')
    p.add_argument('--no-merge', action='store_true', help='one function per fragment (diagnostic)')
    p.add_argument('--no-lt', action='store_true', help='no signed-compare shortcut (diagnostic)')
    p.add_argument('--generate-only', action='store_true')
    p.add_argument('--keep-special', action='store_true', help='leave the patched units as their production objects')
    p.add_argument('--no-prefetch', action='store_true', help='without the software prefetch statements')
    p.add_argument('--keep-eh', action='store_true', help='keep try-functions in their original form with setjmp landing pads')
    p.add_argument('--icall', action='store_true', help='inline lookup-cache fast path for indirect calls')
    p = sub.add_parser('host', help='compile android/host sources with the production flags')
    p.add_argument('--out')
    p.add_argument('names', nargs='+')
    p = sub.add_parser('bridge', help='compile the D3D11 bridge with the production recipe')
    p.add_argument('--out')
    p.add_argument('--verify', help='folder with the pinned production bridge.cpp and buffer_map_count.h to reproduce first')
    p = sub.add_parser('openal', help='compile the OpenAL bridge with the production recipe')
    p.add_argument('--out')
    p.add_argument('--verify', help='folder with the pinned production win64_openal.c to reproduce first')
    p = sub.add_parser('link')
    p.add_argument('--out')
    p.add_argument('--name', required=True)
    p.add_argument('--add', action='append', help='extra object to link')
    p.add_argument('--drop', action='append', help='drop link rows whose file name starts with this')
    args = ap.parse_args()
    {'stats': stats, 'build': build, 'link': link, 'host': host, 'bridge': bridge, 'openal': openal}[args.command](args)
