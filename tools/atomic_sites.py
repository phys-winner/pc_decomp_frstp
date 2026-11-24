"""x86 `lock` read-modify-writes and `xchg [mem]` in the compiled lifted trees: inventory, byte-proven
rewrite to the atomic helpers of native/runtime64/cpu64.h, and a build guard.

    python -X utf8 tools/atomic_sites.py inventory [--json research/atomic-sites.json]
    python -X utf8 tools/atomic_sites.py check [TREE ...]      # exit 1 while a plain lowering remains
    python -X utf8 tools/atomic_sites.py overlay --out DIR     # byte-proven copies of the side trees

The generated trees lowered `lock inc dword ptr [rcx + 8]` as `wr32(a, flags_incs(c, rd32(a), 4))` and
`xchg dword ptr [rbx + 0x4b0], eax` as a load and a store: not atomic and no barrier.  A rewrite is
proven per line: the instruction decoded from the original image at the address of the line's comment
must print exactly as the comment, the old code must equal what the unmodified lift64 emits for those
bytes, and the replacement is what tools/lift64_checked.py now emits (CheckedLifter.locked), so a fresh
lift and a repaired tree agree.  The main executable is repaired inside tools/fastlift.py (unit_text);
the side trees (payload, startup, supplemental, msvcp140) through `overlay`.
"""
import argparse
import bisect
import collections
import functools
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'reference/pcrecomp/tools/lift'))

MAIN_SHA256 = '45d6a087515fab5e5fc55e673699b04f4d207da09958fd274ffe7467d5a09bdc'
# Each compiled tree: the C files the Android/Windows builds compile and the image they were lifted from.
TREES = {
    'fastlift': ('build/fastlift/src/recomp_funcs_*.c', 'main'),
    'canonical': ('generated/frostpunk-x64/recomp_funcs_*.c', 'main'),
    'android-overlay': ('build/arm64/generated-overlay/recomp_funcs_*.c', 'main'),
    'payload': ('generated/payload/recomp/recomp_funcs_*.c', 'payload'),
    'startup': ('generated/startup/recomp_funcs.c', 'main'),
    'supplemental': ('generated/supplemental/recomp_supplemental.c', 'main'),
    'msvcp140': ('generated/dependencies/msvcp140/recomp/recomp_funcs_*.c', 'msvcp140'),
}
IMAGES = {
    'payload': ('generated/payload/input/SteamDRMP.dll', 'a74f148390a95c6c07446341bd901add520e18b221733ff22e111602d1044112'),
    'msvcp140': ('generated/dependencies/msvcp140/input/msvcp140.static.dll', '538c43b4dc37bc5a4f1795b40d21952aee4ff0ded75d8765c91e8fcd7877e9d8'),
}
LINE = re.compile(r'^(\s*)(.*?)\s*/\* ([0-9A-F]{12}): ((?:lock |xchg)[^*]*?) \*/\s*$')
ATOMIC = re.compile(r'\b(?:lock_(?:add|sub|or|and|xor|xchg|neg|not)(?:8|16|32|64)|atomic_(?:xadd|cmpxchg|xchg)(?:32|64)|guest_cmpxchg16b)\(')
FENCE = '__atomic_thread_fence(__ATOMIC_SEQ_CST)'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


@functools.lru_cache(maxsize=None)
def image(name):
    """(pefile.PE, path) of the image a tree was lifted from, hash-checked."""
    import pefile
    if name == 'main':
        config = json.loads((ROOT / 'port.json').read_text())
        path = Path(config['game_directory']) / 'Frostpunk.exe'
        expected = MAIN_SHA256
    else:
        path, expected = ROOT / IMAGES[name][0], IMAGES[name][1]
    if sha(path) != expected:
        raise ValueError('%s: image hash changed (%s)' % (name, path))
    return pefile.PE(str(path), fast_load=True), path


@functools.lru_cache(maxsize=None)
def lifters(name):
    """(base lift64 Lifter, CheckedLifter, capstone decoder) for one image."""
    import capstone
    from lift64_cpu import Lifter
    from lift64_checked import CheckedLifter
    pe, _ = image(name)
    size, base = pe.OPTIONAL_HEADER.SizeOfImage, pe.OPTIONAL_HEADER.ImageBase
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    return Lifter(image_size=size, image_base=base), CheckedLifter(image_size=size, image_base=base), decoder


def decode(name, va):
    pe, _ = image(name)
    _, _, decoder = lifters(name)
    data = pe.get_data(va - pe.OPTIONAL_HEADER.ImageBase, 15)
    return next(decoder.disasm(data, va))


def memory_operand(asm):
    return 'ptr [' in asm


def classify(asm):
    """Form of one lock/xchg instruction from its printed text."""
    mnemonic = asm.split(' ptr')[0].split()
    if asm.startswith('xchg ') or asm.startswith('lock xchg '):
        return 'xchg-mem' if memory_operand(asm) else 'xchg-reg'
    op = mnemonic[1] if len(mnemonic) > 1 else ''
    if op in ('cmpxchg', 'xadd', 'cmpxchg16b', 'cmpxchg8b'):
        return 'interlocked'
    if re.match(r'^lock or dword ptr \[rsp\], 0$', asm):
        return 'stack-fence'
    if re.match(r'^lock or dword ptr \[rsp\], \w+$', asm):
        return 'stack-fence-reg'
    return 'rmw'


def status(form, code):
    if form == 'xchg-reg':
        return 'register'
    if form == 'stack-fence':
        return 'fenced' if FENCE in code else 'plain'
    return 'atomic' if ATOMIC.search(code) else 'plain'


@functools.lru_cache(maxsize=None)
def ida_functions():
    path = ROOT / 'research/ida/functions.json'
    rows = json.loads(path.read_text())
    chunks = sorted((lo, hi, start) for start, _, parts in rows for lo, hi in parts)
    return chunks, [c[0] for c in chunks]


def owner(va):
    """Start of the IDA function whose chunk covers va (main executable), else None."""
    chunks, starts = ida_functions()
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0 and chunks[i][0] <= va < chunks[i][1]:
        return chunks[i][2]
    return None


def scan(tree):
    pattern, image_name = TREES[tree]
    sites = []
    for path in sorted(ROOT.glob(pattern)):
        # One instruction can span several consecutive lines with the same comment (lock xadd/cmpxchg).
        # fastlift keeps the comment on the first line only: an unbalanced block takes the next lines.
        groups = []
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        for number, line in enumerate(lines, 1):
            m = LINE.match(line)
            if not m:
                continue
            if groups and groups[-1][2] == m[3] and groups[-1][1] + groups[-1][4] == number:
                groups[-1][3].append(m[2])
                groups[-1][4] += 1
                continue
            groups.append([m[4], number, m[3], [m[2]], 1])
            depth, extra = m[2].count('{') - m[2].count('}'), number
            while depth > 0 and extra < len(lines) and extra - number < 8 and not LINE.match(lines[extra]):
                groups[-1][3].append(lines[extra])
                depth += lines[extra].count('{') - lines[extra].count('}')
                extra += 1
        for asm, number, va_text, codes, _ in groups:
            form = classify(asm)
            if form == 'xchg-reg':
                continue
            va = int(va_text, 16)
            row = dict(tree=tree, file=str(path.relative_to(ROOT)).replace('\\', '/'), line=number, va='0x%X' % va,
                       asm=asm, form=form, status=status(form, ' '.join(codes)))
            if image_name == 'main':
                fn = owner(va)
                row['function'] = 'L_%012X' % fn if fn else None
                row['kind'] = ('stack-fence' if form.startswith('stack-fence') else
                               'data-as-code' if fn is None else 'shared-memory')
            else:
                disp = re.search(r'([+-]) 0x([0-9a-f]+)\]', asm)
                odd = disp and int(disp[2], 16) >= 0x1000000
                row['kind'] = 'stack-fence' if form.startswith('stack-fence') else ('data-as-code' if odd else 'shared-memory')
            sites.append(row)
    return sites


def rewrite_text(text, image_name, receipt=None):
    """Replace every plain lock/xchg-mem lowering of a C unit by the atomic one, proven per line.
    Returns the new text; `receipt` (a list) collects one row per changed line."""
    base, checked, _ = lifters(image_name)
    out, changed = [], 0
    for line in text.split('\n'):
        m = LINE.match(line) if (': lock ' in line or ': xchg ' in line) else None
        if not m:
            out.append(line)
            continue
        indent, code, va_text, asm = m[1], m[2], m[3], m[4]
        form = classify(asm)
        fenced_before = bool(out) and FENCE in out[-1] and out[-1].rstrip().endswith('/* %s: %s */' % (va_text, asm))
        if form in ('xchg-reg', 'interlocked') or status(form, code) != 'plain' or fenced_before:
            out.append(line)
            continue
        va = int(va_text, 16)
        insn = decode(image_name, va)
        printed = insn.mnemonic + (' ' + insn.op_str if insn.op_str else '')
        if printed != asm:
            raise ValueError('%s: decoded "%s" but the comment says "%s"' % (va_text, printed, asm))
        plain = base.translate(insn, set())
        new = checked.locked(insn)
        if new is None:
            raise ValueError('%s: %s has no atomic lowering' % (va_text, asm))
        comment = '/* %s: %s */' % (va_text, asm)
        if form == 'stack-fence':
            # The generated tree kept lift64's statement; the fence goes in front of it.
            if [code] != plain or new[1:] != plain:
                raise ValueError('%s: unexpected fence lowering %r' % (va_text, code))
        elif [code] != plain:
            raise ValueError('%s: old code is not the plain lift64 lowering:\n  %s\n  %s' % (va_text, code, plain))
        for statement in new:
            out.append('%s%s %s' % (indent, statement, comment))
        changed += 1
        if receipt is not None:
            receipt.append(dict(va='0x%s' % va_text, bytes=bytes(insn.bytes).hex(), instruction=asm, form=form,
                                old_code=code, new_code=new))
    return '\n'.join(out), changed


def inventory(trees):
    rows = []
    for tree in trees:
        rows += scan(tree)
    return rows


def summary(rows):
    table = collections.defaultdict(collections.Counter)
    for r in rows:
        table[r['tree']]['%s/%s' % (r['form'], r['status'])] += 1
    return {tree: dict(sorted(c.items())) for tree, c in table.items()}


def check(trees):
    """Guard: any plain (non-atomic, unfenced) lowering of a lock/xchg-mem instruction fails."""
    bad = [r for r in inventory(trees) if r['status'] == 'plain']
    for r in bad[:20]:
        print('PLAIN %s %s:%d %s %s' % (r['tree'], r['file'], r['line'], r['va'], r['asm']))
    return bad


OVERLAY_TREES = ('payload', 'startup', 'supplemental', 'msvcp140')


def overlay(out):
    """Byte-proven copies of the side trees with the atomic lowering, plus a receipt."""
    out = Path(out).resolve()
    manifest = dict(tool=str(Path(__file__).relative_to(ROOT)).replace('\\', '/'), tool_sha256=sha(__file__),
                    lifter_sha256=sha(ROOT / 'tools/lift64_checked.py'), cpu64_sha256=sha(ROOT / 'native/runtime64/cpu64.h'),
                    images={}, units=[])
    for tree in OVERLAY_TREES:
        pattern, image_name = TREES[tree]
        manifest['images'][image_name] = sha(image(image_name)[1])
        for path in sorted(ROOT.glob(pattern)):
            raw = path.read_bytes().decode('utf-8')
            crlf = '\r\n' in raw   # keep the tree's line endings, so a fresh lift and the copy are byte-identical
            receipt = []
            new, changed = rewrite_text(raw.replace('\r\n', '\n'), image_name, receipt)
            if not changed:
                continue
            target = out / path.relative_to(ROOT)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((new.replace('\n', '\r\n') if crlf else new).encode('utf-8'))
            manifest['units'].append(dict(tree=tree, source=str(path.relative_to(ROOT)).replace('\\', '/'), source_sha256=sha(path),
                                          overlay=str(target.relative_to(ROOT)).replace('\\', '/'), overlay_sha256=sha(target),
                                          sites=receipt))
    (out / 'atomic-overlay.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    inv = sub.add_parser('inventory')
    inv.add_argument('--json', type=Path)
    inv.add_argument('trees', nargs='*', default=list(TREES))
    chk = sub.add_parser('check')
    chk.add_argument('trees', nargs='*', default=['fastlift', 'payload', 'startup', 'supplemental', 'msvcp140'])
    ov = sub.add_parser('overlay')
    ov.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    if args.command == 'inventory':
        rows = inventory(args.trees)
        print(json.dumps(summary(rows), indent=1))
        if args.json:
            args.json.write_text(json.dumps(dict(summary=summary(rows), sites=rows), indent=1) + '\n', encoding='utf-8')
    elif args.command == 'check':
        bad = check(args.trees)
        print('atomic_sites check: %d plain lock/xchg lowerings in %s' % (len(bad), ', '.join(args.trees)))
        sys.exit(1 if bad else 0)
    else:
        manifest = overlay(args.out)
        print('overlay units %d, sites %d -> %s' % (len(manifest['units']), sum(len(u['sites']) for u in manifest['units']), args.out))


if __name__ == '__main__':
    main()
