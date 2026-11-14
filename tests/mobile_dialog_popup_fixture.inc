/* Native popup geometry from EXT_SAVE_GAME_PANEL and the live HUD tooltip tree. */
static void check_mobile_dialog_popups(const CPU *caller) {
    static _Alignas(16) unsigned char storage[9][0x400];
    uint64_t screen=(uintptr_t)storage[0],frame=(uintptr_t)storage[1];
    uint64_t popup=(uintptr_t)storage[2],ink=(uintptr_t)storage[3];
    uint64_t paint=(uintptr_t)storage[4],editor=(uintptr_t)storage[5];
    uint64_t tooltips=(uintptr_t)storage[6],hope=(uintptr_t)storage[7],detail=(uintptr_t)storage[8];
    for(unsigned i=1;i<9;++i){
        uint64_t node=(uintptr_t)storage[i];
        wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7100+i);
        wrss(node+0xf0,1);wrss(node+0xf4,1);
        wrss(node+0x100,64);wrss(node+0x104,64);
        wrss(node+0x210,64);wrss(node+0x214,64);
    }
    wr64(frame+0x1d8,(uintptr_t)"FRAME");wr64(frame+0x230,screen);wr64(screen+0x220,frame);
    wr64(frame+0x220,popup);wr64(popup+0x230,frame);wr64(popup+0x220,ink);
    wr64(popup+0x1d8,(uintptr_t)"EXT_SAVE_GAME_PANEL");
    for(unsigned i=1;i<=2;++i){
        uint64_t node=(uintptr_t)storage[i];
        wrss(node+0x100,1280);wrss(node+0x104,720);
        wrss(node+0x210,1280);wrss(node+0x214,720);
    }
    wr64(ink+0x230,popup);wr64(ink+0x220,paint);wr64(ink+0x1d8,(uintptr_t)"INK_PANEL");
    wrss(ink+0xc0,423);wrss(ink+0xc4,155);
    wr64(paint,GVA(0x141D796F8));wr64(paint+0x230,ink);wr64(paint+0x220,editor);
    wr64(paint+0x1d8,(uintptr_t)"SFX_MASK");wrss(paint+0xc0,-171);wrss(paint+0xc4,107);
    wrss(paint+0x378,75);wrss(paint+0x37c,10);
    wr64(editor,GVA(0x141DAC248));wr64(editor+0x230,paint);
    wr64(editor+0x1d8,(uintptr_t)"SAVE_NAME_INPUT");
    wrss(editor+0xc0,243);wrss(editor+0xc4,62);
    wrss(editor+0x100,264);wrss(editor+0x104,40);wrss(editor+0x210,264);wrss(editor+0x214,40);
    CPU cpu=*caller;cpu.rcx=screen;fp_mobile_camera_destroy();
    const unsigned requested[]={125,150,200,100,200,100};
    for(unsigned i=0;i<sizeof(requested)/sizeof(requested[0]);++i){
        scale=requested[i];fp_mobile_ui_before(&cpu);
        float factor=scale==100?1:fminf(scale/100.f,1.5f);
        assert(rdss(GVA(0x142980e60))==1280&&rdss(GVA(0x142980e64))==720);
        assert(rdss(ink+0x210)==64&&rdss(ink+0x214)==64&&rdss(ink+0x100)==64&&rdss(ink+0x104)==64);
        assert(fabsf(rdss(ink+0xf0)-factor)<.0001f&&fabsf(rdss(ink+0xf4)-factor)<.0001f);
        assert(fabsf(rdss(ink+0xc0)-(scale==100?423:640-204*factor))<.001f);
        assert(fabsf(rdss(ink+0xc4)-(scale==100?155:360-178*factor))<.001f);
        assert(rdss(paint+0xc0)==-171&&rdss(paint+0xc4)==107&&rdss(paint+0xf0)==1);
        assert(rdss(editor+0xc0)==243&&rdss(editor+0xc4)==62&&rdss(editor+0xf0)==1);
        assert(rdss(editor+0x210)==264&&rdss(editor+0x214)==40);
        float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};CPU draw=cpu;draw.rcx=paint;draw.rdx=(uintptr_t)matrix;
        for(unsigned fullscreen=0;fullscreen<2;++fullscreen){
            wr8(paint+0x383,fullscreen);wr8(paint+0x385,1);
            expected_projection=75;
            expected_template_width=fullscreen&&factor!=1?1280*factor:64*factor;
            expected_template_height=fullscreen&&factor!=1?720*factor:64*factor;
            fp_mobile_ui_template(&draw,template_original);
            assert(rdss(paint+0x378)==75&&rdss(paint+0x100)==64&&rdss(paint+0x104)==64);
            wr8(paint+0x385,0);expected_projection=10*factor;
            fp_mobile_ui_template(&draw,template_original);
            assert(rdss(paint+0x37c)==10&&rdss(paint+0x100)==64&&rdss(paint+0x104)==64);
            assert(rdss(paint+0x210)==64&&rdss(paint+0x214)==64&&rd8(paint+0x383)==fullscreen);
        }
        fp_mobile_ui_before(&cpu);assert(fabsf(rdss(ink+0xf0)-factor)<.0001f);
    }
    expected_template_width=expected_template_height=0;

    test_ime_editor=editor;
    for(unsigned requested_scale=125;requested_scale<=200;requested_scale+=25){
        scale=requested_scale;float factor=fminf(scale/100.f,1.5f),baseline=360-178*factor;
        test_ime_fraction=.4f;
        float expected_y=baseline-fmaxf(0,baseline+209*factor-288+8);
        for(unsigned repeat=0;repeat<10;++repeat){
            fp_mobile_ui_before(&cpu);assert(fabsf(rdss(ink+0xc4)-expected_y)<.002f);
        }
        if(factor==1.5f)assert(fabsf(expected_y+33.5f)<.001f);
        check_dialog_affine_draw(&cpu,paint,423,155,640-204*factor,expected_y,factor);
        test_ime_fraction=.5f;fp_mobile_ui_before(&cpu);
        expected_y=baseline-fmaxf(0,baseline+209*factor-360+8);
        assert(fabsf(rdss(ink+0xc4)-expected_y)<.002f);
        if(factor==1.5f)assert(fabsf(expected_y-38.5f)<.001f);
        test_ime_fraction=1;fp_mobile_ui_before(&cpu);assert(fabsf(rdss(ink+0xc4)-baseline)<.002f);
    }
    scale=200;test_ime_fraction=.4f;test_ime_editor=0;fp_mobile_ui_before(&cpu);assert(rdss(ink+0xc4)==93);
    test_ime_editor=detail;fp_mobile_ui_before(&cpu);assert(rdss(ink+0xc4)==93);
    test_ime_editor=editor;scale=100;
    for(unsigned repeat=0;repeat<10;++repeat){fp_mobile_ui_before(&cpu);assert(fabsf(rdss(ink+0xc4)-71)<.002f);}
    check_dialog_affine_draw(&cpu,paint,423,155,423,71,1);
    test_ime_fraction=1;test_ime_editor=0;fp_mobile_ui_before(&cpu);assert(rdss(ink+0xc0)==423&&rdss(ink+0xc4)==155);

    /* This 64px origin must not inherit generic dialog centring from its
     * child bounds: the native tooltip controller already positions in HUD units. */
    wr64(frame+0x220,tooltips);wr64(tooltips+0x230,frame);wr64(tooltips+0x220,hope);
    wr64(tooltips+0x1d8,(uintptr_t)"TOOLTIPS");
    wr64(hope,GVA(0x141DE0A48));wr64(hope+0x230,tooltips);wr64(hope+0x240,detail);
    wr64(hope+0x1d8,(uintptr_t)"EXT_HOPE_TOOLTIP");wrss(hope+0xc0,166);wrss(hope+0xc4,211);
    wrss(hope+0x100,318);wrss(hope+0x104,94);wrss(hope+0x210,318);wrss(hope+0x214,94);
    wr64(detail+0x230,tooltips);wr64(detail+0x1d8,(uintptr_t)"EXT_DISCONTENT_TOOLTIP");
    wrss(detail+0xc0,340);wrss(detail+0xc4,190);
    _Alignas(16) unsigned char system[0x340]={0},camera[0x240]={0};
    wr64((uintptr_t)system+0xa0,(uintptr_t)camera);wrss((uintptr_t)camera+0x30,100);wrss((uintptr_t)camera+0x2c,1);
    cpu.rcx=(uintptr_t)system;fp_mobile_camera_before(&cpu);cpu.rcx=screen;
    const unsigned hud_scales[]={200,150,200,100};
    for(unsigned i=0;i<sizeof(hud_scales)/sizeof(hud_scales[0]);++i){
        scale=hud_scales[i];fp_mobile_ui_before(&cpu);fp_mobile_ui_before(&cpu);
        assert(fabsf(rdss(GVA(0x142980e60))-128000.f/scale)<.001f);
        assert(rdss(tooltips+0xc0)==0&&rdss(tooltips+0xc4)==0);
        assert(rd32(tooltips+0x13c)==0&&rd32(tooltips+0x138)==0);
        assert(rdss(tooltips+0x210)==64&&rdss(tooltips+0x214)==64&&rdss(tooltips+0xf0)==1);
        assert(rdss(hope+0xc0)==166&&rdss(hope+0xc4)==211&&rdss(hope+0xf0)==1&&rd32(hope+0x13c)==0);
        assert(rdss(detail+0xc0)==340&&rdss(detail+0xc4)==190&&rdss(detail+0xf0)==1&&rd32(detail+0x13c)==0);
    }
    /* Repair a container already moved by the old generic path without moving its children. */
    scale=200;wrss(tooltips+0xc0,-320);wrss(tooltips+0xc4,-180);wr32(tooltips+0x13c,31);wr32(tooltips+0x138,5);
    fp_mobile_ui_before(&cpu);
    assert(rdss(tooltips+0xc0)==0&&rdss(tooltips+0xc4)==0&&rd32(tooltips+0x13c)==0&&rd32(tooltips+0x138)==0);
    assert(rdss(hope+0xc0)==166&&rdss(hope+0xc4)==211&&rdss(detail+0xc0)==340&&rdss(detail+0xc4)==190);
    /* A closing native tooltip may clear its measured contents before hiding.
     * The mobile wrapper must neither recenter nor revive that empty panel. */
    wr64(detail,GVA(0x141DE0A48));wr64(detail+0x1d8,(uintptr_t)"EXT_DETAILED_TOOLTIP");
    wrss(detail+0x100,318);wrss(detail+0x104,170);wrss(detail+0x210,300);wrss(detail+0x214,250);
    wrss(detail+0xc0,340);wrss(detail+0xc4,120);
    unsigned positions_before=position_calls;fp_mobile_ui_before(&cpu);
    assert(position_calls==positions_before&&rdss(detail+0xc0)==340&&rdss(detail+0xc4)==120);
    wrss(detail+0x100,0);wrss(detail+0x104,0);fp_mobile_ui_before(&cpu);
    assert(position_calls==positions_before&&rdss(detail+0xc0)==340&&rdss(detail+0xc4)==120);
    wr32(detail+0x1e0,rd32(detail+0x1e0)|2);fp_mobile_ui_before(&cpu);
    assert(position_calls==positions_before&&(rd32(detail+0x1e0)&2));
    float tooltip_position[4]={240,96,0,1};CPU tooltip_callback=cpu;tooltip_callback.rcx=detail;tooltip_callback.rdx=(uintptr_t)tooltip_position;
    fp_mobile_ui_position(&tooltip_callback,position_original);
    wrss(detail+0x100,318);wrss(detail+0x104,170);wr32(detail+0x1e0,rd32(detail+0x1e0)&~2u);fp_mobile_ui_before(&cpu);
    assert(position_calls==positions_before&&rdss(detail+0xc0)==240&&rdss(detail+0xc4)==96);
    assert(rdss(tooltips+0xc0)==0&&rdss(tooltips+0xc4)==0&&rdss(detail+0xf0)==1);
    scale=100;fp_mobile_ui_before(&cpu);fp_mobile_camera_destroy();
}
