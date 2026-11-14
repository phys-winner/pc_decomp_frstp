static void check_switch_panels(CPU *cpu){
    static _Alignas(16) unsigned char storage[9][0x600];uint64_t n[9];
    const char *names[]={"","EXT_ECONOMY_PANEL","PLACES_LIST_PANEL","Table_mask","SCROLL_PANE","CONTENT_LAYOUT","PLACE_ROW_ELEMENT","row_background","EXT_NOTIFICATION_PANEL"};
    const unsigned parents[]={0,0,1,2,3,4,5,6,0};
    const float widths[]={640,1280,640,566,540,607,607,607,410},heights[]={360,720,447,446,348,41,41,41,400};
    for(unsigned i=0;i<9;++i){
        n[i]=(uintptr_t)storage[i];wr64(n[i],GVA(i==4?0x141DA74C8:0x141D78A60));
        wr64(n[i]+0x1d8,(uintptr_t)names[i]);wr32(n[i]+0x1e4,29000+i);
        wrss(n[i]+0xf0,1);wrss(n[i]+0xf4,1);wrss(n[i]+0x100,widths[i]);wrss(n[i]+0x210,widths[i]);
        wrss(n[i]+0x104,heights[i]);wrss(n[i]+0x214,heights[i]);
        if(i){uint64_t parent=n[parents[i]],last=rd64(parent+0x220);wr64(n[i]+0x230,parent);
            if(!last)wr64(parent+0x220,n[i]);else{while(rd64(last+0x240))last=rd64(last+0x240);wr64(last+0x240,n[i]);}}
    }
    wrss(n[8]+0xc0,9);wrss(n[8]+0xc4,53);wr32(n[8]+0x13c,3);
    uint64_t saved=cpu->rcx;cpu->rcx=n[0];scale=200;switch_active=0;fp_mobile_ui_before(cpu);
    assert(rdss(n[2]+0xf0)==1&&rdss(n[4]+0x100)==540&&rdss(n[8]+0xc4)==53);
    static _Alignas(16) unsigned char extra[5][0x600];uint64_t e[5];
    const char *extra_names[]={"tabs_clip","SWITCH_SUBPANEL_BUTTONS_LAYOUT","ROTATED_CONTAINER","ROTATED_CHILD","SCROLL_BAR"};
    for(unsigned i=0;i<5;++i){
        e[i]=(uintptr_t)extra[i];wr64(e[i],GVA(0x141D78A60));wr64(e[i]+0x1d8,(uintptr_t)extra_names[i]);wr32(e[i]+0x1e4,29100+i);
        float sx=(i==0||i==2)?1.2f:1,w=(i==0||i==2)?174:64,h=(i==0||i==2)?496:64;
        wrss(e[i]+0xf0,sx);wrss(e[i]+0xf4,sx);wrss(e[i]+0x100,w);wrss(e[i]+0x210,w);wrss(e[i]+0x104,h);wrss(e[i]+0x214,h);
    }
    wr64(e[0]+0x230,n[1]);wr64(n[2]+0x240,e[0]);wr64(e[0]+0x220,e[1]);wr64(e[1]+0x230,e[0]);
    wrss(e[0]+0xc0,406);wrss(e[0]+0xc4,290);wrss(e[0]+0xe8,-90);
    wr64(e[2]+0x230,n[0]);wr64(n[8]+0x240,e[2]);wr64(e[2]+0x220,e[3]);wr64(e[3]+0x230,e[2]);
    wrss(e[2]+0xc0,120);wrss(e[2]+0xc4,120);wrss(e[2]+0xe8,-90);
    wr64(e[4]+0x230,n[2]);wr64(n[3]+0x240,e[4]);wrss(e[4]+0xc0,545);wrss(e[4]+0xc4,44);
    switch_active=1;fp_mobile_ui_before(cpu);
    assert(rdss(n[1]+0xf0)==.5f&&rdss(n[1]+0xc4)==0);
    assert(fabsf(rdss(n[2]+0xf0)-540.f/607)<.0001f&&rdss(n[2]+0xf0)==rdss(n[2]+0xf4));
    assert(rdss(n[4]+0x100)==607&&rdss(n[3]+0x100)==633&&rdss(n[6]+0x104)==56);
    assert(rdss(n[8]+0xf0)==1&&rdss(n[8]+0xf4)==1&&rdss(n[8]+0xc4)==53);
    assert(rdss(e[0]+0xc0)==406&&rdss(e[0]+0xc4)==290&&rdss(e[0]+0xf0)==1.2f&&rdss(e[1]+0xc0)==0&&rdss(e[1]+0xf0)==1);
    assert(rdss(e[2]+0xc0)==120&&rdss(e[2]+0xc4)==120&&rdss(e[2]+0xf0)==1.2f&&rd32(e[2]+0x13c)==0);
    assert(rdss(e[4]+0xc0)==612&&rdss(e[4]+0xc4)==44);
    fp_mobile_ui_before(cpu);
    assert(rdss(n[4]+0x100)==607&&rdss(n[3]+0x100)==633&&fabsf(rdss(n[2]+0xf0)-540.f/607)<.0001f);
    switch_active=0;cpu->rcx=saved;
}
