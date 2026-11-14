// Paint-mask cameras do not inherit a dialog INK_PANEL's 2D scale.
// Include before fp_mobile_ui_template, after remember_layout/ui_index_find.
static uint64_t dialog_mask_ink(uint64_t node) {
    for(unsigned depth=0;node&&depth<32;++depth,node=rd64(node+0x230)){
        const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
        if(!name||strcmp(name,"INK_PANEL"))continue;
        uint64_t parent=rd64(node+0x230);
        const char *parent_name=parent?(const char*)(uintptr_t)rd64(parent+0x1d8):NULL;
        if(!parent_name||(strcmp(parent_name,"EXT_MAIN_MENU_PANEL")
           &&strcmp(parent_name,"EXT_INGAME_MENU_MAIN_PANEL")&&strcmp(parent_name,"EXT_SETTINGS_PANEL")
           &&strcmp(parent_name,"EXT_INGAME_MENU_SETTINGS_PANEL")&&strcmp(parent_name,"EXT_LOADGAME_PANEL")
           &&strcmp(parent_name,"EXT_INGAME_MENU_LOADGAME_PANEL")&&strcmp(parent_name,"EXT_INGAME_MENU_SAVEGAME_PANEL")
           &&strcmp(parent_name,"EXT_SAVE_GAME_PANEL")&&strcmp(parent_name,"EXT_CONFIRMATION_PANEL")))continue;
        return node;
    }
    return 0;
}

static float dialog_mask_projection_factor(uint64_t node) {
    uint64_t ink=dialog_mask_ink(node);
    if(!ink){
        if(applied_scale<=100)return 1;
        /* The camera must follow the 2D zoom the mask's text gets: the interface scale times the
         * node's own accumulated scale.  PC dialogs fitted at 0.5 (story screens) net 1 — doubling
         * them drew a second, displaced ink copy of every dilemma title. */
        float x,y,sx,sy;parent_transform(node,&x,&y,&sx,&sy);
        float scale=sy*rdss(node+0xf4),factor=applied_scale/100.f;
        if(isfinite(scale)&&scale>0&&scale<.95f)factor*=scale;
        return factor;
    }
    // Only an already captured baseline is valid; never capture the enlarged scale here.
    int index=ui_index_find(mobile_layout_index,8191u,ink,rd32(ink+0x1e4));
    if(index<0)return 1;
    const mobile_layout *base=&mobile_layouts[index];
    float sx=rdss(ink+0xf0),sy=rdss(ink+0xf4);
    if(!isfinite(sx)||!isfinite(sy)||base->sx<=0||base->sy<=0)return 1;
    float ratio_x=sx/base->sx,ratio_y=sy/base->sy;
    // These dialog transforms are uniform. Do not reinterpret unrelated/nonuniform art.
    if(!isfinite(ratio_x)||!isfinite(ratio_y)||ratio_x<=0||fabsf(ratio_x-ratio_y)>.001f)return 1;
    return ratio_x;
}

static int dialog_mask_fullscreen(CPU *cpu,void (*original)(CPU*),uint64_t ink) {
    uint64_t node=cpu->rcx;
    int index=ui_index_find(mobile_layout_index,8191u,ink,rd32(ink+0x1e4));
    if(index<0)return 0;
    const mobile_layout *base=&mobile_layouts[index];
    float x,y,sx,sy;parent_transform(ink,&x,&y,&sx,&sy);
    if(base->sx<=0||base->sy<=0)return 0;
    float ratio=sx*rdss(ink+0xf0)/base->sx,ratio_y=sy*rdss(ink+0xf4)/base->sy;
    if(!isfinite(ratio)||!isfinite(ratio_y)||ratio<=0||fabsf(ratio-ratio_y)>.001f)return 0;
    // These external dialogs author their paint image on the full PC canvas.
    // Preserve the original pivot: SetSize can change the live pivot of a centred INK.
    float source_x=base->x+base->px*base->sx,source_y=base->y+base->py*base->sy;
    float target_x=x+sx*(rdss(ink+0xc0)+rdss(ink+0xd0)*rdss(ink+0xf0));
    float target_y=y+sy*(rdss(ink+0xc4)+rdss(ink+0xd4)*rdss(ink+0xf4));
    float tx=target_x-ratio*source_x,ty=target_y-ratio*source_y;
    if(!isfinite(tx)||!isfinite(ty))return 0;
    if(fabsf(ratio-1)<.0001f&&fabsf(tx)<.001f&&fabsf(ty)<.001f){original(cpu);return 1;}
    if(!cpu->rdx)return 0;
    unsigned perspective=rd8(node+0x385);float value=rdss(node+(perspective?0x378:0x37c));
    if(!isfinite(value)||value<=0||(perspective&&value>=179))return 0;
    float matrix[16];uint64_t input=cpu->rdx;memcpy(matrix,(void*)(uintptr_t)input,sizeof(matrix));
    matrix[12]=tx-rdss(node+0xd0);matrix[13]=ty-rdss(node+0xd4);
    float width=rdss(node+0x100),height=rdss(node+0x104);
    wrss(node+0x100,1280*ratio);wrss(node+0x104,720*ratio);wr8(node+0x383,0);
    // Native rectangular perspective already obtains zoom from its viewport height.
    if(!perspective)wrss(node+0x37c,value*ratio*applied_scale/100.f);
    cpu->rdx=(uintptr_t)matrix;original(cpu);
    if(cpu->rdx==(uintptr_t)matrix)cpu->rdx=input;
    if(!perspective)wrss(node+0x37c,value);
    wrss(node+0x100,width);wrss(node+0x104,height);wr8(node+0x383,1);
    return 1;
}

static int dialog_mask_render(CPU *cpu,void (*original)(CPU*)) {
    uint64_t node=cpu->rcx;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);
    uint64_t ink=dialog_mask_ink(node);
    if(!name||!strstr(name,"MASK")||!ink)return 0;
    if(rd8(node+0x380)){original(cpu);return 1;}
    if(rd8(node+0x383))return dialog_mask_fullscreen(cpu,original,ink);
    float factor=dialog_mask_projection_factor(node);
    if(fabsf(factor-1)<.0001f){original(cpu);return 1;}
    unsigned perspective=rd8(node+0x385),offset=perspective?0x378:0x37c;
    float value=rdss(node+offset),width=rdss(node+0x100),height=rdss(node+0x104);
    if(!isfinite(value)||value<=0||(perspective&&value>=179))return 0;
    unsigned viewport=!rd8(node+0x383)&&isfinite(width)&&isfinite(height)&&width>0&&height>0;
    // 140F9D240 uses unscaled +100/+104 for a non-fullscreen mask's rectangle and
    // its off-centre projection. Supply the actual enlarged rectangle only for this draw.
    // The perspective branch already multiplies P00/P11 by viewportHeight/targetHeight;
    // changing the rectangle therefore supplies its zoom, without a second FOV adjustment.
    if(viewport){wrss(node+0x100,width*factor);wrss(node+0x104,height*factor);}
    unsigned projection=!perspective||!viewport;
    if(projection){
        float adjusted=perspective?atanf(tanf(value*(3.141592654f/360))/factor)*(360/3.141592654f):value*factor;
        wrss(node+offset,adjusted);
    }
    original(cpu);
    if(projection)wrss(node+offset,value);
    if(viewport){wrss(node+0x100,width);wrss(node+0x104,height);}
    return 1;
}
