/* Photo's mask contains only three decorative splosh templates. Settings remain
 * siblings; hide the broken decoration at every scale without changing controls. */
static void check_mobile_photo_mask(const CPU *caller){
    static _Alignas(16) unsigned char storage[37][0x400];
    const char *names[]={"","FRAME","EXT_PHOTO_MODE_PANEL","ANIM","SETTINGS_PANEL",
        "SETTINGS_DETAILED","settings_left_top_container","mask","unrelated","mask",
        "left_splosh","SFX_MASK","center_splosh","SFX_MASK","right_splosh","SFX_MASK",
        "CAPTION","RESET_SETTINGS_BUTTON","TOGGLE_BUTTON","TAKE_SCREENSHOT_BUTTON","EXIT_BUTTON",
        "right_bottom_panel","HELP_PANEL","mask","PHOTOMODE_TOOLTIP_TEXT","HELP_BUTTON",
        "SETTINGS_SHORT","left_splosh","SFX_MASK","SFX_MASK","SFX_MASK","SFX_MASK","SFX_MASK",
        "TOGGLE_BUTTON","TAKE_SCREENSHOT_BUTTON","SFX_MASK","SFX_MASK"};
    for(unsigned i=0;i<37;++i){
        uint64_t node=(uintptr_t)storage[i];wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7e800+i);
        wr64(node+0x1d8,(uintptr_t)names[i]);wrss(node+0xf0,1);wrss(node+0xf4,1);
        wrss(node+0x100,64);wrss(node+0x104,64);wrss(node+0x210,64);wrss(node+0x214,64);
        if(i&&i<8){wr64(node+0x230,(uintptr_t)storage[i-1]);wr64((uintptr_t)storage[i-1]+0x220,node);}
    }
    uint64_t screen=(uintptr_t)storage[0],frame=(uintptr_t)storage[1],container=(uintptr_t)storage[6];
    uint64_t mask=(uintptr_t)storage[7],other=(uintptr_t)storage[9];
    wr64(screen,GVA(0x141D7B0B0));
    for(unsigned i=1;i<6;++i){uint64_t node=(uintptr_t)storage[i];wrss(node+0x100,1280);wrss(node+0x104,720);wrss(node+0x210,1280);wrss(node+0x214,720);}
    // Actual photo100-6j held physical-pixel extents despite 1280x720 requested size.
    for(unsigned i=4;i<6;++i){uint64_t node=(uintptr_t)storage[i];wrss(node+0x100,2750.232f);wrss(node+0x104,972);wr32(node+0x138,i==4?6:5);}
    wrss((uintptr_t)storage[4]+0xc0,-285.116f);
    wrss(container+0xc0,12);wrss(container+0xc4,282);
    wr64(mask,GVA(0x141DA2AA8));wrss(mask+0xc0,-30);wrss(mask+0xc4,-347.2f);
    wrss(mask+0x100,500);wrss(mask+0x104,400);wrss(mask+0x210,500);wrss(mask+0x214,400);
    wr64(frame+0x240,(uintptr_t)storage[8]);wr64((uintptr_t)storage[8]+0x230,screen);
    wr64((uintptr_t)storage[8]+0x220,other);wr64(other+0x230,(uintptr_t)storage[8]);wr64(other,GVA(0x141DA2AA8));
    wrss(other+0x100,500);wrss(other+0x104,400);wrss(other+0x210,500);wrss(other+0x214,400);
    for(unsigned i=10;i<16;i+=2){
        uint64_t splosh=(uintptr_t)storage[i],sfx=(uintptr_t)storage[i+1];
        wr64(splosh+0x230,mask);wr64(splosh+0x220,sfx);wr64(sfx+0x230,splosh);wr64(sfx,GVA(0x141D796F8));
        if(i==10)wr64(mask+0x220,splosh);else wr64((uintptr_t)storage[i-2]+0x240,splosh);
        wrss(splosh+0xc0,i==10?-720:i==12?-315:-218);wrss(splosh+0xc4,i==10?-212:-230);
        wrss(splosh+0x100,i==10?1700:1100);wrss(splosh+0x104,500);
        wrss(sfx+0x100,i==10?1700:1100);wrss(sfx+0x104,500);
    }
    wr64(mask+0x240,(uintptr_t)storage[16]);
    for(unsigned i=16;i<21;++i){
        uint64_t node=(uintptr_t)storage[i];wr64(node+0x230,container);
        if(i<20)wr64(node+0x240,(uintptr_t)storage[i+1]);
        wrss(node+0x100,i==16?360:108);wrss(node+0x104,i==16?37:25);
        wrss(node+0x210,rdss(node+0x100));wrss(node+0x214,rdss(node+0x104));
    }
    uint64_t caption=(uintptr_t)storage[16];wrss(caption+0xc4,17);
    wr64(container+0x240,(uintptr_t)storage[21]);wr64((uintptr_t)storage[21]+0x230,(uintptr_t)storage[5]);
    for(unsigned i=22;i<25;++i){uint64_t node=(uintptr_t)storage[i],parent=(uintptr_t)storage[i-1];wr64(node+0x230,parent);wr64(parent+0x220,node);}
    uint64_t help=(uintptr_t)storage[25];wr64(help+0x230,(uintptr_t)storage[21]);wr64((uintptr_t)storage[22]+0x240,help);
    wrss(help+0x100,188.571f);wrss(help+0x104,71.429f);wrss(help+0x210,188.571f);wrss(help+0x214,71.429f);
    wr64((uintptr_t)storage[5]+0x240,(uintptr_t)storage[26]);wr64((uintptr_t)storage[26]+0x230,(uintptr_t)storage[4]);
    wrss((uintptr_t)storage[26]+0x100,285);wrss((uintptr_t)storage[26]+0x104,157);
    const unsigned short_parent[]={26,27,26,29,26,31,30,32};
    for(unsigned i=27;i<35;++i){
        uint64_t node=(uintptr_t)storage[i],parent=(uintptr_t)storage[short_parent[i-27]];wr64(node+0x230,parent);
        uint64_t tail=rd64(parent+0x220);if(!tail)wr64(parent+0x220,node);else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,node);}
    }
    for(unsigned i=28;i<33;i+=2){uint64_t node=(uintptr_t)storage[i];wr64(node,GVA(0x141D796F8));wr32(node+0x1e0,0x300200);wrss(node+0x100,1280);wrss(node+0x104,720);}
    wrss((uintptr_t)storage[30]+0xc0,-646);wrss((uintptr_t)storage[30]+0xc4,-365);
    wrss((uintptr_t)storage[32]+0xc0,-606);wrss((uintptr_t)storage[32]+0xc4,-345);
    for(unsigned i=33;i<35;++i){uint64_t node=(uintptr_t)storage[i];wrss(node+0x100,30);wrss(node+0x104,30);wrss(node+0x210,30);wrss(node+0x214,30);wrss(node+0xd0,-15);wrss(node+0xd4,-15);}
    wr64((uintptr_t)storage[20]+0x240,(uintptr_t)storage[35]);wr64((uintptr_t)storage[35]+0x230,container);wr64((uintptr_t)storage[35],GVA(0x141D796F8));
    wr64(other+0x220,(uintptr_t)storage[36]);wr64((uintptr_t)storage[36]+0x230,other);wr64((uintptr_t)storage[36],GVA(0x141D796F8));
    CPU cpu=*caller;cpu.rcx=screen;fp_mobile_camera_destroy();
    _Alignas(16) unsigned char camera_system[0x340]={0},camera[0x240]={0};
    wr64((uintptr_t)camera_system+0xa0,(uintptr_t)camera);wrss((uintptr_t)camera+0x30,100);wrss((uintptr_t)camera+0x2c,1);
    CPU camera_cpu=cpu;camera_cpu.rcx=(uintptr_t)camera_system;fp_mobile_camera_before(&camera_cpu);
    const unsigned scales[]={100,125,150,200,200,100,200,100};
    for(unsigned original_hidden=0;original_hidden<2;++original_hidden){
        wr32(mask+0x1e4,0x7e807+original_hidden*0x100);wr32(mask+0x1e0,0x100200|(original_hidden?2:0));
        for(unsigned i=0;i<sizeof(scales)/sizeof(scales[0]);++i){
            scale=scales[i];fp_mobile_ui_before(&cpu);
            assert(rdss(mask+0x104)==400&&rdss(mask+0x214)==400&&rdss(mask+0x100)==500&&rdss(mask+0x210)==500);
            assert(rdss(mask+0xf4)==1&&rdss(mask+0xf0)==1&&rdss(mask+0xc0)==-30&&rdss(mask+0xc4)==-347.2f);
            assert(rd32(mask+0x1e0)==(0x100200|2));
            assert(rdss(other+0x100)==500&&rdss(other+0x104)==400&&rdss(other+0xf4)==1&&rd32(other+0x1e0)==0);
            for(unsigned j=10;j<26;++j)assert(!(rd32((uintptr_t)storage[j]+0x1e0)&2));
            assert(rd64(mask+0x220)==(uintptr_t)storage[10]&&rd64(mask+0x240)==caption);
            assert(rdss(caption+0xc4)==17&&rdss(caption+0x100)==360&&rdss(caption+0x104)==37);
            for(unsigned j=4;j<6;++j){
                uint64_t node=(uintptr_t)storage[j];float bounds[4];law_selected_bounds(node,bounds);
                assert(rd32(node+0x138)==0&&fabsf(bounds[0])<.002f&&fabsf(bounds[1])<.002f);
                assert(rdss(node+0x100)==rdss(GVA(0x142980E60))&&rdss(node+0x104)==rdss(GVA(0x142980E64)));
            }
            for(unsigned j=16;j<26;++j){
                if(j==21)continue;
                float bounds[4];law_selected_bounds((uintptr_t)storage[j],bounds);
                float left=fminf(bounds[0],bounds[0]+bounds[2]),right=fmaxf(bounds[0],bounds[0]+bounds[2]);
                float top=fminf(bounds[1],bounds[1]+bounds[3]),bottom=fmaxf(bounds[1],bounds[1]+bounds[3]);
                assert(left>=-.002f&&top>=-.002f&&right<=rdss(GVA(0x142980E60))+.002f&&bottom<=rdss(GVA(0x142980E64))+.002f);
            }
            float hb[4];law_selected_bounds(help,hb);
            assert(fabsf(rdss(help+0xf0)+.6f)<.001f&&fabsf(rdss(help+0xf4)-.6f)<.001f);
            assert(rdss(help+0x100)==188.571f&&rdss(help+0x104)==71.429f);
            float hl=fminf(hb[0],hb[0]+hb[2]),hr=fmaxf(hb[0],hb[0]+hb[2]);
            for(unsigned j=17;j<21;++j){
                float fb[4];law_selected_bounds((uintptr_t)storage[j],fb);
                assert(hr<=fb[0]||fb[0]+fb[2]<=hl||hb[1]+hb[3]<=fb[1]||fb[1]+fb[3]<=hb[1]);
            }
            for(unsigned j=28;j<33;j+=2){uint64_t node=(uintptr_t)storage[j];assert(rd32(node+0x1e0)==0x300201&&rdss(node+0x100)==1280&&rdss(node+0x104)==720);}
            assert(rd32((uintptr_t)storage[35]+0x1e0)==0&&rd32((uintptr_t)storage[36]+0x1e0)==0);
            for(unsigned j=33;j<35;++j){
                uint64_t node=(uintptr_t)storage[j];float b[4];law_selected_bounds(node,b);
                assert(rd32(node+0x1e0)==0&&rd64(node+0x230)==(uintptr_t)storage[j==33?30:32]);
                assert(fabsf(b[0]-(rdss(GVA(0x142980E60))*.5f+(j==33?-50:8)))<.002f);
                assert(fabsf(b[1]-(rdss(GVA(0x142980E64))-50))<.002f&&fabsf(b[2]-42)<.002f&&fabsf(b[3]-42)<.002f);
            }
        }
    }
    fp_mobile_camera_destroy();
}
