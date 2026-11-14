static float affine_source[2],affine_target[2],affine_ratio;
static unsigned affine_draws;
static void dialog_affine_original(CPU *cpu) {
    uint64_t node=cpu->rcx;const float *matrix=(const float*)(uintptr_t)cpu->rdx;
    assert(!rd8(node+0x383));
    assert(fabsf(rdss(node+0x100)-1280*affine_ratio)<.002f&&fabsf(rdss(node+0x104)-720*affine_ratio)<.002f);
    assert(fabsf(rdss(node+(rd8(node+0x385)?0x378:0x37c))-(rd8(node+0x385)?75:10*affine_ratio))<.002f);
    /* Map the source INK origin and the far full-image corner. Both must follow
     * the same 2D composition, including translation with an unchanged scale. */
    for(unsigned i=0;i<2;++i){
        float source_x=affine_source[0]+i*1280,source_y=affine_source[1]+i*720;
        float target_x=affine_target[0]+i*1280*affine_ratio,target_y=affine_target[1]+i*720*affine_ratio;
        assert(fabsf(matrix[12]+rdss(node+0xd0)+source_x*affine_ratio-target_x)<.003f);
        assert(fabsf(matrix[13]+rdss(node+0xd4)+source_y*affine_ratio-target_y)<.003f);
    }
    assert(matrix[0]==1&&matrix[5]==1&&matrix[10]==1&&matrix[15]==1);
    ++affine_draws;
}

static void check_dialog_affine_draw(const CPU *caller,uint64_t node,float source_x,float source_y,float target_x,float target_y,float ratio) {
    affine_source[0]=source_x;affine_source[1]=source_y;affine_target[0]=target_x;affine_target[1]=target_y;affine_ratio=ratio;
    float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,19,23,0,1},saved[16];memcpy(saved,matrix,sizeof(saved));
    CPU draw=*caller;draw.rcx=node;draw.rdx=(uintptr_t)matrix;
    float width=rdss(node+0x100),height=rdss(node+0x104),source_w=rdss(node+0x210),source_h=rdss(node+0x214);
    wr8(node+0x383,1);wrss(node+0x378,75);wrss(node+0x37c,10);
    unsigned before=affine_draws;
    for(unsigned perspective=0;perspective<2;++perspective){
        wr8(node+0x385,perspective);fp_mobile_ui_template(&draw,dialog_affine_original);
        assert(draw.rdx==(uintptr_t)matrix&&!memcmp(matrix,saved,sizeof(matrix)));
        assert(rd8(node+0x383)==1&&rdss(node+0x100)==width&&rdss(node+0x104)==height);
        assert(rdss(node+0x210)==source_w&&rdss(node+0x214)==source_h&&rdss(node+0x378)==75&&rdss(node+0x37c)==10);
    }
    assert(affine_draws==before+2);
}

static void check_mobile_dialog_affine(const CPU *caller) {
    static _Alignas(16) unsigned char storage[9][5][0x400];
    const char *names[]={"EXT_MAIN_MENU_PANEL","EXT_INGAME_MENU_MAIN_PANEL","EXT_SETTINGS_PANEL",
        "EXT_INGAME_MENU_SETTINGS_PANEL","EXT_LOADGAME_PANEL","EXT_INGAME_MENU_LOADGAME_PANEL",
        "EXT_INGAME_MENU_SAVEGAME_PANEL","EXT_SAVE_GAME_PANEL","EXT_CONFIRMATION_PANEL"};
    fp_mobile_camera_destroy();test_ime_fraction=1;test_ime_editor=0;
    for(unsigned i=0;i<9;++i){
        unsigned compact=i>=7;
        uint64_t screen=(uintptr_t)storage[i][0],frame=(uintptr_t)storage[i][1],root=(uintptr_t)storage[i][2];
        uint64_t ink=(uintptr_t)storage[i][3],mask=(uintptr_t)storage[i][4];
        for(unsigned j=1;j<5;++j){
            uint64_t node=(uintptr_t)storage[i][j];wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7300+i*5+j);
            wrss(node+0xf0,1);wrss(node+0xf4,1);wrss(node+0x100,1280);wrss(node+0x104,720);wrss(node+0x210,1280);wrss(node+0x214,720);
            uint64_t parent=(uintptr_t)storage[i][j-1];wr64(node+0x230,parent);wr64(parent+0x220,node);
        }
        wr64(frame+0x1d8,(uintptr_t)"FRAME");wr64(root+0x1d8,(uintptr_t)names[i]);
        wr64(ink+0x1d8,(uintptr_t)"INK_PANEL");wr64(mask+0x1d8,(uintptr_t)"SFX_TXT_MASK");wr64(mask,GVA(0x141D796F8));
        float source_x=compact?423:0,source_y=compact?155:0;
        wrss(ink+0xc0,compact?423:250);wrss(ink+0xc4,compact?155:250);wrss(ink+0xd0,compact?0:-250);wrss(ink+0xd4,compact?0:-250);
        wrss(ink+0x100,compact?64:500);wrss(ink+0x104,compact?64:500);wrss(ink+0x210,compact?64:500);wrss(ink+0x214,compact?64:500);
        wrss(mask+0xd0,-7);wrss(mask+0xd4,-11);wrss(mask+0x100,64);wrss(mask+0x104,64);wrss(mask+0x210,64);wrss(mask+0x214,64);
        CPU cpu=*caller;cpu.rcx=screen;scale=100;fp_mobile_ui_before(&cpu);
        scale=200;fp_mobile_ui_before(&cpu);
        if(!compact){
            // Real native SetSize recomputes a centred pivot. Its original -250
            // must still be used to recover the source image after this mutation.
            wrss(ink+0xd0,-640);wrss(ink+0xd4,-360);fp_mobile_ui_before(&cpu);
        }
        float ratio=rdss(ink+0xf0),target_x=rdss(ink+0xc0)+rdss(ink+0xd0)*ratio,target_y=rdss(ink+0xc4)+rdss(ink+0xd4)*ratio;
        check_dialog_affine_draw(&cpu,mask,source_x,source_y,target_x,target_y,ratio);
        // A pure shift (as with IME at 100%) must not be lost by a factor==1 early return.
        wrss(ink+0xf0,1);wrss(ink+0xf4,1);wrss(ink+0xd0,compact?0:-250);wrss(ink+0xd4,compact?0:-250);
        wrss(ink+0xc0,compact?423:250);wrss(ink+0xc4,(compact?155:250)-84);
        check_dialog_affine_draw(&cpu,mask,source_x,source_y,source_x,source_y-84,1);
        scale=100;fp_mobile_ui_before(&cpu);
        assert(rdss(ink+0xc0)==(compact?423:250)&&rdss(ink+0xc4)==(compact?155:250));
        // Ordinary 100% is the exact original draw: no viewport or pointer replacement.
        wr8(mask+0x383,1);wr8(mask+0x385,1);expected_projection=75;expected_template_width=expected_template_height=64;
        CPU draw=cpu;draw.rcx=mask;draw.rdx=0;fp_mobile_ui_template(&draw,template_original);assert(draw.rdx==0&&rd8(mask+0x383)==1);
    }
    expected_template_width=expected_template_height=0;scale=100;
}
