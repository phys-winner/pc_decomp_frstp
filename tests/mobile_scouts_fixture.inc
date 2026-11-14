static void check_mobile_scouts(CPU *caller){
    unsigned relayout_before=layout_relayout_calls;
    static _Alignas(16) unsigned char storage[11][0x600];
    uint64_t nodes[11];for(unsigned i=0;i<11;++i)nodes[i]=(uintptr_t)storage[i];
    const char *names[]={"","FRAME","EXT_MESSAGE_SCOUTS_PANEL","ANI","Background","content_layout","CONTENT","MESSAGE_TEXT","TITLE_TEXT","CLOSE_BUTTON","info_logo"};
    const unsigned parents[]={0,0,1,2,3,3,5,5,3,3,3};
    for(unsigned i=0;i<11;++i){
        uint64_t node=nodes[i];wr64(node,GVA(i==0?0x141D7B0B0:i==7||i==8?0x141DED7D8:i==5||i==6?0x141DE0A48:i==9?0x141D79F78:0x141D78A60));
        wr64(node+0x1d8,(uintptr_t)names[i]);wr32(node+0x1e4,0x12000+i);wrss(node+0xf0,1);wrss(node+0xf4,1);
        wrss(node+0x100,64);wrss(node+0x104,64);wrss(node+0x210,64);wrss(node+0x214,64);
        if(i){uint64_t parent=nodes[parents[i]],tail=rd64(parent+0x220);wr64(node+0x230,parent);
            if(!tail)wr64(parent+0x220,node);else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,node);}}
    }
    uint64_t root=nodes[2],background=nodes[4],body=nodes[5],content=nodes[6],text=nodes[7],title=nodes[8],close=nodes[9],icon=nodes[10];
    const float sizes[][4]={{1280,720,1280,720},{800,520,800,520},{707,400,406,137},{605,247.8f,64,64},{605,48,616,177},{583,18.225f,583,18.225f},{557,55,557,55},{195,38,195,38}};
    const uint64_t sized[]={nodes[1],root,background,body,content,text,title,close};
    for(unsigned i=0;i<8;++i){wrss(sized[i]+0x100,sizes[i][0]);wrss(sized[i]+0x104,sizes[i][1]);wrss(sized[i]+0x210,sizes[i][2]);wrss(sized[i]+0x214,sizes[i][3]);}
    wrss(root+0xc0,640);wrss(root+0xc4,360);wrss(root+0xd0,-400);wrss(root+0xd4,-260);wrss(root+0xf0,.6f);wrss(root+0xf4,.6f);
    wrss(background+0xc0,28);wrss(background+0xc4,91);wrss(body+0xc0,90);wrss(body+0xc4,172);
    wrss(text+0xc0,11);wrss(text+0xc4,78);wrss(title+0xc0,113);wrss(title+0xc4,82);
    wrss(close+0xc0,397);wrss(close+0xc4,439);wrss(close+0xd0,-97.5f);wrss(close+0xd4,-19);wrss(close+0xf0,1.7f);wrss(close+0xf4,1.7f);
    wrss(icon+0xc0,399);wrss(icon+0xc4,31);wrss(icon+0xd0,-32);wrss(icon+0xd4,-32);
    static const uint16_t message[]={'t','e','s','t',0},empty[]={0};
    wr64(text+0x3e8,(uintptr_t)message);wr64(title+0x3e8,(uintptr_t)empty);
    wrss(text+0x37c,583);wrss(title+0x37c,557);wrss(title+0x380,55);wr8(text+0x384,1);
    CPU cpu=*caller;cpu.rcx=nodes[0];unsigned wrap_before=message_wrap_calls;
    const unsigned scales[]={200,150,200,100,200,100};
    for(unsigned pass=0;pass<6;++pass){
        scale=scales[pass];fp_mobile_ui_before(&cpu);
        if(scale>100){
            float root_bounds[4],text_bounds[4],button_bounds[4];
            law_selected_bounds(root,root_bounds);law_selected_bounds(text,text_bounds);law_selected_bounds(close,button_bounds);
            float canvas_width=rdss(GVA(0x142980e60)),canvas_height=rdss(GVA(0x142980e64));
            assert(root_bounds[0]>=16&&root_bounds[1]>=16&&root_bounds[0]+root_bounds[2]<=canvas_width-16&&root_bounds[1]+root_bounds[3]<=canvas_height-16);
            assert(rdss(text+0x37c)==432&&rdss(text+0x380)==0&&rd8(text+0x384)==1);
            assert(text_bounds[0]>root_bounds[0]&&text_bounds[0]+text_bounds[2]<root_bounds[0]+root_bounds[2]);
            assert(text_bounds[1]+text_bounds[3]<button_bounds[1]&&button_bounds[1]+button_bounds[3]<root_bounds[1]+root_bounds[3]);
            assert(rdss(close+0xf0)==1&&(rd32(text+0x1e0)&4)&&(rd32(content+0x1e0)&4));
            assert(rdss(background+0x100)==459&&fabsf(rdss(background+0x104)-(rdss(root+0x104)-66))<.001f);
            float before_y=root_bounds[1];
            /* Reproduce the physical preset-size feedback while the requested size already matches. */
            wrss(background+0x100,707);wrss(background+0x104,400);
            fp_mobile_ui_before(&cpu);law_selected_bounds(root,root_bounds);assert(fabsf(before_y-root_bounds[1])<.001f);
            assert(rdss(background+0x100)==459&&fabsf(rdss(background+0x104)-(rdss(root+0x104)-66))<.001f);
        }else{
            assert(rdss(text+0x37c)==583&&rdss(title+0x37c)==557&&rdss(title+0x380)==55&&rd8(title+0x384)==0);
            assert(!(rd32(text+0x1e0)&4)&&!(rd32(content+0x1e0)&4));
            assert(rdss(root+0xc0)==640&&rdss(root+0xc4)==360&&fabsf(rdss(root+0xf0)-.6f)<.0001f);
            assert(rdss(background+0x100)==707&&rdss(background+0x104)==400&&rdss(background+0x210)==406&&rdss(background+0x214)==137);
            assert(rdss(body+0x100)==605&&rdss(body+0x104)==247.8f&&rdss(body+0x210)==64);
            assert(rdss(content+0x100)==605&&rdss(content+0x104)==48&&rdss(content+0x210)==616);
            assert(rdss(close+0xc0)==397&&rdss(close+0xc4)==439&&fabsf(rdss(close+0xf0)-1.7f)<.0001f);
        }
    }
    assert(message_wrap_calls>wrap_before);
    wr64(title+0x3e8,(uintptr_t)message);wrss(title+0x4d4,95.175f);scale=200;fp_mobile_ui_before(&cpu);
    float title_bounds[4],text_bounds[4];law_selected_bounds(title,title_bounds);law_selected_bounds(text,text_bounds);
    assert(fabsf(rdss(title+0xf0)-.65f)<.001f&&title_bounds[2]<=356.01f&&title_bounds[1]+title_bounds[3]<text_bounds[1]);
    assert(fabsf(rdss(title+0x37c)-356.f/.65f)<.001f);
    scale=100;fp_mobile_ui_before(&cpu);wr64(title+0x3e8,(uintptr_t)empty);
    /* Long translated paragraphs retain the complete measured text and shrink the complete card only if needed. */
    wrss(text+0x4d4,420);scale=200;fp_mobile_ui_before(&cpu);
    float bounds[4];law_selected_bounds(root,bounds);assert(bounds[1]>=15.99f&&bounds[1]+bounds[3]<=344.01f&&rdss(root+0xf0)<1);
    assert(rdss(text+0x104)==420);
    scale=100;fp_mobile_ui_before(&cpu);
    /* An unrelated panel with the same child names keeps the original layout path. */
    wr64(root+0x1d8,(uintptr_t)"OTHER_SCOUTS_PANEL");scale=200;fp_mobile_ui_before(&cpu);assert(rdss(close+0xf0)==1.7f);
    scale=100;fp_mobile_ui_before(&cpu);
    layout_relayout_calls=relayout_before;
}
