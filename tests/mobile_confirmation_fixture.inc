/* Exact UI/Misc/Confirmation geometry: wide camera, compact 64px INK origin. */
static void check_mobile_confirmation(const CPU *caller){
    static _Alignas(16) unsigned char storage[12][0x400];
    const char *names[]={"","FRAME","EXT_CONFIRMATION_PANEL","INK_PANEL","SFX_MASK","TEXT_BLOCK_HOLDER",
        "MESSAGE_TEXT","YES_NO_LAYOUT","YES_BUTTON","NO_BUTTON","header","TITLE_TEXT"};
    const unsigned parents[]={0,0,1,2,3,4,5,5,7,7,3,10};
    const float geometry[][6]={
        {0,0,0,0,64,64},{0,0,0,0,1280,720},{0,0,0,0,1280,720},
        {423,155,0,0,64,64},{-1068,-155,0,0,2560,720},{897,262,0,0,64,64},
        {375,69,-200,-100,400,200},{257,218,-32,-32,300,32},
        {75,16,-75,-16,150,32},{225,16,-75,-16,150,32},{3,0,0,0,408,94},{203,47,-512,-16,1024,32}};
    for(unsigned i=0;i<12;++i){
        uint64_t node=(uintptr_t)storage[i];wr64(node,GVA(0x141D78A60));wr32(node+0x1e4,0x7900+i);wr64(node+0x1d8,(uintptr_t)names[i]);
        wrss(node+0xc0,geometry[i][0]);wrss(node+0xc4,geometry[i][1]);wrss(node+0xd0,geometry[i][2]);wrss(node+0xd4,geometry[i][3]);
        wrss(node+0xf0,1);wrss(node+0xf4,1);wrss(node+0x100,geometry[i][4]);wrss(node+0x104,geometry[i][5]);
        wrss(node+0x210,geometry[i][4]);wrss(node+0x214,geometry[i][5]);
        if(i){
            uint64_t parent=(uintptr_t)storage[parents[i]],tail=rd64(parent+0x220);wr64(node+0x230,parent);
            if(!tail)wr64(parent+0x220,node);else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,node);}
        }
    }
    uint64_t screen=(uintptr_t)storage[0],ink=(uintptr_t)storage[3],mask=(uintptr_t)storage[4];
    wr64(mask,GVA(0x141D796F8));wrss(mask+0x378,75);wrss(mask+0x37c,10);
    CPU cpu=*caller;cpu.rcx=screen;fp_mobile_camera_destroy();test_ime_fraction=1;test_ime_editor=0;
    const unsigned scales[]={125,150,200,100,200,100};
    for(unsigned pass=0;pass<sizeof(scales)/sizeof(scales[0]);++pass){
        scale=scales[pass];float factor=fminf(scale/100.f,1.5f);
        float x=scale==100?423:640-204*factor,y=scale==100?155:360-178*factor;
        for(unsigned repeat=0;repeat<3;++repeat)fp_mobile_ui_before(&cpu);
        assert(fabsf(rdss(ink+0xc0)-x)<.002f&&fabsf(rdss(ink+0xc4)-y)<.002f&&fabsf(rdss(ink+0xf0)-factor)<.0001f);
        assert(rdss(ink+0x100)==64&&rdss(ink+0x104)==64&&rdss(ink+0x210)==64&&rdss(ink+0x214)==64);
        // Message bounds are source (427,231)..(827,431); the two buttons end
        // at source (777,480). Their complete enlarged controls stay on screen.
        assert(x+4*factor>=12&&x+404*factor<=1268&&y+76*factor>=12&&y+276*factor<=708);
        assert(x+54*factor>=12&&x+354*factor<=1268&&y+293*factor>=12&&y+325*factor<=708);
        assert(fabsf((x+204*factor)-(scale==100?627:640))<.002f); // Title/content centre.
        assert(rdss(mask+0xc0)==-1068&&rdss(mask+0xc4)==-155&&rdss(mask+0x210)==2560&&rdss(mask+0x214)==720);
        for(unsigned i=5;i<12;++i){uint64_t node=(uintptr_t)storage[i];assert(rdss(node+0xc0)==geometry[i][0]&&rdss(node+0xc4)==geometry[i][1]&&rdss(node+0xf0)==1);}
        CPU draw=cpu;draw.rcx=mask;wr8(mask+0x383,0);wr8(mask+0x385,1);
        expected_projection=75;expected_template_width=2560*factor;expected_template_height=720*factor;
        fp_mobile_ui_template(&draw,template_original);assert(rdss(mask+0x100)==2560&&rdss(mask+0x104)==720&&rdss(mask+0x378)==75);
        if(scale>100)check_dialog_affine_draw(&cpu,mask,423,155,x,y,factor);
    }
    expected_template_width=expected_template_height=0;
}
