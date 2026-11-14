typedef struct { float table_left,table_scale,table_top,art_size,tree_top,tree_height,action_scale,law_action_top,law_selected_y; } law_photo_geometry;

static law_photo_geometry law_photo_measure(float width,float height) {
    law_photo_geometry g={0};
    if(!isfinite(width)||!isfinite(height)||width<240||height<160)return g;
    g.table_left=fminf(220,width*.34f);
    g.table_scale=fminf(1,fminf((height-24)/458.f,(width-g.table_left-12)/412.f));
    g.table_top=12+95*g.table_scale;
    g.art_size=fminf(200,fminf(g.table_left-24,height-100));  /* the sign button moved into the table */
    g.tree_top=52;g.tree_height=height-56;
    g.action_scale=fminf(1.4f,(width-24)/(4*108.f));
    g.law_action_top=60+g.art_size+6;g.law_selected_y=height-76;
    return g;
}

static unsigned law_photo_full_canvas(unsigned family,const char *name,const char *parent,const char *grandparent) {
    if(!name)return 0;
    const char *root=family==1?"EXT_SOCIOTECH_PANEL":"EXT_PHOTO_MODE_PANEL";
    if(!strcmp(name,root))return 1;
    if(!parent)return 0;
    unsigned root_parent=!strcmp(parent,root)||(!strcmp(parent,"FRAME")&&grandparent&&!strcmp(grandparent,root));
    unsigned animation_parent=!strcmp(parent,"ANIM")&&grandparent
        &&(!strcmp(grandparent,root)||!strcmp(grandparent,"FRAME"));
    if(!strcmp(name,"FRAME"))return !strcmp(parent,root);
    if(!strcmp(name,"ANIM"))return root_parent;
    if(family==1)return animation_parent&&(!strcmp(name,"TREE_VIEW")||!strcmp(name,"CATEGORIES_VIEW"));
    return (animation_parent&&(!strcmp(name,"SETTINGS_PANEL")||!strcmp(name,"ORNAMENTS_PANEL")))
        ||(!strcmp(name,"SETTINGS_DETAILED")&&!strcmp(parent,"SETTINGS_PANEL"))
        ||(!strcmp(name,"LOGO_PANEL")&&!strcmp(parent,"ORNAMENTS_PANEL"));
}

#ifndef FP_LAW_LAYOUT_GEOMETRY_ONLY
typedef struct {
    uint64_t node;uint32_t id,dock,anchors,active;float viewport_width,viewport_height;
    mobile_layout base;
    float text_wrap[2];unsigned text_limit,text_saved;
} law_photo_saved;
static law_photo_saved law_photo_saved_nodes[2048];
static ui_index_slot law_photo_saved_index[4096];
static unsigned law_photo_saved_count,law_photo_placing;
typedef struct {uint64_t object,view;uint32_t id,active;float initial[4],selected[4],viewport[4];} law_photo_camera_saved;
static law_photo_camera_saved law_photo_cameras[16];
static unsigned law_photo_camera_count;
static ui_index_slot law_photo_camera_index[32];

static const char *law_photo_name(uint64_t node){return (const char*)(uintptr_t)rd64(node+0x1d8);}

static uint64_t law_photo_ancestor(uint64_t node,const char *wanted) {
    for(unsigned depth=0;node&&depth<32;++depth,node=rd64(node+0x230)){
        const char *name=law_photo_name(node);
        if(name&&!strcmp(name,wanted))return node;
    }
    return 0;
}

static unsigned law_photo_family(uint64_t node) {
    for(unsigned depth=0;node&&depth<32;++depth,node=rd64(node+0x230)){
        const char *name=law_photo_name(node);if(!name)continue;
        if(!strcmp(name,"EXT_SOCIOTECH_PANEL"))return 1;
        if(!strcmp(name,"EXT_PHOTO_MODE_PANEL"))return 2;
    }
    return 0;
}

static void law_photo_camera_cache(uint64_t object) {
    uint64_t reference=rd64(object+0xd8),view=reference?rd64(reference):0;
    if(!view||law_photo_family(view)!=1)return;
    uint32_t id=rd32(view+0x1e4);int index=ui_index_find(law_photo_camera_index,31u,object,id);
    if(index<0){
        // Controllers are normally initialized while loading still uses the desktop canvas.
        // Capture that baseline now, but apply mobile camera targets only after the scale increases.
        if(law_photo_camera_count==16)return;
        index=(int)law_photo_camera_count++;
        law_photo_camera_saved *saved=&law_photo_cameras[index];
        saved->object=object;saved->view=view;saved->id=id;
        memcpy(saved->initial,(void*)(uintptr_t)(object+0x260),16);
        memcpy(saved->selected,(void*)(uintptr_t)(object+0x270),16);
        memcpy(saved->viewport,(void*)(uintptr_t)(object+0x280),16);
        ui_index_add(law_photo_camera_index,31u,object,id,index);
    }
    law_photo_camera_saved *saved=&law_photo_cameras[index];
    if(applied_scale<=100){
        if(saved->active){
            memcpy((void*)(uintptr_t)(object+0x260),saved->initial,16);
            memcpy((void*)(uintptr_t)(object+0x270),saved->selected,16);
            memcpy((void*)(uintptr_t)(object+0x280),saved->viewport,16);saved->active=0;
        }
        return;
    }
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    law_photo_geometry g=law_photo_measure(width,height);if(!g.tree_height)return;
    float view_width=fminf(width-4,rdss(view+0x100)),view_height=fminf(g.tree_height,rdss(view+0x104));
    if(view_width<=0||view_height<=0)return;
    // The controller copies these properties once; resizing the widgets does not update its camera targets.
    wrss(object+0x260,view_width*.5f);wrss(object+0x264,view_height*.5f);
    // Details use the right column and the upper-left illustration. Keep the chosen
    // node in the remaining lower-left tree area; initial/root focus stays centred.
    wrss(object+0x270,(g.table_left-4)*.5f);wrss(object+0x274,fminf(view_height-58,g.law_selected_y-g.tree_top));
    wrss(object+0x280,view_width);wrss(object+0x284,view_height);saved->active=1;
}

void fp_mobile_law_camera(CPU *cpu,void (*original)(CPU*)) {
    law_photo_camera_cache(cpu->rcx);original(cpu);
}

void fp_mobile_law_camera_init(CPU *cpu,void (*original)(CPU*)) {
    uint64_t object=cpu->rcx;original(cpu);law_photo_camera_cache(object);
}

static law_photo_saved *law_photo_remember(uint64_t node) {
    uint32_t id=rd32(node+0x1e4);
    int index=ui_index_find(law_photo_saved_index,4095u,node,id);
    if(index>=0){law_photo_saved_nodes[index].active=1;return &law_photo_saved_nodes[index];}
    if(law_photo_saved_count==2048){static unsigned once;if(!once++)fprintf(stderr,"FP_UI_REGISTRY_FULL layout_nodes=2048\n");return NULL;}
    mobile_layout *base=remember_layout(node);if(!base)return NULL;
    index=(int)law_photo_saved_count++;
    law_photo_saved *saved=&law_photo_saved_nodes[index];
    *saved=(law_photo_saved){node,id,rd32(node+0x138),rd32(node+0x13c),1,0,0,*base};
    if(rd64(node)==GVA(0x141DED7D8)){
        saved->text_wrap[0]=rdss(node+0x37c);saved->text_wrap[1]=rdss(node+0x380);
        saved->text_limit=rd8(node+0x384);saved->text_saved=1;
    }
    ui_index_add(law_photo_saved_index,4095u,node,id,index);
    return saved;
}

static uint64_t law_photo_tree_root(uint64_t node) {
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
        const char *name=law_photo_name(child);if(name&&!strcmp(name,"ROOT"))return child;
    }
    return 0;
}

static uint64_t law_photo_child(uint64_t node,const char *wanted) {
    unsigned count=0;
    for(uint64_t child=rd64(node+0x220);child&&count++<128;child=rd64(child+0x240)){
        const char *name=law_photo_name(child);if(name&&!strcmp(name,wanted))return child;
    }
    return 0;
}

static uint64_t law_photo_touch_controller,law_photo_touch_view,law_photo_pending_controller,law_photo_pending_view;
static uint32_t law_photo_touch_id,law_photo_pending_id;
static float law_photo_empty_left;
static touch_rect law_photo_touch_buttons[128];
static unsigned law_photo_touch_button_count;

static void law_photo_touch_reset(void) {
    law_photo_touch_controller=law_photo_touch_view=0;law_photo_touch_id=0;law_photo_empty_left=1;law_photo_touch_button_count=0;
}

static uint64_t mobile_button_controller(uint64_t node,uint64_t action) {
    uint64_t callbacks=rd64(node+0x258);unsigned count=rd32(node+0x260);
    if(!callbacks||count>32)return 0;
    for(unsigned i=0;i<count;++i){
        uint64_t callback=callbacks+(uint64_t)i*32,reference=rd64(callback);
        if(rd64(callback+8)==GVA(action)&&reference)return rd64(reference);
    }
    return 0;
}

static void law_photo_touch_bind(uint64_t object,uint64_t view) {
    if(!object||!view||rd32(object+0xd0)!=2||!ui_visible_chain(view))return;
    law_photo_touch_controller=object;law_photo_touch_view=view;law_photo_touch_id=rd32(view+0x1e4);
}

static void law_photo_touch_control(uint64_t node) {
    const char *name=law_photo_name(node);
    if(!name)return;
    if(!strcmp(name,"ACTIVATE_SOCIOTECH_BUTTON")&&law_photo_family(node)==1){
        uint64_t object=mobile_button_controller(node,UINT64_C(0x141A5FBB0)),reference=object?rd64(object+0xd8):0;
        // Some native paths bypass the controller wrapper; its native button callback still owns live camera state.
        // Visit hidden live buttons too, so returning to UI100 restores the original camera before reopening the book.
        if(object)law_photo_camera_cache(object);
        if(applied_scale>100)law_photo_touch_bind(object,reference?rd64(reference):0);
    }
    if(applied_scale<=100||!ui_visible_chain(node))return;
    if(rd64(node)==GVA(0x141D79F78)&&law_photo_touch_button_count<128&&law_photo_family(node)==1){
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
        if(width>0&&height>0&&sx>0&&sy>0){
            x+=sx*(rdss(node+0xc0)+rdss(node+0xd0)*rdss(node+0xf0));
            y+=sy*(rdss(node+0xc4)+rdss(node+0xd4)*rdss(node+0xf4));
            law_photo_touch_buttons[law_photo_touch_button_count++]=(touch_rect){x/width,y/height,
                (x+sx*rdss(node+0xf0)*rdss(node+0x100))/width,(y+sy*rdss(node+0xf4)*rdss(node+0x104))/height,0};
        }
    }
    if(strcmp(name,"SOCIOTECHTREE"))return;
    uint32_t id=rd32(node+0x1e4);
    for(unsigned i=0;i<law_photo_camera_count;++i){
        law_photo_camera_saved *saved=&law_photo_cameras[i];
        if(saved->view==node&&saved->id==id&&rd32(saved->object+0xd0)==2){
            law_photo_touch_bind(saved->object,node);break;
        }
    }
}

static void law_photo_touch_finish(void) {
    if(!law_photo_touch_controller)return;
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    law_photo_geometry g=law_photo_measure(width,height);
    if(g.table_scale>0)law_photo_empty_left=(g.table_left+412*g.table_scale+8)/width;
}

static uint64_t law_photo_tap_target(float x,float y) {
    if(!law_photo_touch_controller||x<=law_photo_empty_left||y<.15f||y>.85f)return 0;
    for(unsigned i=0;i<law_photo_touch_button_count;++i){touch_rect r=law_photo_touch_buttons[i];
        if(x>=r.left&&x<=r.right&&y>=r.top&&y<=r.bottom)return 0;
    }
    law_photo_pending_controller=law_photo_touch_controller;law_photo_pending_view=law_photo_touch_view;
    law_photo_pending_id=law_photo_touch_id;return UINT64_MAX;
}

static void law_photo_actions(CPU *cpu) {
    uint64_t object=0;
    pthread_mutex_lock(&input_mutex);
    if(law_photo_pending_controller==law_photo_touch_controller&&law_photo_pending_view==law_photo_touch_view
       &&law_photo_pending_id==law_photo_touch_id)object=law_photo_touch_controller;
    law_photo_pending_controller=law_photo_pending_view=0;law_photo_pending_id=0;
    pthread_mutex_unlock(&input_mutex);
    // Use the original root-node action: it clears selection and restores all native animations and camera targets.
    if(object&&rd32(object+0xd0)==2)call_value(cpu,UINT64_C(0x141A60190),object,0);
}

static mobile_layout law_photo_original(uint64_t node) {
    int index=ui_index_find(mobile_layout_index,8191u,node,rd32(node+0x1e4));
    if(index>=0)return mobile_layouts[index];
    return (mobile_layout){node,rd32(node+0x1e4),rdss(node+0xc0),rdss(node+0xc4),rdss(node+0xf0),rdss(node+0xf4),rdss(node+0x210),rdss(node+0x214),rd32(node+0x138),rdss(node+0xd0),rdss(node+0xd4),rd32(node+0x1e0)&0x10};
}

static int law_photo_original_transform(uint64_t node,float *x,float *y,float *sx,float *sy) {
    uint64_t chain[40];unsigned count=0;
    for(;node&&count<40;node=rd64(node+0x230))chain[count++]=node;
    if(node)return 0;
    *x=*y=0;*sx=*sy=1;
    while(count){
        node=chain[--count];mobile_layout base=law_photo_original(node);
        if(!isfinite(base.sx)||!isfinite(base.sy)||base.sx<=0||base.sy<=0)return 0;
        *x+=*sx*(base.x+rdss(node+0xd0)*base.sx);
        *y+=*sy*(base.y+rdss(node+0xd4)*base.sy);
        *sx*=base.sx;*sy*=base.sy;
    }
    return isfinite(*x)&&isfinite(*y);
}

static void law_photo_size(CPU *cpu,uint64_t node,float width,float height) {
    if(fabsf(rdss(node+0x210)-width)>.01f||fabsf(rdss(node+0x214)-height)>.01f){
        float size[4]={width,height,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
    }
}

static void law_photo_unanchor(CPU *cpu,uint64_t node) {
    if(rd32(node+0x138))call_value(cpu,UINT64_C(0x140EC39C0),node,0);
    if(rd32(node+0x13c))call_value(cpu,UINT64_C(0x140EC30A0),node,0);
}

static void law_photo_place(CPU *cpu,uint64_t node,float left,float top,float scale_x,float scale_y) {
    if(!law_photo_remember(node))return;
    law_photo_unanchor(cpu,node);
    float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
    if(sx<=0||sy<=0)return;
    layout_scale(cpu,node,scale_x/sx,scale_y/sy);
    layout_position(cpu,node,(left-x)/sx-rdss(node+0xd0)*scale_x/sx,
                              (top-y)/sy-rdss(node+0xd4)*scale_y/sy);
}

static int law_photo_position(CPU *cpu,void (*original)(CPU*)) {
    if(law_photo_placing){original(cpu);return 1;}
    uint64_t node=cpu->rcx;const char *name=law_photo_name(node);
    if(applied_scale<=100||!name||strcmp(name,"TREE_PANEL")||law_photo_family(node)!=1)return 0;
    uint64_t root=law_photo_tree_root(node);if(!root)return 0;
    float position[4];memcpy(position,(void*)(uintptr_t)cpu->rdx,16);
    if(fabsf(position[0]+rdss(root+0xc0)-633.5f)>=2||fabsf(position[1]+rdss(root+0xc4)-303.5f)>=2)return 0;
    float width=rdss(GVA(0x142980E60)),height=rdss(GVA(0x142980E64));
    law_photo_geometry g=law_photo_measure(width,height);if(!g.tree_height)return 0;
    position[0]=width*.5f-rdss(root+0xc0);position[1]=g.tree_height*.5f-rdss(root+0xc4);
    uint64_t input=cpu->rdx;cpu->rdx=(uintptr_t)position;
    law_photo_placing=1;original(cpu);law_photo_placing=0;
    if(cpu->rdx==(uintptr_t)position)cpu->rdx=input;
    return 1;
}

static void law_photo_restore(CPU *cpu,uint64_t node) {
    uint32_t id=rd32(node+0x1e4);
    int index=ui_index_find(law_photo_saved_index,4095u,node,id);
    if(index>=0){
        law_photo_saved *saved=&law_photo_saved_nodes[index];
        if(!saved->active)return;
        mobile_layout *base=&saved->base;
        law_photo_placing=1;law_photo_unanchor(cpu,node);
        law_photo_size(cpu,node,base->w,base->h);
        if(saved->text_saved)mobile_message_wrap(cpu,node,saved->text_wrap[0],saved->text_wrap[1],saved->text_limit);
        layout_scale(cpu,node,base->sx,base->sy);layout_position(cpu,node,base->x,base->y);
        if(saved->dock)call_value(cpu,UINT64_C(0x140EC39C0),node,saved->dock);
        if(saved->anchors)call_value(cpu,UINT64_C(0x140EC30A0),node,saved->anchors);
        saved->viewport_width=saved->viewport_height=0;
        saved->active=0;
        law_photo_placing=0;return;
    }
}

static void law_photo_category(CPU *cpu,uint64_t card,float width) {
    uint64_t content=law_photo_child(card,"show_hide_animation_container");if(!content)content=card;
    uint64_t graphic=law_photo_child(content,"graphic"),title=law_photo_child(content,"TITLE_TEXT");
    uint64_t description=law_photo_child(content,"SHORT_DESCRIPTION_TEXT");
    if(!graphic)return;
    float x,y,sx,sy;parent_transform(card,&x,&y,&sx,&sy);
    float card_left=x+sx*(rdss(card+0xc0)+rdss(card+0xd0)*rdss(card+0xf0));
    float card_top=y+sy*(rdss(card+0xc4)+rdss(card+0xd4)*rdss(card+0xf4));
    float card_width=rdss(card+0x100)*sx*rdss(card+0xf0),center=card_left+card_width*.5f;
    unsigned cards=0,budget=0;uint64_t layout=rd64(card+0x230);
    for(uint64_t item=layout?rd64(layout+0x220):0;item&&budget++<128;item=rd64(item+0x240)){
        const char *name=law_photo_name(item);
        if(name&&!strcmp(name,"CATEGORY_BUTTON")&&ui_visible_chain(item))++cards;
    }
    float factor=fminf(1,(cards==1?width-24:card_width-16)/301.f);if(factor<=0)return;
    float title_bottom=card_top+34,description_height=0;
    uint64_t labels[]={title,description};
    for(unsigned i=0;i<2;++i){uint64_t label=labels[i];
        if(!label||rd64(label)!=GVA(0x141DED7D8)||!law_photo_remember(label))continue;
        float scale=i?1.2f:.9f;
        float available=i?fminf(card_width-24,233*factor):card_width-24;
        if(available<=0)continue;
        mobile_message_wrap(cpu,label,available/scale,0,1);
        float height=fmaxf(1,mobile_message_height(label));
        law_photo_size(cpu,label,available/scale,height);
        if(i)description_height=height*scale;else title_bottom=card_top+10+height*scale;
    }
    float plate_top=title_bottom+12,plate_height=fmaxf(44*factor,description_height+12);
    float graphic_sy=plate_height/44;
    law_photo_place(cpu,graphic,center-150.5f*factor,plate_top-41*graphic_sy,factor,graphic_sy);
    for(unsigned i=0;i<2;++i){uint64_t label=labels[i];
        if(!label||rd64(label)!=GVA(0x141DED7D8))continue;
        float scale=i?1.2f:.9f,ink_width=rdss(label+0x4d0);
        if(!isfinite(ink_width)||ink_width<=0)ink_width=rdss(label+0x210);
        ink_width=fminf(ink_width,rdss(label+0x210));
        float top=i?plate_top+(plate_height-description_height)*.5f:card_top+10;
        // Native glyph layout centres individual lines within its measured block, not the configured wrap box.
        law_photo_place(cpu,label,center-ink_width*scale*.5f,top,scale,scale);
    }
}

static int law_photo_layout(CPU *cpu,uint64_t node,float width,float height) {
    unsigned family=law_photo_family(node);if(!family)return 0;
    const char *name=law_photo_name(node);if(!name)return 1;
    uint64_t parent=rd64(node+0x230);const char *parent_name=parent?law_photo_name(parent):NULL;
    uint64_t grandparent=parent?rd64(parent+0x230):0;
    const char *grandparent_name=grandparent?law_photo_name(grandparent):NULL;
    unsigned full=law_photo_full_canvas(family,name,parent_name,grandparent_name);
    if(applied_scale<=100&&family!=2){law_photo_restore(cpu,node);return 1;}
    law_photo_geometry g=law_photo_measure(width,height);if(!g.tree_height)return 1;
    law_photo_placing=1;
    if(full){
        if(law_photo_remember(node)){
            law_photo_unanchor(cpu,node);
            // Restoring photo's desktop docking can retain a physical-pixel viewport at 100%.
            if(family==2&&(fabsf(rdss(node+0x100)-width)>.01f||fabsf(rdss(node+0x104)-height)>.01f)){
                float size[4]={width,height,0,0};call(cpu,UINT64_C(0x140EC4C70),node,size,0,0,0);
            } else law_photo_size(cpu,node,width,height);
            law_photo_place(cpu,node,0,0,1,1);
        }
    } else if(family==1){
        if(!strcmp(name,"CONTENT")&&parent_name&&(!strcmp(parent_name,"TREE_VIEW")||!strcmp(parent_name,"CATEGORIES_VIEW"))){
            if(law_photo_remember(node)){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width,height);law_photo_place(cpu,node,0,0,1,1);}
        } else if((!strcmp(name,"TreeClip")||!strcmp(name,"SOCIOTECHTREE"))&&law_photo_ancestor(node,"TREE_VIEW")){
            if(law_photo_remember(node)){
                law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width-4,g.tree_height);
                law_photo_place(cpu,node,2,g.tree_top,1,1);
            }
        } else if(!strcmp(name,"TREE_PANEL")){
            uint64_t root=law_photo_tree_root(node);
            law_photo_saved *saved=law_photo_remember(node);
            if(root&&saved){
                float x=rdss(node+0xc0),y=rdss(node+0xc4);
                float center_x=x+rdss(root+0xc0),center_y=y+rdss(root+0xc4);
                if(fabsf(center_x-633.5f)<2&&fabsf(center_y-303.5f)<2){
                    x=width*.5f-rdss(root+0xc0);y=g.tree_height*.5f-rdss(root+0xc4);
                } else if(saved->viewport_width>0&&(fabsf(saved->viewport_width-width)>.01f||fabsf(saved->viewport_height-g.tree_height)>.01f)){
                    x+=(width-saved->viewport_width)*.5f;y+=(g.tree_height-saved->viewport_height)*.5f;
                }
                layout_position(cpu,node,x,y);saved->viewport_width=width;saved->viewport_height=g.tree_height;
            }
        } else if(!strcmp(name,"INFO_PANEL")&&parent_name&&!strcmp(parent_name,"CONTENT")&&grandparent_name&&!strcmp(grandparent_name,"TREE_VIEW")){
            if(law_photo_remember(node)){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width,height);law_photo_place(cpu,node,0,0,1,1);}
        } else if(law_photo_ancestor(node,"INFO_PANEL")){
            uint64_t picture=!strcmp(name,"SFX_MASK")&&parent_name&&!strcmp(parent_name,"INFO_PANEL")?law_photo_child(node,"BIG_PICTURE"):0;
            uint64_t table=!strcmp(name,"SFX_MASK")&&parent_name&&!strcmp(parent_name,"INFO_PANEL")?law_photo_child(node,"Table"):0;
            if(picture){
                mobile_layout base=law_photo_original(picture);
                if(base.w>0){float factor=g.art_size/base.w;law_photo_place(cpu,node,12-(base.x+rdss(picture+0xd0))*factor,60-(base.y+rdss(picture+0xd4))*factor,factor,factor);}
            } else if(table){
                mobile_layout base=law_photo_original(table);
                law_photo_place(cpu,node,g.table_left-(base.x+rdss(table+0xd0))*g.table_scale,g.table_top-(base.y+rdss(table+0xd4))*g.table_scale,g.table_scale,g.table_scale);
            } else if(!strcmp(name,"SFX_MAIN")&&parent_name&&!strcmp(parent_name,"SFX_MASK")&&grandparent_name&&!strcmp(grandparent_name,"INFO_PANEL")){
                uint64_t image=law_photo_child(parent,"BIG_PICTURE");
                if(image){mobile_layout base=law_photo_original(image);if(base.w>0){float factor=g.art_size/base.w;law_photo_place(cpu,node,12-(base.x+rdss(image+0xd0))*factor,60-(base.y+rdss(image+0xd4))*factor,factor,factor);}}
            } else if(!strcmp(name,"BIG_PICTURE"))law_photo_place(cpu,node,12,60,g.art_size/800,g.art_size/800);
            else if(!strcmp(name,"Table"))law_photo_place(cpu,node,g.table_left,g.table_top,g.table_scale,g.table_scale);
            else if(!strcmp(name,"ACTIVATE_SOCIOTECH_BUTTON")){
                /* The button renders inside the table's SFX_MASK camera, which clips everything left of the
                 * table: the left column cut its arrow plate in half.  Use the seal's slot (where the PC page
                 * shows the button before signing), centred in the table. */
                uint64_t seal=parent?law_photo_child(parent,"SEAL"):0;
                float factor=fminf(1.15f,(412*g.table_scale-24)/220);
                if(seal&&factor>0){
                    float x,y,sx,sy;parent_transform(seal,&x,&y,&sx,&sy);
                    float cy=y+sy*(rdss(seal+0xc4)+(rdss(seal+0xd4)+rdss(seal+0x104)*.5f)*rdss(seal+0xf4));
                    law_photo_place(cpu,node,g.table_left+(412*g.table_scale-220*factor)*.5f,cy-20*factor,factor,factor);
                } else {
                    factor=fminf(1.2f,(g.table_left-16)/220);
                    law_photo_place(cpu,node,(g.table_left-220*factor)*.5f,g.law_action_top,factor,factor);
                }
            } else if(!strcmp(name,"COOLDOWN_ACTIVE_PANEL")||!strcmp(name,"GENERATOR_NOT_BUILT_PANEL")||!strcmp(name,"LAW_IMPOSED_PANEL")){
                law_photo_place(cpu,node,20,height-60,1,1);
            }
        } else if(!strcmp(name,"CATEGORY_BUTTON")&&law_photo_ancestor(node,"CATEGORIES_VIEW")){
            law_photo_category(cpu,node,width);
        } else if(!strcmp(name,"CATEGORIES_LAYOUT")&&parent_name&&!strcmp(parent_name,"CONTENT")&&grandparent_name&&!strcmp(grandparent_name,"CATEGORIES_VIEW")){
            mobile_layout *base=remember_layout(node);
            if(base){
                float factor=fminf((width-24)/1163.95f,(height-76)/600.f),left=INFINITY,right=-INFINITY;unsigned count=0;
                // A new game shows only Adaptation: centre the visible cards under the centred caption.
                for(uint64_t card=rd64(node+0x220);card&&count++<64;card=rd64(card+0x240)){
                    const char *card_name=law_photo_name(card);
                    if(!card_name||strcmp(card_name,"CATEGORY_BUTTON")||(rd32(card+0x1e0)&2))continue;
                    float a=rdss(card+0xf0),l=rdss(card+0xc0)+rdss(card+0xd0)*a;
                    left=fminf(left,l);right=fmaxf(right,l+rdss(card+0x100)*a);
                }
                float x=12;
                if(isfinite(left)&&right>left&&right-left<2000)x=(width-(right-left)*factor)*.5f-left*factor;
                law_photo_place(cpu,node,x,16,factor,factor);
            }
        } else if(!strcmp(name,"CUSTOM_CLOSE_BUTTON")&&parent_name&&!strcmp(parent_name,"ANIM"))law_photo_place(cpu,node,width-148,8,.85f,.85f);
        else if(!strcmp(name,"top")&&parent_name&&!strcmp(parent_name,"TREE_VIEW"))law_photo_place(cpu,node,0,0,1,1);
        else if((!strcmp(name,"logo")||!strcmp(name,"BACK_BUTTON"))&&parent_name&&!strcmp(parent_name,"top")&&grandparent_name&&!strcmp(grandparent_name,"TREE_VIEW")){
            uint64_t content=law_photo_child(grandparent,"CONTENT"),info=content?law_photo_child(content,"INFO_PANEL"):0;
            if(info&&!(rd32(info+0x1e0)&2)){
                if(!strcmp(name,"logo"))law_photo_place(cpu,node,12-140*.6f,4+501*.6f,.6f,.6f);
                else law_photo_place(cpu,node,12,height-42,.8f,.8f);
            } else law_photo_restore(cpu,node);
        }
        else if(!strcmp(name,"HEADER")&&parent_name&&!strcmp(parent_name,"CATEGORIES_VIEW")){
            law_photo_place(cpu,node,0,height-60,width/1280,width/1280);  /* uniform: 0.5x0.6 stretched the caption */
        } else if(parent_name&&((!strcmp(name,"BACKGROUND")&&!strcmp(parent_name,"CATEGORIES_VIEW"))
                  ||(!strcmp(name,"DARK")&&!strcmp(parent_name,"TREE_VIEW"))
                  ||((!strcmp(name,"bg blur")||!strcmp(name,"BackgroundBrown")||!strcmp(name,"bg"))&&law_photo_ancestor(node,"background")))){
            mobile_layout *base=remember_layout(node);
            if(base&&base->w>0&&base->h>0){
                float factor=fmaxf(width/1280,height/720);
                law_photo_place(cpu,node,(width-base->w*factor)*.5f,(height-base->h*factor)*.5f,factor,factor);
            }
        }
    } else {
        float settings_factor=fminf(1,fminf((height-76)/330.f,(width-24)/460.f));
        float help_left=fmaxf(width*.54f,12+380*settings_factor);
        float help_width=fmaxf(80,width-help_left-12),help_top=44;
        float caption_top=height-104;
        if(!strcmp(name,"SFX_MASK")&&rd64(node)==GVA(0x141D796F8)&&law_photo_ancestor(node,"SETTINGS_SHORT")){
            /* Skip this 3D decoration's own draw; the normal 2D child icons must still render. */
            if(!(rd32(node+0x1e0)&1))call_value(cpu,UINT64_C(0x140EC0F60),node,1);
        } else if(!strcmp(name,"settings_left_top_container")){
            law_photo_place(cpu,node,12,12+282*settings_factor,settings_factor,settings_factor);
        } else if(!strcmp(name,"mask")&&parent_name&&!strcmp(parent_name,"settings_left_top_container")&&rd64(node)==GVA(0x141DA2AA8)){
            law_photo_saved *saved=law_photo_remember(node);
            /* This subtree contains only decorative 3D splashes; their camera ignores the 2D scale. */
            if(saved){
                mobile_layout *base=&saved->base;
                law_photo_size(cpu,node,base->w,base->h);
                layout_scale(cpu,node,base->sx,base->sy);layout_position(cpu,node,base->x,base->y);
                if(!(rd32(node+0x1e0)&2))call_value(cpu,UINT64_C(0x140EC0F60),node,2);
            }
        } else if((!strcmp(name,"RESET_SETTINGS_BUTTON")||!strcmp(name,"EXIT_BUTTON")||!strcmp(name,"TOGGLE_BUTTON")||!strcmp(name,"TAKE_SCREENSHOT_BUTTON"))
                   &&law_photo_ancestor(node,"SETTINGS_DETAILED")){
            unsigned index=!strcmp(name,"RESET_SETTINGS_BUTTON")?0:!strcmp(name,"EXIT_BUTTON")?1:!strcmp(name,"TOGGLE_BUTTON")?2:3;
            float w=rdss(node+0x100),h=rdss(node+0x104),factor=g.action_scale;
            law_photo_place(cpu,node,(index+.5f)*width/4-w*factor*.5f,height-8-h*factor,factor,factor);
        } else if((!strcmp(name,"TOGGLE_BUTTON")||!strcmp(name,"TAKE_SCREENSHOT_BUTTON"))&&law_photo_ancestor(node,"SETTINGS_SHORT")){
            unsigned index=!strcmp(name,"TAKE_SCREENSHOT_BUTTON");
            law_photo_place(cpu,node,width*.5f+(index?8:-50),height-50,1.4f,1.4f);
        } else if(!strcmp(name,"HELP_BUTTON"))law_photo_place(cpu,node,width-12,12,-.6f,.6f);
        else if(!strcmp(name,"HELP_PANEL")&&parent_name&&!strcmp(parent_name,"right_bottom_panel")){
            if(law_photo_remember(node)){
                law_photo_size(cpu,node,help_width,caption_top-help_top-12);
                law_photo_place(cpu,node,help_left,help_top,1,1);
            }
        } else if(!strcmp(name,"mask")&&parent_name&&!strcmp(parent_name,"HELP_PANEL")){
            if(law_photo_remember(node)){
                law_photo_size(cpu,node,help_width,caption_top-help_top-12);
                law_photo_place(cpu,node,help_left,help_top,1,1);
            }
        } else if(!strcmp(name,"PHOTOMODE_TOOLTIP_TEXT")&&parent_name&&!strcmp(parent_name,"mask")&&grandparent_name&&!strcmp(grandparent_name,"HELP_PANEL")){
            if(law_photo_remember(node)){
                // Keep the game's text wrapping and alignment; replace the desktop-wide box.
                law_photo_size(cpu,node,help_width/.9f,(caption_top-help_top-12)/.9f);
                law_photo_place(cpu,node,help_left,help_top,.9f,.9f);
            }
        } else if(!strcmp(name,"SCREENSHOT_NOTIFICATION_PANEL")&&parent_name&&!strcmp(parent_name,"ANIM")){
            if(law_photo_remember(node)){
                law_photo_size(cpu,node,help_width,64);
                law_photo_place(cpu,node,help_left,caption_top,1,1);
            }
        } else if(law_photo_ancestor(node,"SCREENSHOT_NOTIFICATION_PANEL")){
            if((!strcmp(name,"NOTIFICATION")&&parent_name&&!strcmp(parent_name,"SCREENSHOT_NOTIFICATION_PANEL"))
                ||(!strcmp(name,"text_mask")&&parent_name&&!strcmp(parent_name,"NOTIFICATION"))){
                if(law_photo_remember(node)){
                    law_photo_size(cpu,node,help_width,58);
                    law_photo_place(cpu,node,help_left,caption_top,1,1);
                }
            } else if(!strcmp(name,"NOTIFICATION_TEXT")&&parent_name&&!strcmp(parent_name,"text_mask")){
                if(law_photo_remember(node)){
                    law_photo_size(cpu,node,help_width/.9f,58/.9f);
                    law_photo_place(cpu,node,help_left,caption_top,.9f,.9f);
                }
            } else if(!strcmp(name,"bg_centering_element")&&parent_name&&!strcmp(parent_name,"text_mask")){
                float factor=fminf(1,help_width/360);
                law_photo_place(cpu,node,help_left+42*factor,caption_top,factor,factor);
            }
        }
        else if(!strcmp(name,"darken_lefttop")||!strcmp(name,"darken_rightbottom")){
            if(law_photo_remember(node)){law_photo_unanchor(cpu,node);law_photo_size(cpu,node,width,height);law_photo_place(cpu,node,0,0,1,1);}
        } else if(!strcmp(name,"ornaments_frame_father_element"))law_photo_place(cpu,node,0,0,width/1296,height/730);
    }
    law_photo_placing=0;return 1;
}

// Transform the original PC paint image with exactly the same affine map as its
// mobile 2D anchor. Fullscreen template cameras otherwise ignore all parent transforms.
static int law_photo_template(CPU *cpu,void (*original)(CPU*)) {
    uint64_t node=cpu->rcx;const char *name=law_photo_name(node);
    if(applied_scale<=100||!name||law_photo_family(node)!=1)return 0;
    if(law_photo_ancestor(node,"background")||!strcmp(name,"SFX_MASK_SPLINTERS")){
        // These fullscreen paint cameras belong to the backdrop, so HUD zoom must not magnify their composition.
        original(cpu);return 1;
    }
    uint64_t mask=node,parent=rd64(node+0x230);
    if(!strcmp(name,"SFX_MAIN")){
        const char *parent_name=parent?law_photo_name(parent):NULL;
        if(!parent_name||strcmp(parent_name,"SFX_MASK"))return 0;
        mask=parent;
    } else if(strcmp(name,"SFX_MASK"))return 0;
    uint64_t info=rd64(mask+0x230);const char *info_name=info?law_photo_name(info):NULL;
    if(!info_name||strcmp(info_name,"INFO_PANEL"))return 0;
    // The model-transform path already consumes the corrected mask/SFX_MAIN matrix.
    if(rd8(node+0x380)){original(cpu);return 1;}
    uint64_t anchor=law_photo_child(mask,"Table");unsigned art=!anchor;
    if(art)anchor=law_photo_child(mask,"BIG_PICTURE");
    if(!anchor||!cpu->rdx)return 0;
    float source_x,source_y,source_sx,source_sy;
    if(!law_photo_original_transform(anchor,&source_x,&source_y,&source_sx,&source_sy))return 0;
    float x,y,sx,sy,ratio,target_x,target_y;
    if(art){
        law_photo_geometry g=law_photo_measure(rdss(GVA(0x142980E60)),rdss(GVA(0x142980E64)));
        mobile_layout base=law_photo_original(anchor);if(base.w<=0)return 0;
        ratio=g.art_size/(base.w*source_sx);target_x=12;target_y=60;
    } else {
        parent_transform(anchor,&x,&y,&sx,&sy);
        target_x=x+sx*(rdss(anchor+0xc0)+rdss(anchor+0xd0)*rdss(anchor+0xf0));
        target_y=y+sy*(rdss(anchor+0xc4)+rdss(anchor+0xd4)*rdss(anchor+0xf4));
        ratio=sx*rdss(anchor+0xf0)/source_sx;
    }
    if(!isfinite(ratio)||ratio<=0)return 0;
    float matrix[16];uint64_t input=cpu->rdx;memcpy(matrix,(void*)(uintptr_t)input,sizeof(matrix));
    matrix[12]=target_x-ratio*source_x-rdss(node+0xd0);
    matrix[13]=target_y-ratio*source_y-rdss(node+0xd4);
    float width=rdss(node+0x100),height=rdss(node+0x104);unsigned fullscreen=rd8(node+0x383);
    float orthographic=rdss(node+0x37c);unsigned perspective=rd8(node+0x385);
    wrss(node+0x100,1280*ratio);wrss(node+0x104,720*ratio);wr8(node+0x383,0);
    if(!perspective)wrss(node+0x37c,orthographic*ratio*applied_scale/100.f);
    cpu->rdx=(uintptr_t)matrix;original(cpu);
    if(cpu->rdx==(uintptr_t)matrix)cpu->rdx=input;
    if(!perspective)wrss(node+0x37c,orthographic);
    wrss(node+0x100,width);wrss(node+0x104,height);wr8(node+0x383,fullscreen);
    return 1;
}
#endif
