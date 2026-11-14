"""Use the mobile screen-size threshold only for the entity renderer's visibility query."""
def apply(text):
    anchors=[
        'void L_000140F8F530(CPU *c)\n{',
        'sse_load_ss(&c->xmm[7], rdss(GVA(0x141D83E94)));',
        'flags_sub(c, rd8((c->r14 + 0x44ull)), 0x0u, 1);']
    assert all(text.count(a)==1 for a in anchors),'Visibility detail anchors changed'
    text=text.replace(anchors[0],anchors[0]+'\n    unsigned fp_detail_mode=fp_test_detail_mode(c);',1)
    text=text.replace(anchors[1],'sse_load_ss(&c->xmm[7], fp_test_detail_threshold(rdss(GVA(0x141D83E94)),fp_detail_mode));',1)
    text=text.replace(anchors[2],'uint8_t fp_detail_gate=rd8((c->r14 + 0x44ull));\n    flags_sub(c, fp_detail_gate?fp_detail_gate:(fp_detail_mode?1u:0u), 0x0u, 1);',1)
    return '#include "mobile_detail.h"\n'+text
