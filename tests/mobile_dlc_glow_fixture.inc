static void dlc_fixture_attach(uint64_t parent,uint64_t child){
    wr64(child+0x230,parent);uint64_t tail=rd64(parent+0x220);
    if(!tail)wr64(parent+0x220,child);
    else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,child);}
}

static void check_mobile_dlc_glow(const CPU *caller){
    static _Alignas(16) unsigned char storage[26][0x400];
    uint64_t screen=(uintptr_t)storage[0],frame=(uintptr_t)storage[1],root=(uintptr_t)storage[2];
    uint64_t ink=(uintptr_t)storage[3],mask=(uintptr_t)storage[4],menu=(uintptr_t)storage[5];
    for(unsigned i=1;i<26;++i){
        uint64_t node=(uintptr_t)storage[i];wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7500+i);
        wrss(node+0xf0,1);wrss(node+0xf4,1);wrss(node+0x100,64);wrss(node+0x104,64);wrss(node+0x210,64);wrss(node+0x214,64);
    }
    dlc_fixture_attach(screen,frame);dlc_fixture_attach(frame,root);dlc_fixture_attach(root,ink);dlc_fixture_attach(ink,mask);dlc_fixture_attach(mask,menu);
    wr64(frame+0x1d8,(uintptr_t)"FRAME");wr64(root+0x1d8,(uintptr_t)"EXT_MAIN_MENU_PANEL");
    wr64(ink+0x1d8,(uintptr_t)"INK_PANEL");wr64(mask+0x1d8,(uintptr_t)"SFX_TXT_MASK");wr64(menu+0x1d8,(uintptr_t)"MENU_LAYOUT");
    wr64(mask,GVA(0x141D796F8));wr64(menu,GVA(0x141DE0A48));
    for(unsigned i=1;i<=2;++i){uint64_t node=(uintptr_t)storage[i];wrss(node+0x100,1280);wrss(node+0x104,720);wrss(node+0x210,1280);wrss(node+0x214,720);}
    const char *buttons[]={"SCENARIO_BUTTON","ENDLESS_BUTTON","DLC_BUTTON"};
    uint64_t glows[3],labels[3],pictures[3];
    for(unsigned i=0;i<3;++i){
        uint64_t button=(uintptr_t)storage[6+i*5],layout=(uintptr_t)storage[7+i*5];
        glows[i]=(uintptr_t)storage[8+i*5];labels[i]=(uintptr_t)storage[9+i*5];pictures[i]=(uintptr_t)storage[10+i*5];
        dlc_fixture_attach(menu,button);dlc_fixture_attach(button,layout);dlc_fixture_attach(layout,glows[i]);dlc_fixture_attach(layout,labels[i]);dlc_fixture_attach(glows[i],pictures[i]);
        wr64(button+0x1d8,(uintptr_t)buttons[i]);wr64(layout+0x1d8,(uintptr_t)"MENU_BUTTON_LAYOUT");
        wr64(glows[i]+0x1d8,(uintptr_t)"NEW_DLC_SHARP_GLOW");wr32(glows[i]+0x1e0,0x4300205);
        wr64(labels[i],GVA(0x141DAD740));wr64(labels[i]+0x1d8,(uintptr_t)"NEW_DLC_AVAILABLE_UNLOCKED");wr32(labels[i]+0x1e0,0x300200);
        wr64(pictures[i],GVA(0x141DA2AA8));wr64(pictures[i]+0x1d8,(uintptr_t)"sharp_glow");
        wrss(pictures[i]+0xc0,83.362f);wrss(pictures[i]+0xc4,65.091f);
    }
    uint64_t wrong_parent=(uintptr_t)storage[21],unrelated=(uintptr_t)storage[22],wrong_type=(uintptr_t)storage[23];
    dlc_fixture_attach(menu,wrong_parent);dlc_fixture_attach(wrong_parent,unrelated);dlc_fixture_attach((uintptr_t)storage[7],wrong_type);
    wr64(wrong_parent+0x1d8,(uintptr_t)"OTHER_LAYOUT");wr64(unrelated+0x1d8,(uintptr_t)"NEW_DLC_SHARP_GLOW");
    wr64(wrong_type+0x1d8,(uintptr_t)"NEW_DLC_SHARP_GLOW");wr64(wrong_type,GVA(0x141DA2AA8));
    CPU cpu=*caller;cpu.rcx=screen;fp_mobile_camera_destroy();
    const unsigned scales[]={200,150,200,100,200,100};
    for(unsigned pass=0;pass<sizeof(scales)/sizeof(scales[0]);++pass){
        scale=scales[pass];
        if(scale==100)wr32(glows[0]+0x1e0,0x4300207); // Restore even if this decorative node is currently hidden.
        for(unsigned repeat=0;repeat<10;++repeat)fp_mobile_ui_before(&cpu);
        for(unsigned i=0;i<3;++i){
            assert(rdss(glows[i]+0xf0)==(scale>100?0:1)&&rdss(glows[i]+0xf4)==(scale>100?0:1));
            assert(rdss(labels[i]+0xf0)==1&&rdss(labels[i]+0xf4)==1&&rd32(labels[i]+0x1e0)==0x300200);
            assert(rdss(pictures[i]+0xf0)==1&&rdss(pictures[i]+0xc0)==83.362f&&rdss(pictures[i]+0xc4)==65.091f);
        }
        wr32(glows[0]+0x1e0,0x4300205);
        assert(rdss(unrelated+0xf0)==1&&rdss(wrong_type+0xf0)==1);
    }
    // Identical decoration inside the pause menu remains authored.
    wr64(root+0x1d8,(uintptr_t)"EXT_INGAME_MENU_MAIN_PANEL");scale=200;fp_mobile_ui_before(&cpu);
    for(unsigned i=0;i<3;++i)assert(rdss(glows[i]+0xf0)==1&&rdss(glows[i]+0xf4)==1);
    scale=100;fp_mobile_ui_before(&cpu);expected_template_width=expected_template_height=0;
}
