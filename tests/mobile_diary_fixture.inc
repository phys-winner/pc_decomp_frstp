static void check_mobile_diary(CPU *caller){
    /* Scout diary entries (DIARY_TEXT in a scroll pane under EXT_DIARY_DISPLAY) are enlarged; the same
     * name elsewhere keeps its authored size. */
    static _Alignas(16) unsigned char storage[8][0x600];
    uint64_t n[8];for(unsigned i=0;i<8;++i)n[i]=(uintptr_t)storage[i];
    /* a full-size EXT_ panel is an original dialog, so the fitter walks its contents */
    const char *names[]={"","EXT_DIARY_TEST_PANEL","EXT_DIARY_DISPLAY","layout","SCROLL_PANE","TEXTS_LAYOUT","DIARY_TEXT","DIARY_TEXT"};
    const unsigned parents[]={0,0,1,2,3,4,5,1};
    const uint64_t types[]={0x141D7B0B0,0x141D78A60,0x141EF17C0,0x141DE0A48,0x141DA74C8,0x141DE0A48,0x141DED7D8,0x141DED7D8};
    for(unsigned i=0;i<8;++i){
        wr64(n[i],GVA(types[i]));wr64(n[i]+0x1d8,(uintptr_t)names[i]);wr32(n[i]+0x1e4,0x13000+i);
        wrss(n[i]+0xf0,1);wrss(n[i]+0xf4,1);float w=i==1?1280:303,h=i==1?720:39;wrss(n[i]+0x100,w);wrss(n[i]+0x104,h);wrss(n[i]+0x210,w);wrss(n[i]+0x214,h);
        if(i){uint64_t parent=n[parents[i]],tail=rd64(parent+0x220);wr64(n[i]+0x230,parent);
            if(!tail)wr64(parent+0x220,n[i]);else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,n[i]);}}
    }
    wrss(n[6]+0x3a0,12);wrss(n[7]+0x3a0,12);wrss(n[6]+0x37c,303);wrss(n[7]+0x37c,303);
    CPU cpu=*caller;cpu.rcx=n[0];
    scale=200;fp_mobile_ui_before(&cpu);
    assert(rdss(n[6]+0x3a0)==20&&rdss(n[7]+0x3a0)==12);
    scale=100;fp_mobile_ui_before(&cpu);
    assert(rdss(n[6]+0x3a0)==12&&rdss(n[7]+0x3a0)==12);
}
