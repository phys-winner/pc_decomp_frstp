/* Keep native tables and their hit testing; only the viewport becomes shorter.
 * Rows remain in the original scroll pane, including sorting and citizen focus. */
static unsigned phone_hud_covered;
static unsigned phone_side_hud(const char *name){
    return name&&(!strcmp(name,"EXT_NOTIFICATION_PANEL")||!strcmp(name,"EXT_EXPEDITIONS_PANEL")||!strcmp(name,"EXT_QUEST_PANEL"));
}
static void phone_cover_scan(uint64_t node,unsigned depth,unsigned *budget,uint64_t targets[3],unsigned *count){
    if(!node||depth>16||!*budget)return;
    --*budget;const char *name=law_photo_name(node);
    if(phone_side_hud(name)){if(*count<3)targets[(*count)++]=node;return;}
    if(name&&(!strcmp(name,"EXT_SELECTION_PANEL")||!strcmp(name,"EXT_SEARCH_CITIZENS_PANEL")||!strcmp(name,"EXT_CONSTRUCTION_PANEL"))){
        uint64_t parent=node;unsigned n=0;
        for(;parent&&n<40;++n,parent=rd64(parent+0x230))
            if((rd32(parent+0x30)|rd32(parent+0x1e0)|rd8(parent+0x34))&2)break;
        if(!parent)phone_hud_covered=1;
        return;
    }
    if(name&&!strncmp(name,"EXT_",4))return;
    unsigned children=0;
    for(uint64_t child=rd64(node+0x220);child&&children++<512;child=rd64(child+0x240))phone_cover_scan(child,depth+1,budget,targets,count);
}
static void phone_hud_visibility(CPU *cpu,uint64_t screen){
    uint64_t frame=law_photo_child(screen,"FRAME"),hud=frame?law_photo_child(frame,"HUD"):0;
    uint64_t targets[3];unsigned count=0,budget=512;phone_hud_covered=0;
    phone_cover_scan(hud,0,&budget,targets,&count);
    phone_hud_covered=phone_hud_covered&&applied_scale>100;
    for(unsigned i=0;i<count;++i){mobile_layout *base=remember_layout(targets[i]);if(!base)continue;
        // Keep queued city notices intact while a phone panel occupies their touch area.
        layout_scale(cpu,targets[i],phone_hud_covered?0:base->sx,phone_hud_covered?0:base->sy);
    }
}

typedef struct {uint64_t node;uint32_t id;float minimum;} phone_table_saved;
static phone_table_saved phone_tables[128];
static unsigned phone_table_count;

static void phone_table_rows(CPU *cpu,uint64_t table){
    uint32_t id=rd32(table+0x1e4);unsigned i=0;
    for(;i<phone_table_count;++i)if(phone_tables[i].node==table&&phone_tables[i].id==id)break;
    if(i==phone_table_count){if(i==128)return;phone_tables[phone_table_count++]=(phone_table_saved){table,id,rdss(table+0x460)};}
    float minimum=applied_scale>100?fmaxf(26,phone_tables[i].minimum):phone_tables[i].minimum;
    unsigned changed=fabsf(rdss(table+0x460)-minimum)>.01f;wrss(table+0x460,minimum);
    uint64_t rows=rd64(table+0x2f0);unsigned count=rd32(table+0x2f8);
    if(!rows||count>4096)return;
    for(unsigned n=0;n<count;++n){uint64_t row=rd64(rows+8*n);if(!row)continue;
        if(!changed&&rdss(row+52)>=minimum)continue;
        float measured=minimum;uint64_t cells=rd64(row+72);unsigned columns=rd32(row+80);
        if(cells&&columns<=32)for(unsigned c=0;c<columns;++c){uint64_t cell=rd64(cells+8*c);if(cell)measured=fmaxf(measured,rdss(cell+52));}
        if((changed||rdss(row+52)<minimum)&&fabsf(rdss(row+52)-measured)>.01f)
            call(cpu,UINT64_C(0x14109CFA0),row,NULL,measured,0,0);
    }
}

typedef struct {uint64_t node;uint32_t id;float size,fitted;} phone_font_saved;
static phone_font_saved phone_fonts[4096];
static ui_index_slot phone_font_index[8192];
static unsigned phone_font_count;
static void phone_text_size(CPU *cpu,uint64_t node,float minimum){
    if(rd64(node)==GVA(0x141DED7D8)&&!rd8(node+0x40e)){
        uint32_t id=rd32(node+0x1e4);int i=ui_index_find(phone_font_index,8191u,node,id);
        float size=rdss(node+0x3a0);
        if(i<0&&size>0&&size<minimum&&phone_font_count<4096){
            i=(int)phone_font_count++;phone_fonts[i]=(phone_font_saved){node,id,size,0};ui_index_add(phone_font_index,8191u,node,id,i);
        }
        if(i>=0){
            float target=applied_scale>100?(phone_fonts[i].fitted>0?phone_fonts[i].fitted:fmaxf(minimum,phone_fonts[i].size)):phone_fonts[i].size;
            if(fabsf(size-target)>.01f){
                call_value(cpu,UINT64_C(0x140FB3C50),node,0);wrss(node+0x3a0,target);
                call_value(cpu,UINT64_C(0x140FB4090),node,0);
            }
        }
    }
}
/* phone_text_size, then a one-line label (no wrap, +0x384 == 0) that the larger glyphs push past its own
 * box is brought back to the largest size that fits, never below the authored one. */
static void phone_text_fit(CPU *cpu,uint64_t node,float minimum){
    phone_text_size(cpu,node,minimum);
    if(applied_scale<=100||rd64(node)!=GVA(0x141DED7D8)||rd8(node+0x40e)||rd8(node+0x384))return;
    uint32_t id=rd32(node+0x1e4);int i=ui_index_find(phone_font_index,8191u,node,id);if(i<0)return;
    float size=rdss(node+0x3a0),ink=rdss(node+0x4d0),box=rdss(node+0x100),original=phone_fonts[i].size;
    if(!isfinite(ink)||!isfinite(box)||ink<=0||box<=0||ink<=box*1.02f||size<=original)return;
    float target=fmaxf(original,floorf(size*box/ink*4)/4);
    phone_fonts[i].fitted=target;
    if(fabsf(size-target)>.01f){call_value(cpu,UINT64_C(0x140FB3C50),node,0);wrss(node+0x3a0,target);call_value(cpu,UINT64_C(0x140FB4090),node,0);}
}
static void phone_scroll_refresh(CPU *cpu,uint64_t node);
static void phone_economy_text(CPU *cpu,uint64_t node,unsigned depth,unsigned *budget){
    if(!node||depth>32||!*budget||(rd32(node+0x1e0)&2))return;
    /* Readable on a phone: the panel already fills the screen height at about the desktop scale. */
    --*budget;phone_text_fit(cpu,node,21);
    const char *name=law_photo_name(node);
    if(frostpunk_switch_ui()&&name&&!strcmp(name,"PLACES_LIST_PANEL")){
        uint64_t mask=law_photo_child(node,"Table_mask"),pane=mask?law_photo_child(mask,"SCROLL_PANE"):0;
        uint64_t content=pane?law_photo_child(pane,"CONTENT_LAYOUT"):0;
        law_photo_saved *base=pane?law_photo_remember(pane):NULL;
        if(base&&content&&rdss(content+0x100)>base->base.w){
            float width=rdss(content+0x100),factor=base->base.w/width;
            // Switch viewports are narrower; keep every column of the PC controller's generated rows.
            law_photo_saved *mask_base=law_photo_remember(mask);
            law_photo_size(cpu,pane,width,rdss(pane+0x104));
            if(mask_base)law_photo_size(cpu,mask,mask_base->base.w+width-base->base.w,rdss(mask+0x104));
            uint64_t bar=law_photo_child(node,"SCROLL_BAR");law_photo_saved *bar_base=bar?law_photo_remember(bar):NULL;
            if(bar_base){layout_position(cpu,bar,bar_base->base.x+width-base->base.w,bar_base->base.y);phone_scroll_refresh(cpu,bar);}
            layout_scale(cpu,node,factor,factor);
        }
    }
    if(name&&!strcmp(name,"PLACE_ROW_ELEMENT")){
        law_photo_saved *base=law_photo_remember(node);
        if(base){
            float height=applied_scale>100?fmaxf(56,base->base.h):base->base.h;
            unsigned changed=fabsf(rdss(node+0x104)-height)>.01f;
            law_photo_size(cpu,node,base->base.w,height);
            const char *parts[]={"row_background","row_underscore","row_columns_layout"};
            for(unsigned i=0;i<3;++i){uint64_t child=law_photo_child(node,parts[i]);law_photo_saved *saved=child?law_photo_remember(child):NULL;
                if(!saved)continue;
                if(applied_scale<=100)law_photo_restore(cpu,child);
                else if(i==0)law_photo_size(cpu,child,saved->base.w,height);
                else layout_position(cpu,child,saved->base.x,saved->base.y+(height-base->base.h)*(i==1?1:.5f));
            }
            uint64_t parent=rd64(node+0x230);
            if(changed&&parent&&rd64(parent)==GVA(0x141DE0A48))call_value(cpu,UINT64_C(0x141096AE0),parent,0);
        }
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))phone_economy_text(cpu,child,depth+1,budget);
}

typedef struct {uint64_t node,children;uint32_t id;unsigned char shrink,exclude_hidden;float height;} phone_flow_saved;
static phone_flow_saved phone_flows[256];
static unsigned phone_flow_count;
static unsigned phone_citizen_table(uint64_t node){
    uint64_t table=law_photo_child(node,"TABLE");
    return table&&rd64(table)==GVA(0x141DE15E0);
}
static void phone_tab_flow(CPU *cpu,uint64_t node,const char *name){
    if(!name)return;
    unsigned tab=!strcmp(name,"TAB_CONTROLLER")||!strncmp(name,"EXT_TAB_",8);
    unsigned content=!strcmp(name,"CONTENT")&&law_photo_ancestor(node,"TAB_CONTROLLER");
    // ENTITY_LIST_TABLE is a scroll view: shrinking its viewport to the scrolled table feeds the scroll
    // range back into the viewport and runs both off to -2500 (empty steam/visitor/depot lists).
    if(!tab&&!content&&!(phone_citizen_table(node)&&rd64(node)!=GVA(0x1420C25C8)))return;
    unsigned i=0;uint32_t id=rd32(node+0x1e4);
    for(;i<phone_flow_count;++i)if(phone_flows[i].node==node&&phone_flows[i].id==id)break;
    if(i==phone_flow_count){if(i==256)return;phone_flows[phone_flow_count++]=(phone_flow_saved){.node=node,.id=id,.shrink=rd8(node+0x2a9),.exclude_hidden=rd8(node+0x2aa),.height=rdss(node+0x104)};}
    unsigned shrink=applied_scale>100?1:phone_flows[i].shrink;
    unsigned exclude=applied_scale>100?1:phone_flows[i].exclude_hidden;
    uint64_t children=UINT64_C(14695981039346656037);unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
        if((rd32(child+0x1e0)&6)||(rd8(child+0x34)&2)||(rd32(child+0x30)&2))continue;
        unsigned offsets[]={0x1e4,0x100,0x104,0xf0,0xf4};
        for(unsigned n=0;n<5;++n)children=(children^rd32(child+offsets[n]))*UINT64_C(1099511628211);
    }
    // Actual child sizes do not invalidate the native parent's cached measure.
    unsigned changed=applied_scale>100&&phone_flows[i].children!=children;
    phone_flows[i].children=children;
    if(rd8(node+0x2a9)!=shrink||rd8(node+0x2aa)!=exclude){
        wr8(node+0x2a9,shrink);wr8(node+0x2aa,exclude);
        if(applied_scale<=100){float size[4]={rdss(node+0x100),phone_flows[i].height,rdss(node+0x108),1};call(cpu,UINT64_C(0x140F99FB0),node,size,0,0,0);}
        changed=1;
    }
    if(changed)call_value(cpu,UINT64_C(0x141096AE0),node,0);
}

static void phone_scroll_refresh(CPU *cpu,uint64_t node){
    if(rd64(node)!=GVA(0x141EDB0F8))return;
    uint64_t pane_ref=rd64(node+0x2e0),bar_ref=rd64(node+0x318);
    uint64_t pane=pane_ref?rd64(pane_ref):0,bar=bar_ref?rd64(bar_ref):0;
    if(!pane||!bar)return;
    if(memcmp((void*)(uintptr_t)(node+0x2f0),(void*)(uintptr_t)(pane+0x100),16)
       ||memcmp((void*)(uintptr_t)(node+0x320),(void*)(uintptr_t)(bar+0x100),16)){
        // Size bindings cache the PC dimensions; native anchors change only the actual geometry.
        memcpy((void*)(uintptr_t)(node+0x2f0),(void*)(uintptr_t)(pane+0x100),16);
        memcpy((void*)(uintptr_t)(node+0x320),(void*)(uintptr_t)(bar+0x100),16);
        call_value(cpu,UINT64_C(0x1413B1D00),node,1);call_value(cpu,UINT64_C(0x1413B1C20),node,1);
    }
}

static void phone_list_contents(CPU *cpu,uint64_t node,unsigned depth,unsigned *budget,unsigned search,float table_height){
    if(!node||depth>32||!*budget||(rd32(node+0x1e0)&2))return;
    --*budget;
    const char *name=law_photo_name(node);uint64_t type=rd64(node);
    if(!search)phone_tab_flow(cpu,node,name);
    if(type==GVA(0x141DE15E0)){
        law_photo_saved *base=law_photo_remember(node);
        if(base){
            if(applied_scale>100){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,base->base.w,fminf(base->base.h,table_height));}
            else law_photo_restore(cpu,node);
            phone_table_rows(cpu,node);
        }
        // The table owns the generated row geometry, not this traversal.
        return;
    }
    if(name&&(search&&!strcmp(name,"TABLE_PARENT")||!strcmp(name,"ENTITY_LIST_TABLE"))){
        law_photo_saved *base=law_photo_remember(node);
        if(base){
            if(applied_scale>100){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,base->base.w,fminf(base->base.h,table_height+(search?0:24)));}
            else law_photo_restore(cpu,node);
        }
    }
    if(name&&!strcmp(name,"SCROLL_BAR")&&(search||law_photo_ancestor(node,"ENTITY_LIST_TABLE")||phone_citizen_table(rd64(node+0x230)))){
        law_photo_saved *base=law_photo_remember(node);
        if(base){if(applied_scale>100)law_photo_size(cpu,node,table_height/.9f+20,base->base.h);else law_photo_restore(cpu,node);}
        phone_scroll_refresh(cpu,node);
    }
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<512;child=rd64(child+0x240))phone_list_contents(cpu,child,depth+1,budget,search,table_height);
}

static unsigned phone_search_panel(CPU *cpu,uint64_t root,float width,float height,unsigned *budget){
    const char *name=law_photo_name(root);if(!name||strcmp(name,"EXT_SEARCH_CITIZENS_PANEL"))return 0;
    float table_height=fmaxf(120,height-180);
    phone_list_contents(cpu,root,0,budget,1,table_height);
    uint64_t animation=law_photo_child(root,"ANI"),background=animation?law_photo_child(animation,"Background"):0;
    if(applied_scale<=100){law_photo_restore(cpu,root);if(background)law_photo_restore(cpu,background);return 0;}
    if(law_photo_remember(root)){
        law_photo_unanchor(cpu,root);law_photo_size(cpu,root,342,table_height+60);
        float factor=fminf(1,(width-48)/380.f);
        law_photo_place(cpu,root,width-366*factor,60,factor,factor);
    }
    if(background&&law_photo_remember(background))law_photo_size(cpu,background,360,table_height+2);
    return 1;
}
