/* SelectionPanel::Close (1419FA3C0) clears ActiveSubPanel/Idx while its root is still rendered. */
static void check_mobile_selection_close(const CPU *caller){
    unsigned relayout_before=layout_relayout_calls;
    static _Alignas(16) unsigned char storage[8][0x400],manager[0x900],controller[0x138];
    uint64_t nodes[8];for(unsigned i=0;i<8;++i)nodes[i]=(uintptr_t)storage[i];
    const char *names[]={"","EXT_SELECTION_PANEL","ANI","Background","PANEL","BUILDING_SUBPANEL","EXT_BUILDING_HEADER","BODY"};
    const unsigned parents[]={0,0,1,2,2,4,5,5};
    for(unsigned i=0;i<8;++i){
        wr64(nodes[i],GVA(i==0?0x141D7B0B0:i==5?0x141DE0A48:0x141D78A60));
        wr64(nodes[i]+0x1d8,(uintptr_t)names[i]);wr32(nodes[i]+0x1e4,0x7900+i);
        wrss(nodes[i]+0xf0,1);wrss(nodes[i]+0xf4,1);
        wrss(nodes[i]+0x100,331);wrss(nodes[i]+0x104,i==6?225:200);
        wrss(nodes[i]+0x210,331);wrss(nodes[i]+0x214,i==6?225:200);
        if(i){wr64(nodes[i]+0x230,nodes[parents[i]]);uint64_t child=rd64(nodes[parents[i]]+0x220);
            if(child){while(rd64(child+0x240))child=rd64(child+0x240);wr64(child+0x240,nodes[i]);}else wr64(nodes[parents[i]]+0x220,nodes[i]);}
    }
    wrss(nodes[1]+0x100,342);wrss(nodes[1]+0x104,596);wrss(nodes[1]+0x210,342);wrss(nodes[1]+0x214,596);
    wrss(nodes[3]+0x100,360);wrss(nodes[3]+0x104,538);wrss(nodes[3]+0x210,360);wrss(nodes[3]+0x214,538);
    wr64((uintptr_t)manager+0x240,(uintptr_t)controller);wr64((uintptr_t)controller,GVA(0x14212E5D8));
    static uint64_t root_reference;root_reference=nodes[1];wr64((uintptr_t)controller+0xa8,(uintptr_t)&root_reference);
    wr32((uintptr_t)controller+0xd8,UINT32_MAX);
    CPU cpu=*caller;cpu.rcx=(uintptr_t)manager;fp_mobile_panels_created(&cpu,created_original);cpu.rcx=nodes[0];
    scale=200;wr32(nodes[5]+0x1e0,2);fp_mobile_ui_before(&cpu);
    assert(!(rd32(nodes[3]+0x1e0)&2)); /* An uninitialized/blank opening is not a close. */
    wr64((uintptr_t)controller+0xd0,1234);wr32((uintptr_t)controller+0xd8,3);wr32(nodes[5]+0x1e0,0);
    fp_mobile_ui_before(&cpu);assert(rdss(nodes[1]+0x210)==684&&!(rd32(nodes[3]+0x1e0)&2));
    float left=rdss(nodes[1]+0xc0),top=rdss(nodes[1]+0xc4),width=rdss(nodes[3]+0x210);
    wr64((uintptr_t)controller+0xd0,0);wr32((uintptr_t)controller+0xd8,UINT32_MAX);wr32(nodes[5]+0x1e0,2);
    for(unsigned i=0;i<4;++i){
        CPU before=cpu;fp_mobile_ui_before(&cpu);assert(!memcmp(&cpu,&before,sizeof(cpu)));
        assert(rd32(nodes[3]+0x1e0)&2);assert(!(rd32(nodes[1]+0x1e0)&2));
        assert(rdss(nodes[1]+0x210)==684&&rdss(nodes[1]+0xc0)==left&&rdss(nodes[1]+0xc4)==top);
        assert(rdss(nodes[3]+0x210)==width); /* No narrow empty-window reflow during native close. */
    }
    wr32(nodes[1]+0x1e0,2);fp_mobile_ui_before(&cpu);wr32(nodes[1]+0x1e0,0);
    wr64((uintptr_t)controller+0xd0,5678);wr32((uintptr_t)controller+0xd8,4);wr32(nodes[5]+0x1e0,0);
    fp_mobile_ui_before(&cpu);assert(!(rd32(nodes[3]+0x1e0)&2)&&rdss(nodes[1]+0x210)==684);
    wr32(nodes[3]+0x1e0,2);wr64((uintptr_t)controller+0xd0,0);wr32((uintptr_t)controller+0xd8,UINT32_MAX);wr32(nodes[5]+0x1e0,2);
    fp_mobile_ui_before(&cpu);wr64((uintptr_t)controller+0xd0,5678);wr32((uintptr_t)controller+0xd8,4);wr32(nodes[5]+0x1e0,0);
    fp_mobile_ui_before(&cpu);assert(rd32(nodes[3]+0x1e0)&2); /* Native-hidden decoration is never revived. */
    wr32(nodes[3]+0x1e0,0);wr64((uintptr_t)controller+0xd0,0);wr32((uintptr_t)controller+0xd8,UINT32_MAX);wr32(nodes[5]+0x1e0,2);
    scale=100;fp_mobile_ui_before(&cpu);assert(!(rd32(nodes[3]+0x1e0)&2)&&rdss(nodes[1]+0x210)==342);
    layout_relayout_calls=relayout_before;
}
