static float law_paint_source[4],law_paint_target[4];
static unsigned law_paint_draws;
static void law_backdrop_original(CPU *cpu) {
    assert(rdss(cpu->rcx+0x378)==62&&rd8(cpu->rcx+0x383)==1);
    ++law_paint_draws;
}
static void law_paint_original(CPU *cpu) {
    uint64_t node=cpu->rcx;const float *matrix=(const float*)(uintptr_t)cpu->rdx;
    float ratio=rdss(node+0x100)/1280;
    assert(!rd8(node+0x383)&&ratio>0&&fabsf(rdss(node+0x104)-720*ratio)<.001f);
    assert(rdss(node+0x378)==75);
    // Native projection must map both source image corners onto the mobile 2D anchor.
    for(unsigned corner=0;corner<2;++corner){
        float x=matrix[12]+rdss(node+0xd0)+ratio*law_paint_source[corner*2];
        float y=matrix[13]+rdss(node+0xd4)+ratio*law_paint_source[corner*2+1];
        assert(fabsf(x-law_paint_target[corner*2])<.002f);
        assert(fabsf(y-law_paint_target[corner*2+1])<.002f);
    }
    ++law_paint_draws;
}

static uint64_t law_selected_node(unsigned id,const char *name,uint64_t parent,float x,float y,float w,float h,float scale) {
    uint64_t node=(uintptr_t)calloc(1,0x600);assert(node);
    wr64(node+0x1d8,(uintptr_t)name);wr32(node+0x1e4,id);wr64(node+0x230,parent);
    wrss(node+0xc0,x);wrss(node+0xc4,y);wrss(node+0xf0,scale);wrss(node+0xf4,scale);
    wrss(node+0x100,w);wrss(node+0x104,h);wrss(node+0x210,w);wrss(node+0x214,h);
    if(parent){
        uint64_t tail=rd64(parent+0x220);
        if(!tail)wr64(parent+0x220,node);
        else{while(rd64(tail+0x240))tail=rd64(tail+0x240);wr64(tail+0x240,node);}
    }
    return node;
}

static void law_selected_bounds(uint64_t node,float bounds[4]) {
    uint64_t chain[40];unsigned count=0;
    for(uint64_t current=node;current;current=rd64(current+0x230)){
        assert(count<40);chain[count++]=current;
    }
    float sx=1,sy=1;bounds[0]=bounds[1]=0;
    while(count){
        uint64_t current=chain[--count];
        bounds[0]+=sx*(rdss(current+0xc0)+rdss(current+0xd0)*rdss(current+0xf0));
        bounds[1]+=sy*(rdss(current+0xc4)+rdss(current+0xd4)*rdss(current+0xf4));
        sx*=rdss(current+0xf0);sy*=rdss(current+0xf4);
    }
    bounds[2]=rdss(node+0x100)*sx;bounds[3]=rdss(node+0x104)*sy;
}

static void law_selected_fixture(CPU *cpu,uint64_t screen,uint64_t frame) {
    uint64_t previous=rd64(frame+0x220),saved_rcx=cpu->rcx;wr64(frame+0x220,0);
    float frame_sx=rdss(frame+0xf0),frame_sy=rdss(frame+0xf4);
    wrss(frame+0xf0,1);wrss(frame+0xf4,1);
    _Alignas(16) unsigned char camera_system[0x340]={0},camera[0x240]={0};
    wr64((uintptr_t)camera_system+0xa0,(uintptr_t)camera);wrss((uintptr_t)camera+0x30,100);wrss((uintptr_t)camera+0x2c,1);
    cpu->rcx=(uintptr_t)camera_system;fp_mobile_camera_before(cpu);
    uint64_t nodes[48];unsigned count=0;
    uint64_t root=nodes[count++]=law_selected_node(0x5010,"EXT_SOCIOTECH_PANEL",frame,640,360,1280,720,1);
    wrss(root+0xd0,-640);wrss(root+0xd4,-360);
    uint64_t anim=nodes[count++]=law_selected_node(0x5011,"ANIM",root,0,0,1280,720,1);
    uint64_t tree=nodes[count++]=law_selected_node(0x5012,"TREE_VIEW",anim,0,0,1280,720,1);
    uint64_t content=nodes[count++]=law_selected_node(0x5013,"CONTENT",tree,0,0,1280,720,1);
    uint64_t info=nodes[count++]=law_selected_node(0x5014,"INFO_PANEL",content,997,194,320,680,.9f);
    uint64_t art_mask=nodes[count++]=law_selected_node(0x5015,"SFX_MASK",info,0,0,64,64,1);
    uint64_t art=nodes[count++]=law_selected_node(0x5016,"SFX_MAIN",art_mask,0,0,0,0,1);
    uint64_t picture=nodes[count++]=law_selected_node(0x5017,"BIG_PICTURE",art_mask,-484,-215,800,800,1);
    wr32(picture+0x1e0,2); // The real selected law renders SFX_MAIN and hides this picture.
    uint64_t text_mask=nodes[count++]=law_selected_node(0x5018,"SFX_MASK",info,-12,-13,64,64,1);
    uint64_t table=nodes[count++]=law_selected_node(0x5019,"Table",text_mask,-55,265,412,363,1);
    uint64_t action=nodes[count++]=law_selected_node(0x501a,"ACTIVATE_SOCIOTECH_BUTTON",text_mask,206,570,220,40,1);
    wrss(action+0xd0,-110);wrss(action+0xd4,-20);
    uint64_t top=nodes[count++]=law_selected_node(0x501b,"top",tree,0,0,1280,720,1);
    uint64_t back=nodes[count++]=law_selected_node(0x501c,"BACK_BUTTON",top,100,500,156,40,1);
    wrss(back+0xd0,-78);wrss(back+0xd4,-20);
    uint64_t background=nodes[count++]=law_selected_node(0x501d,"background",anim,0,0,64,64,1);
    uint64_t backdrop_mask=nodes[count++]=law_selected_node(0x501e,"SFX_MASK_SPLINTERS",background,0,0,64,64,1);
    wr8(backdrop_mask+0x383,1);wr8(backdrop_mask+0x385,1);wrss(backdrop_mask+0x378,62);
    uint64_t backdrop=nodes[count++]=law_selected_node(0x501f,"BackgroundBrown",backdrop_mask,640,360,1700,1200,1);
    wrss(backdrop+0xd0,-850);wrss(backdrop+0xd4,-600);
    uint64_t categories=nodes[count++]=law_selected_node(0x5020,"CATEGORIES_VIEW",anim,0,0,1280,720,1);
    uint64_t category_content=nodes[count++]=law_selected_node(0x5021,"CONTENT",categories,0,0,1280,600,1);
    uint64_t category_layout=nodes[count++]=law_selected_node(0x5022,"CATEGORIES_LAYOUT",category_content,63,-1,1163.95f,487,1);
    uint64_t category=nodes[count++]=law_selected_node(0x5023,"CATEGORY_BUTTON",category_layout,581.975f,300,488,600,1);
    wrss(category+0xd0,-244);wrss(category+0xd4,-300);
    uint64_t category_graphic=nodes[count++]=law_selected_node(0x5024,"graphic",category,93,0,301,175,1);
    uint64_t category_title=nodes[count++]=law_selected_node(0x5028,"TITLE_TEXT",category,105,50,279,24,1);
    uint64_t category_description=nodes[count++]=law_selected_node(0x5029,"SHORT_DESCRIPTION_TEXT",category,137,91,210,31,1);
    uint64_t labels[]={category_title,category_description};static const uint16_t label_text[]={'L','a','w',0};
    for(unsigned i=0;i<2;++i){
        wr64(labels[i],GVA(0x141DED7D8));wr64(labels[i]+0x3e8,(uintptr_t)label_text);
        wrss(labels[i]+0x37c,i?210:279);wrss(labels[i]+0x380,i?31:24);
        wrss(labels[i]+0x4d0,i?120:90);wrss(labels[i]+0x4d4,i?31:24);
    }
    uint64_t scroll=nodes[count++]=law_selected_node(0x5025,"SOCIOTECHTREE",content,0,0,1280,722,1);
    uint64_t other_law=nodes[count++]=law_selected_node(0x5026,"TECH_ENTRY",content,590,160,20,40,1);
    wr64(other_law,GVA(0x141D79F78));
    uint64_t second_category=nodes[count++]=law_selected_node(0x5027,"CATEGORY_BUTTON",category_layout,1000,300,488,600,1);
    wrss(second_category+0xd0,-244);wrss(second_category+0xd4,-300);wr32(second_category+0x1e0,2);
    _Alignas(16) unsigned char law_camera[0x300]={0};
    uint64_t view_reference=scroll;wr64((uintptr_t)law_camera+0xd8,(uintptr_t)&view_reference);wr32((uintptr_t)law_camera+0xd0,2);
    const float camera_initial[4]={638,361,0,1},camera_selected[4]={316,213,0,1},camera_viewport[4]={1280,722,0,0};
    memcpy(law_camera+0x260,camera_initial,16);memcpy(law_camera+0x270,camera_selected,16);memcpy(law_camera+0x280,camera_viewport,16);
    cpu->rcx=screen;scale=100;fp_mobile_ui_before(cpu);
    CPU law_cpu=*cpu;law_cpu.rcx=(uintptr_t)law_camera;fp_mobile_law_camera_init(&law_cpu,created_original);
    assert(!memcmp(law_camera+0x260,camera_initial,16)&&!memcmp(law_camera+0x270,camera_selected,16));
    assert(!memcmp(law_camera+0x280,camera_viewport,16));
    uint64_t masks[]={art_mask,art,text_mask};
    for(unsigned i=0;i<3;++i){wr8(masks[i]+0x383,1);wr8(masks[i]+0x385,1);wrss(masks[i]+0x378,75);}
    cpu->rcx=screen;scale=200;fp_mobile_ui_before(cpu);
    float table_scale=336.f/458;
    assert(fabsf(rdss(text_mask+0xf0)-table_scale)<.0001f&&fabsf(rdss(table+0xf0)-1)<.0001f);
    assert(fabsf(rdss(art_mask+0xf0)-144.f/800)<.0001f&&fabsf(rdss(art+0xf0)-1)<.0001f);
    float action_bounds[4],back_bounds[4],portrait_bounds[4];
    law_selected_bounds(action,action_bounds);law_selected_bounds(back,back_bounds);law_selected_bounds(picture,portrait_bounds);
    assert(fabsf(action_bounds[1]-210)<.002f&&fabsf(back_bounds[1]-318)<.002f);
    assert(portrait_bounds[1]+portrait_bounds[3]<action_bounds[1]);
    assert(action_bounds[1]+action_bounds[3]<284-24);
    assert(284+24<back_bounds[1]);
    float backdrop_bounds[4],category_bounds[4],graphic_bounds[4];
    law_selected_bounds(backdrop,backdrop_bounds);law_selected_bounds(category,category_bounds);law_selected_bounds(category_graphic,graphic_bounds);
    assert(fabsf(backdrop_bounds[0]+105)<.002f&&fabsf(backdrop_bounds[1]+120)<.002f);
    assert(backdrop_bounds[2]==850&&backdrop_bounds[3]==600);
    assert(graphic_bounds[2]==301&&graphic_bounds[2]>category_bounds[2]);
    assert(graphic_bounds[0]>0&&graphic_bounds[0]+graphic_bounds[2]<640);
    float title_bounds[4],description_bounds[4];
    law_selected_bounds(category_title,title_bounds);law_selected_bounds(category_description,description_bounds);
    float category_center=category_bounds[0]+category_bounds[2]*.5f;
    assert(fabsf(title_bounds[0]+90*.9f*.5f-category_center)<.002f);
    assert(fabsf(description_bounds[0]+120*1.2f*.5f-category_center)<.002f);
    float plate_top=graphic_bounds[1]+41*graphic_bounds[3]/175;
    float plate_bottom=plate_top+44*graphic_bounds[3]/175;
    assert(description_bounds[1]>=plate_top+5.99f&&description_bounds[1]+description_bounds[3]<=plate_bottom-5.99f);
    assert(title_bounds[1]+title_bounds[3]<description_bounds[1]);
    assert(fabsf(title_bounds[3]/rdss(category_title+0x104)-.9f)<.002f);
    assert(fabsf(description_bounds[3]/rdss(category_description+0x104)-1.2f)<.002f);
    float matrix[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    CPU paint=*cpu;paint.rdx=(uintptr_t)matrix;
    const float art_source[]={561.4f,.5f,1281.4f,720.5f},art_target[]={12,60,156,204};
    memcpy(law_paint_source,art_source,sizeof(art_source));memcpy(law_paint_target,art_target,sizeof(art_target));
    for(unsigned i=0;i<2;++i){
        paint.rcx=masks[i];fp_mobile_ui_template(&paint,law_paint_original);
        assert(paint.rdx==(uintptr_t)matrix&&rd8(masks[i]+0x383)==1);
        assert(rdss(masks[i]+0x100)==(i?0:64)&&rdss(masks[i]+0x378)==75);
    }
    const float text_source[]={936.7f,420.8f,1307.5f,747.5f};
    memcpy(law_paint_source,text_source,sizeof(text_source));
    law_paint_target[0]=217.6f;law_paint_target[1]=12+95*table_scale;
    law_paint_target[2]=217.6f+412*table_scale;law_paint_target[3]=12+(95+363)*table_scale;
    paint.rcx=text_mask;fp_mobile_ui_template(&paint,law_paint_original);
    assert(law_paint_draws==3&&paint.rdx==(uintptr_t)matrix&&rdss(text_mask+0x100)==64&&rd8(text_mask+0x383)==1);
    paint.rcx=backdrop_mask;fp_mobile_ui_template(&paint,law_backdrop_original);assert(law_paint_draws==4);
    law_cpu.rcx=(uintptr_t)law_camera;fp_mobile_law_camera(&law_cpu,created_original);
    assert(rdss((uintptr_t)law_camera+0x260)==318&&rdss((uintptr_t)law_camera+0x264)==152);
    assert(rdss((uintptr_t)law_camera+0x280)==636&&rdss((uintptr_t)law_camera+0x284)==304);
    uint64_t controls[]={scroll,0,other_law,0},previous_list=rd64(GVA(0x142A89088));uint32_t previous_count=rd32(GVA(0x142A89090));
    wr64(GVA(0x142A89088),(uintptr_t)controls);wr32(GVA(0x142A89090),2);
    fp_mobile_ui_before(cpu);
    assert(frostpunk_mobile_swipe_target(.5f,.5f,2)==0);
    assert(frostpunk_mobile_swipe_target(.99f,.03f,2)==0);
    assert(frostpunk_mobile_swipe_target(.94f,.5f,2)==0);
    assert(frostpunk_mobile_swipe_target(.99f,.5f,2)==UINT64_MAX);
    fp_mobile_ui_before(cpu);assert(rd32((uintptr_t)law_camera+0xd0)==1);
    wr32((uintptr_t)law_camera+0xd0,2);fp_mobile_ui_before(cpu);
    assert(frostpunk_mobile_swipe_target(.99f,.5f,2)==UINT64_MAX);
    wr32(root+0x1e0,2);fp_mobile_ui_before(cpu);assert(rd32((uintptr_t)law_camera+0xd0)==2);
    wr32(root+0x1e0,0);
    wr64(GVA(0x142A89088),previous_list);wr32(GVA(0x142A89090),previous_count);
    wr32(second_category+0x1e0,0);fp_mobile_ui_before(cpu);law_selected_bounds(category_graphic,graphic_bounds);
    assert(graphic_bounds[2]>200&&graphic_bounds[2]<category_bounds[2]);
    law_selected_bounds(category_title,title_bounds);law_selected_bounds(category_description,description_bounds);
    assert(fabsf(title_bounds[0]+90*.9f*.5f-category_center)<.002f);
    plate_top=graphic_bounds[1]+41*graphic_bounds[3]/175;plate_bottom=plate_top+44*graphic_bounds[3]/175;
    assert(description_bounds[1]>=plate_top+5.99f&&description_bounds[1]+description_bounds[3]<=plate_bottom-5.99f);
    assert(title_bounds[1]+title_bounds[3]<description_bounds[1]);
    // Native callback ownership works even when the controller's scroll pane is absent from the input registry.
    _Alignas(16) unsigned char callback[32]={0};uint64_t owner=(uintptr_t)law_camera;
    wr64((uintptr_t)callback,(uintptr_t)&owner);wr64((uintptr_t)callback+8,GVA(0x141A5FBB0));
    wr64(action,GVA(0x141D79F78));wr64(action+0x258,(uintptr_t)callback);wr32(action+0x260,1);
    controls[0]=action;wr64(GVA(0x142A89088),(uintptr_t)controls);wr32(GVA(0x142A89090),1);
    wr32((uintptr_t)law_camera+0xd0,2);fp_mobile_ui_before(cpu);
    assert(frostpunk_mobile_swipe_target(.99f,.5f,2)==UINT64_MAX);
    fp_mobile_ui_before(cpu);assert(rd32((uintptr_t)law_camera+0xd0)==1);
    // Actual native paths can bypass both camera wrappers. Bind a fresh controller solely through its callback.
    _Alignas(16) unsigned char unhooked_camera[0x300]={0};owner=(uintptr_t)unhooked_camera;
    wr64(owner+0xd8,(uintptr_t)&view_reference);wr32(owner+0xd0,2);
    memcpy(unhooked_camera+0x260,camera_initial,16);memcpy(unhooked_camera+0x270,camera_selected,16);memcpy(unhooked_camera+0x280,camera_viewport,16);
    fp_mobile_ui_before(cpu);assert(rdss(owner+0x280)==636&&rdss(owner+0x284)==304);
    wr32(root+0x1e0,2);scale=100;fp_mobile_ui_before(cpu);
    assert(!memcmp(unhooked_camera+0x260,camera_initial,16)&&!memcmp(unhooked_camera+0x270,camera_selected,16));
    assert(!memcmp(unhooked_camera+0x280,camera_viewport,16)); // Hidden live widgets restore the camera before reopening.
    wr32(root+0x1e0,0);
    scale=100;fp_mobile_ui_before(cpu);
    wr64(GVA(0x142A89088),previous_list);wr32(GVA(0x142A89090),previous_count);
    law_cpu.rcx=(uintptr_t)law_camera;fp_mobile_law_camera(&law_cpu,created_original);
    assert(!memcmp(law_camera+0x260,camera_initial,16)&&!memcmp(law_camera+0x270,camera_selected,16));
    assert(!memcmp(law_camera+0x280,camera_viewport,16));
    assert(rdss(info+0xc0)==997&&rdss(info+0xc4)==194&&fabsf(rdss(info+0xf0)-.9f)<.0001f);
    assert(rdss(table+0xc0)==-55&&rdss(table+0xc4)==265&&rdss(table+0xf0)==1);
    assert(rdss(art_mask+0xc0)==0&&rdss(art_mask+0xf0)==1&&rdss(art+0xf0)==1);
    assert(rdss(picture+0xc0)==-484&&(rd32(picture+0x1e0)&2));
    assert(rdss(action+0xc0)==206&&rdss(action+0xc4)==570&&rdss(action+0xf0)==1&&rdss(action+0xf4)==1);
    assert(rdss(back+0xc0)==100&&rdss(back+0xc4)==500&&rdss(back+0xf0)==1&&rdss(back+0xf4)==1);
    assert(rdss(backdrop+0xc0)==640&&rdss(backdrop+0xc4)==360&&rdss(backdrop+0xf0)==1&&rdss(backdrop+0xf4)==1);
    assert(rdss(category_graphic+0xc0)==93&&rdss(category_graphic+0xc4)==0&&rdss(category_graphic+0xf0)==1);
    for(unsigned i=0;i<2;++i){
        assert(rdss(labels[i]+0xf0)==1&&rdss(labels[i]+0x37c)==(i?210:279));
        assert(rdss(labels[i]+0x380)==(i?31:24)&&!rd8(labels[i]+0x384));
        assert(rdss(labels[i]+0xc0)==(i?137:105)&&rdss(labels[i]+0xc4)==(i?91:50));
    }
    fp_mobile_camera_destroy();wr64(frame+0x220,previous);cpu->rcx=saved_rcx;
    wrss(frame+0xf0,frame_sx);wrss(frame+0xf4,frame_sy);
    for(unsigned i=0;i<count;++i)free((void*)(uintptr_t)nodes[i]);
}
