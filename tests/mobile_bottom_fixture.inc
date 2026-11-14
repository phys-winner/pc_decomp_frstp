static unsigned bottom_expected_dock,bottom_native_calls;
static float bottom_expected_y;
static void bottom_position_original(CPU *cpu){
    assert(rd32(cpu->rcx+0x138)==bottom_expected_dock);
    assert(fabsf(rdss(cpu->rdx+4)-bottom_expected_y)<.001f);
    memcpy((void*)(uintptr_t)(cpu->rcx+0xc0),(void*)(uintptr_t)cpu->rdx,16);
    ++bottom_native_calls;cpu->rsp+=8;
}

static void check_mobile_bottom(const CPU *caller){
    static _Alignas(16) unsigned char screens[2][0x3c0],frames[2][0x280],origins[2][0x280],bars[2][0x280];
    const char *names[]={"BOTTOM","BOTTOM_static"};
    for(unsigned i=0;i<2;++i){
        uint64_t screen=(uintptr_t)screens[i],frame=(uintptr_t)frames[i],origin=(uintptr_t)origins[i],bar=(uintptr_t)bars[i];
        uint64_t nodes[]={frame,origin,bar};
        for(unsigned j=0;j<3;++j){
            uint64_t node=nodes[j];wr64(node,GVA(UINT64_C(0x141D78A60)));wr32(node+0x1e4,9100+i*3+j);
            wrss(node+0xf0,1);wrss(node+0xf4,1);
            wrss(node+0x210,1280);wrss(node+0x100,1280);
        }
        wrss(frame+0x214,720);wrss(frame+0x104,720);
        wrss(bar+0x214,208);wrss(bar+0x104,208);wrss(bar+0xc0,7);wrss(bar+0xc4,-208);
        wr32(origin+0x138,4);wr32(origin+0x13c,14);wr32(bar+0x138,4);wr32(bar+0x13c,8);
        wr64(origin+0x1d8,(uintptr_t)names[i]);wr64(bar+0x1d8,(uintptr_t)"Bottom_Panel");
        wr64(screen+0x220,frame);wr64(frame+0x230,screen);wr64(frame+0x220,origin);
        wr64(origin+0x230,frame);wr64(origin+0x220,bar);wr64(bar+0x230,origin);
        CPU cpu=*caller;cpu.rcx=screen;scale=200;fp_mobile_ui_before(&cpu);
        assert(rd32(bar+0x138)==0&&rd32(bar+0x13c)==8);
        assert(rdss(bar+0x210)==640&&rdss(bar+0x100)==640&&rdss(bar+0x214)==208);
        assert(rdss(bar+0xc0)==0&&rdss(bar+0xc4)==-194);
        fp_mobile_ui_before(&cpu);assert(rd32(bar+0x138)==0&&rdss(bar+0xc4)==-194);
        float authored[4]={0,-208,0,1};CPU animation=cpu;animation.rcx=bar;animation.rdx=(uintptr_t)authored;
        bottom_expected_dock=0;bottom_expected_y=-194;
        fp_mobile_ui_position(&animation,bottom_position_original);
        assert(animation.rdx==(uintptr_t)authored&&rd32(bar+0x138)==0&&rdss(bar+0xc4)==-194);
        /* A native animation can re-dock after preparation. The position hook
         * must temporarily clear that dock for the original callback, then
         * preserve the caller's value until the next canvas preparation. */
        wr32(bar+0x138,4);animation.rdx=(uintptr_t)authored;
        fp_mobile_ui_position(&animation,bottom_position_original);
        assert(animation.rdx==(uintptr_t)authored&&rd32(bar+0x138)==4&&rdss(bar+0xc4)==-194);
        fp_mobile_ui_before(&cpu);assert(rd32(bar+0x138)==0&&rdss(bar+0xc4)==-194);
        scale=100;fp_mobile_ui_before(&cpu);
        assert(rd32(bar+0x138)==4&&rd32(bar+0x13c)==8);
        assert(rdss(bar+0x210)==1280&&rdss(bar+0x100)==1280&&rdss(bar+0x214)==208);
        assert(rdss(bar+0xc0)==7&&rdss(bar+0xc4)==-208);
        scale=200;fp_mobile_ui_before(&cpu);assert(rd32(bar+0x138)==0&&rdss(bar+0xc4)==-194);
        scale=100;fp_mobile_ui_before(&cpu);assert(rd32(bar+0x138)==4&&rdss(bar+0xc4)==-208);
    }
    static _Alignas(16) unsigned char unrelated[3][0x280],parents[3][0x280];
    const char *node_names[]={"OTHER_PANEL","Bottom_Panel","Bottom_Panel"};
    const char *parent_names[]={"BOTTOM","NOT_BOTTOM","BOTTOM"};
    scale=200;fp_mobile_ui_scale();
    for(unsigned i=0;i<3;++i){
        uint64_t node=(uintptr_t)unrelated[i],parent=(uintptr_t)parents[i];
        wr64(node+0x1d8,(uintptr_t)node_names[i]);wr64(parent+0x1d8,(uintptr_t)parent_names[i]);
        wr64(node+0x230,parent);wrss(node+0x214,i==2?207:208);wr32(node+0x138,4);
        float authored[4]={0,-208,0,1};CPU animation=*caller;animation.rcx=node;animation.rdx=(uintptr_t)authored;
        bottom_expected_dock=4;bottom_expected_y=-208;
        fp_mobile_ui_position(&animation,bottom_position_original);
        assert(animation.rdx==(uintptr_t)authored&&rd32(node+0x138)==4&&rdss(node+0xc4)==-208);
    }
    assert(bottom_native_calls==7);scale=100;fp_mobile_ui_scale();
}
