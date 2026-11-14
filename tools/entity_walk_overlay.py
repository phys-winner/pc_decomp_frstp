"""Fuse pure renderer updates; preserve the guest stack and every dirty/unknown call."""

def entry(text,address):
    start=text.index('void L_'+address+'(CPU *c)\n{')
    return text[start:text.index('\n}\n',start)+3]

def apply(text,transforms,visible):
    assert 'fp_walk_transform' not in text,'Entity walk overlay already applied'
    transform=entry(transforms,'000140F87380')
    epilogue=entry(transforms,'000140F873EB')
    body=epilogue[epilogue.index('{')+1:epilogue.rindex('}')]
    transform=transform.replace('void L_000140F87380(', 'static __attribute__((always_inline)) inline void fp_walk_transform(')
    anchor='fp_dispatch_main(c, 0x140F873EBull, L_000140F873EB); return;'
    assert transform.count(anchor)==1,'Transform return route changed'
    transform=transform.replace(anchor,body)
    visible=entry(visible,'000140F85900').replace('void L_000140F85900(', 'static __attribute__((always_inline)) inline void fp_walk_visible(')
    anchor='default: dispatch_jmp(c, _ind); return;'
    assert visible.count(anchor)==1,'Visible update route changed'
    visible=visible.replace(anchor,'default: if (_ind==GVA(0x140D864D0)) { c->rip=0x140D864D0ull; c->rsp+=8; return; } dispatch_jmp(c, _ind); return;')
    anchor='{ uint64_t _ct = rd64((c->rax + 0xF8ull)); push64(c, 0x140EB4654ull); dispatch(c, _ct); }'
    assert text.count(anchor)==1,'Entity update callsite changed'
    text=text.replace(anchor,'{ uint64_t _ct = rd64((c->rax + 0xF8ull)); push64(c, 0x140EB4654ull); if (_ct==GVA(0x140F87380)) fp_walk_transform(c); else dispatch(c, _ct); }')
    anchor='fp_dispatch_main(c, 0x140F85900ull, L_000140F85900);'
    assert text.count(anchor)==1,'Visible update callsite changed'
    text=text.replace(anchor,'fp_walk_visible(c);')
    anchor='void L_000140EB45F0(CPU *c)'
    assert text.count(anchor)==1,'Entity entry changed'
    return text.replace(anchor,transform+'\n'+visible+'\n'+anchor)
