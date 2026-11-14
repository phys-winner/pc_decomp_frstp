"""Targeted translated entries; preserve the unit's existing EH, direct calls and CPU optimizations."""
def texture(text):
    anchor='    wr32((c->rsp + 0x6Cull), (uint32_t)(R32(c->rcx)));           /* 000140F60B3C:'
    assert text.count(anchor)==1,'Texture mip-selection entry changed'
    line='    SET32(c->rcx,fp_switch_texture_skip((const char*)(uintptr_t)rd64(c->r15+8),(const uint32_t*)(uintptr_t)(c->rbp+0x20),R32(c->rcx)));\n'
    return '#include "switch_performance.h"\n'+text.replace(anchor,line+anchor,1)

def gfx(text):
    anchor='    wr8((c->rbx + 0x71ull), (uint8_t)(R8L(c->rax)));             /* 000140E8190D: mov byte ptr [rbx + 0x71], al */'
    assert text.count(anchor)==1,'Renderer transaction entry changed'
    return '#include "switch_performance.h"\n'+text.replace(anchor,anchor+'\n    fp_switch_gfx_config(c->rbx);\n    SET8L(c->rax,rd8(c->rbx+0x71));',1)

def port(text):
    anchor='        fp_port_setting_set(R32(c->rbx)+1u,rd32(c->rsp+0x50u));'
    assert text.count(anchor)==1 and 'fp_mobile_changed' not in text
    return text.replace(anchor,'        uint32_t fp_mobile_changed=R32(c->rbx)==21u&&rd32(c->rsp+0x50u)<=1u&&fp_port_setting_get(22u)!=rd32(c->rsp+0x50u);\n'+anchor+'\n        if(fp_mobile_changed){wr8(c->rdi+0x2cu,1);wr8(c->rsi,1);}',1)
