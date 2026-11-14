"""Keep XMM state local inside the checked, callback-free bounds transform window."""
import hashlib
import re

START='L_000140E2C27E:\n'
END='    SET64(c->rbx, rd64((c->r11 + 0x10ull)));'
WINDOW_SHA256='ec1b96f2f33654b4a1285306bf951ecb5c1881ff78df4327245641d96475333b'


def window(text):
    assert text.count(START)==1
    begin=text.index(START);end=text.index(END,begin)
    assert not re.search(r'^void L_',text[begin:end],re.M)
    region=text[begin:end]
    assert hashlib.sha256(region.encode()).hexdigest()==WINDOW_SHA256,'Bounds window changed'
    assert not re.search(r'\b(?:dispatch|push64|pop64|sse_compare)\(',region)
    # All guest addresses use these four fixed ranges; CPU aliasing takes the original.
    addresses=re.findall(r'\b(?:rdxm|wrxm)\(\((.*?)\)',region)
    assert addresses and all(re.fullmatch(r'c->(?:rsp|rsi|rbx|rdi)(?: \+ 0x[0-9A-Fa-f]+ull)?',a) for a in addresses)
    return region


def replacement(text):
    original=window(text)
    lines=original.splitlines(keepends=True);head=''.join(lines[:2]);region=''.join(lines[2:])
    registers=sorted({int(i) for i in re.findall(r'c->xmm\[(\d+)\]',region)})
    init=''.join('        XMM fp_xmm%d=c->xmm[%d];\n'%(i,i) for i in registers)
    local=re.sub(r'c->xmm\[(\d+)\]',lambda m:'fp_xmm'+m[1],region)
    publish=''.join('        c->xmm[%d]=fp_xmm%d;\n'%(i,i) for i in registers)
    guard='    /* shortcut: retain the original window for non-finite inputs and stack/data aliases. */\n'
    guard+='    if(fp_bounds_separate(c->rsp,144,c) && fp_bounds_separate(c->rsi,32,c) &&\n'
    guard+='       fp_bounds_separate(c->rbx,32,c) && fp_bounds_separate(c->rdi,64,c) &&\n'
    guard+='       fp_bounds_disjoint(c->rsp,144,c->rsi,32) && fp_bounds_disjoint(c->rsp,144,c->rbx,32) &&\n'
    guard+='       fp_bounds_disjoint(c->rsp,144,c->rdi,64) && fp_bounds_finite(c->rsi,8) &&\n'
    guard+='       fp_bounds_finite(c->rbx,8) && fp_bounds_finite(c->rdi,16)) {\n'
    return head+guard+init+local+publish+'    } else {\n'+region+'    }\n'


def apply(text):
    return '#include "bounds_register_locals.h"\n'+text.replace(window(text),replacement(text),1)


def fixture(text):
    region=window(text)
    return '#include "bounds_register_locals.h"\nvoid bounds_reference(CPU*c){\n'+region+'}\n'+\
           'void bounds_candidate(CPU*c){\n'+replacement(text)+'}\n'
