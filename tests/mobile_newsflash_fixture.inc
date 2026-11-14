static void check_mobile_newsflash(const CPU *caller){
    static _Alignas(16) unsigned char storage[5][0x400];
    uint64_t screen=(uintptr_t)storage[0],frame=(uintptr_t)storage[1],top=(uintptr_t)storage[2];
    uint64_t toast=(uintptr_t)storage[3],child=(uintptr_t)storage[4];
    for(unsigned i=1;i<5;++i){
        uint64_t node=(uintptr_t)storage[i];wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7900+i);
        wrss(node+0xf0,1);wrss(node+0xf4,1);wr64(node+0x230,(uintptr_t)storage[i-1]);
        wr64((uintptr_t)storage[i-1]+0x220,node);
    }
    wrss(frame+0x210,1280);wrss(frame+0x214,720);
    wr64(top+0x1d8,(uintptr_t)"TOP");wrss(top+0x210,1280);wrss(top+0x214,100);wr32(top+0x13c,7);
    wr64(toast+0x1d8,(uintptr_t)"EXT_NEWSFLASH_NOTIFICATION_PANEL");
    wrss(toast+0xc0,0);wrss(toast+0xc4,169);wrss(toast+0xd0,-225);wrss(toast+0xd4,-50);
    wrss(toast+0x210,450);wrss(toast+0x214,100);wrss(toast+0x100,450);wrss(toast+0x104,100);
    wrss(child+0x210,498);wrss(child+0x214,110);wrss(child+0x100,498);wrss(child+0x104,110);
    wrss(child+0xc0,250);wrss(child+0xc4,59);
    CPU cpu=*caller;cpu.rcx=screen;scale=200;fp_mobile_ui_scale();
    float authored[4]={302,169,3,2};CPU animation=cpu;animation.rcx=toast;animation.rdx=(uintptr_t)authored;
    // Native callbacks can precede layout with an already adapted X, even while the current field is zero.
    fp_mobile_ui_position(&animation,position_original);
    assert(animation.rdx==(uintptr_t)authored&&rdss(toast+0xc0)==302&&rdss(toast+0xc8)==authored[2]&&rdss(toast+0xcc)==authored[3]);
    const unsigned scales[]={200,150,200,100,200,100};
    for(unsigned i=0;i<sizeof(scales)/sizeof(scales[0]);++i){
        scale=scales[i];
        for(unsigned repeat=0;repeat<12;++repeat)fp_mobile_ui_before(&cpu);
        float expected=622+(scale>100?(rdss(GVA(0x142980E60))-1280)*.5f:0);
        assert(fabsf(rdss(toast+0xc0)-expected)<.001f&&rdss(toast+0xc4)==169);
        assert(rdss(toast+0x210)==450&&rdss(toast+0x214)==100&&rdss(toast+0x100)==450&&rdss(toast+0x104)==100);
        assert(rdss(toast+0xd0)==-225&&rdss(toast+0xd4)==-50&&rdss(toast+0xf0)==1&&rdss(toast+0xf4)==1);
        assert(rd32(toast+0x138)==0&&rd32(toast+0x13c)==0);
        assert(rdss(child+0xc0)==250&&rdss(child+0xc4)==59&&rdss(child+0x210)==498&&rdss(child+0xf0)==1);
        const float incoming_x[]={622,302,0};
        for(unsigned native_pass=0;native_pass<9;++native_pass){
            authored[0]=incoming_x[native_pass%3];authored[2]=(float)native_pass-5;authored[3]=(float)native_pass+2;
            animation.rdx=(uintptr_t)authored;fp_mobile_ui_position(&animation,position_original);
            assert(animation.rdx==(uintptr_t)authored&&fabsf(rdss(toast+0xc0)-expected)<.001f);
            fp_mobile_ui_before(&cpu);
            assert(fabsf(rdss(toast+0xc0)-expected)<.001f&&rdss(toast+0xc4)==authored[1]);
            assert(rdss(toast+0xc8)==authored[2]&&rdss(toast+0xcc)==authored[3]);
        }
    }
    // A newly created instance may instead reach layout before its first animation callback.
    wr32(toast+0x1e4,0x7909);wrss(toast+0xc0,622);scale=200;
    for(unsigned repeat=0;repeat<12;++repeat)fp_mobile_ui_before(&cpu);
    assert(rdss(toast+0xc0)==302);
    animation.rdx=(uintptr_t)authored;fp_mobile_ui_position(&animation,position_original);
    fp_mobile_ui_before(&cpu);assert(rdss(toast+0xc0)==302);
    scale=100;fp_mobile_ui_before(&cpu);assert(rdss(toast+0xc0)==622);
    // An identical name outside TOP must retain its authored position.
    wr64(top+0x1d8,(uintptr_t)"OTHER_TOP");scale=200;fp_mobile_ui_scale();animation.rdx=(uintptr_t)authored;
    fp_mobile_ui_position(&animation,position_original);assert(rdss(toast+0xc0)==authored[0]);
    wr64(top+0x1d8,(uintptr_t)"TOP");scale=100;fp_mobile_ui_before(&cpu);
}
