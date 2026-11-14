"""Replace only the verified integer visibility prefix in build copies of unit 0822."""
import hashlib
import re

START = 'L_000140F8F650:\n'
END = '    flags_logicz(c, R8L(c->r13) & R8L(c->r13), 1);'
PREFIX_SHA256 = 'a4bf166b9ef174bc1f4d576227406b84f32b0f0cf0d177e2846034288a2e73c2'


def prefix(text):
    assert text.count(START) == text.count(END) == 1
    body = text[text.index(START):text.index(END)]
    assert hashlib.sha256(body.encode()).hexdigest() == PREFIX_SHA256, 'Visibility prefix changed'
    return body


def replace_prefix(text):
    original = prefix(text)
    checkpoint = 2
    fallback = []
    for line in original[len(START):].splitlines(keepends=True):
        if re.search(r'\b(?:rd8|rd32|rd64|wr64)\(', line):
            fallback.append('FP_visibility_checkpoint_%d:\n' % checkpoint)
            checkpoint += 1
        fallback.append(line)
    assert checkpoint == 17
    switch = START + '    switch(fp_visibility_filter(c)) {\n'
    switch += '    case 0: goto L_000140F8F87F;\n    case 1: goto FP_visibility_done;\n'
    switch += ''.join('    case %d: goto FP_visibility_checkpoint_%d;\n' % (i, i) for i in range(2, 17))
    switch += '    default: abort();\n    }\n'
    result = switch + ''.join(fallback) + 'FP_visibility_done:\n'
    return text.replace(original, result, 1)


def apply(text):
    assert 'fp_visibility_filter' not in text
    text=replace_prefix(text)
    text=text.replace('switch(fp_visibility_filter(c))','switch(fp_visibility_filter_batch(c))',1)
    checkpoint='    case 16: goto FP_visibility_checkpoint_16;\n'
    backedge='    if (!c->zf) goto L_000140F8F650;                             /* 000140F8F88D: jne 0x140f8f650 */\n'
    assert text.count(checkpoint)==text.count(backedge)==1
    text=text.replace(checkpoint,checkpoint+'    case 17: goto FP_visibility_block_done;\n',1)
    text=text.replace(backedge,backedge+'FP_visibility_block_done:\n',1)
    return '#include "visibility_loop_native.h"\n' + text


def fixture(text):
    body = prefix(text)
    original = 'int filter_reference(CPU*c){\n' + body + 'return 1;\nL_000140F8F87F:return 0;\n}\n'
    candidate = 'int filter_candidate(CPU*c){\n' + replace_prefix(body + END)[:-len(END)]
    candidate += 'return 1;\nL_000140F8F87F:return 0;\n}\n'
    return '#include "visibility_filter_native.h"\n' + original + candidate
