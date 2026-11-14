/* Exercise production snapshot_ui against stale editor hit rectangles. */
static void check_mobile_editor_hits(CPU *cpu){
    CPU caller=*cpu;
    _Alignas(16) unsigned char screen[0x380]={0},other_screen[0x380]={0},parent[0x280]={0};
    _Alignas(16) unsigned char editor[0x720]={0},replacement[0x720]={0},anchor[0x280]={0},hit[0x50]={0};
    uint64_t registry[6]={(uintptr_t)editor,0,(uintptr_t)anchor,0,0,0};
    uint64_t previous_list=rd64(GVA(0x142A89088));uint32_t previous_count=rd32(GVA(0x142A89090));
    uint8_t previous_loading=rd8(GVA(0x142982F8D));wr8(GVA(0x142982F8D),0);
    wr64((uintptr_t)screen,GVA(0x141D7B0B0));wr64((uintptr_t)other_screen,GVA(0x141D7B0B0));
    wr64((uintptr_t)screen+0x220,(uintptr_t)editor);wr64((uintptr_t)screen+0x2f0,(uintptr_t)hit);wr32((uintptr_t)screen+0x2f8,1);
    wr64((uintptr_t)editor,GVA(0x141DAC248));wr64((uintptr_t)editor+0x1d8,(uintptr_t)"NameInput");
    wr32((uintptr_t)editor+0x1e4,0x7ead1234);wr64((uintptr_t)editor+0x230,(uintptr_t)parent);
    wr64((uintptr_t)editor+0x248,(uintptr_t)screen);wr64((uintptr_t)parent+0x230,(uintptr_t)screen);
    wrss((uintptr_t)editor+0x100,200);wrss((uintptr_t)editor+0x104,40);wrss((uintptr_t)editor+0xf0,1);wrss((uintptr_t)editor+0xf4,1);
    wr64((uintptr_t)anchor+0x248,(uintptr_t)screen);wr64(GVA(0x142A89088),(uintptr_t)registry);wr32(GVA(0x142A89090),2);
    wr32((uintptr_t)hit+0x40,0x7ead1234);wr32((uintptr_t)hit+0x44,0x300211);
    for(unsigned v=0;v<4;++v){wrss((uintptr_t)hit+v*16,(v&1)?.9f:.5f);wrss((uintptr_t)hit+v*16+4,(v&2)?-.1f:.2f);}
    CPU c=*cpu;c.rcx=(uintptr_t)screen;
    fp_mobile_ui_before(&c);assert((frostpunk_mobile_context(.8f,.5f)&32770)==32770);
    assert(!rd8((uintptr_t)editor+0x6c8)); /* A visible field is hittable before editing begins. */
    /* Real SAVE_NAME_INPUT leaves +248 zero. Its UIScreen ancestor owns the
     * rectangle, even when no other registered control exposes that owner. */
    wr64((uintptr_t)editor+0x248,0);wr64((uintptr_t)anchor+0x248,0);wr32(GVA(0x142A89090),1);
    fp_mobile_ui_before(&c);assert((frostpunk_mobile_context(.8f,.5f)&32770)==32770);
    wr64((uintptr_t)parent+0x248,(uintptr_t)screen);
    fp_mobile_ui_before(&c);assert((frostpunk_mobile_context(.8f,.5f)&32770)==32770);
    wr64((uintptr_t)parent+0x248,0);wr64((uintptr_t)anchor+0x248,(uintptr_t)screen);wr32(GVA(0x142A89090),2);
    wr32((uintptr_t)parent+0x1e0,2);fp_mobile_ui_before(&c);
    assert(!(frostpunk_mobile_context(.8f,.5f)&32770)); /* Entire hidden rectangle must disappear. */
    wr32((uintptr_t)parent+0x1e0,0);wr32((uintptr_t)editor+0x1e0,2);fp_mobile_ui_before(&c);
    assert(!(frostpunk_mobile_context(.8f,.5f)&32770));
    wr32((uintptr_t)editor+0x1e0,0);wr64((uintptr_t)editor+0x248,(uintptr_t)screen);
    wr64((uintptr_t)parent+0x230,(uintptr_t)editor);fp_mobile_ui_before(&c);
    assert(!(frostpunk_mobile_context(.8f,.5f)&32770)); /* Bounded visibility walk rejects a cycle. */
    wr64((uintptr_t)parent+0x230,(uintptr_t)screen);wr64((uintptr_t)editor+0x248,(uintptr_t)other_screen);fp_mobile_ui_before(&c);
    assert(!(frostpunk_mobile_context(.8f,.5f)&32768)); /* A matching ID in another screen is not an editor hit. */
    wr64((uintptr_t)editor+0x248,0);wr64((uintptr_t)parent+0x248,(uintptr_t)other_screen);fp_mobile_ui_before(&c);
    assert(!(frostpunk_mobile_context(.8f,.5f)&32768)); /* An ancestor's explicit other-screen owner is authoritative. */
    wr64((uintptr_t)parent+0x248,0);
    wr64((uintptr_t)editor+0x248,(uintptr_t)screen);wr32((uintptr_t)parent+0x1e0,2);
    memcpy(replacement,editor,sizeof(editor));wr64((uintptr_t)replacement+0x230,(uintptr_t)screen);
    registry[4]=(uintptr_t)replacement;wr32(GVA(0x142A89090),3);fp_mobile_ui_before(&c);
    assert((frostpunk_mobile_context(.8f,.5f)&32770)==32770); /* Current visible ID wins over an old hidden record. */
    wr64(GVA(0x142A89088),previous_list);wr32(GVA(0x142A89090),previous_count);wr8(GVA(0x142982F8D),previous_loading);
    assert(!memcmp(cpu,&caller,sizeof(caller)));
}
