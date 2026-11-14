static int switch_button_index(const char *name){
    if(!name||(name[0]!='E'&&name[0]!='S'&&name[0]!='T'&&name[0]!='C'))return -1;
    static const char *names[]={"ECONOMY_BUTTON","SEARCH_CITIZENS_BUTTON","SOCIOTECH_BUTTON","TECHNOLOGY_BUTTON","CONSTRUCTION_BUTTON"};
    for(unsigned i=0;i<5;++i)if(!strcmp(name,names[i]))return (int)i;
    return -1;
}

static int switch_button_layout(CPU *cpu,uint64_t node){
    if(!switch_mode||!atomic_load(&saved_controller)||rd64(node)!=GVA(0x141D79F78)||switch_button_index(law_photo_name(node))<0)return 0;
    unsigned open=atomic_load(&switch_menu)&&!phone_hud_covered&&!pause_menu_visible;
    unsigned hidden=(rd32(node+0x1e0)&2)!=0;
    if(hidden==open)call_value(cpu,open?UINT64_C(0x140EBE470):UINT64_C(0x140EC0F60),node,2);
    if(!open)return 1;
    if(rdss(node+0x11c)!=0){
        _Alignas(16) float color[4];memcpy(color,(void*)(uintptr_t)(node+0x110),sizeof(color));color[3]=0;
        call(cpu,UINT64_C(0x140EC4B90),node,color,0,0,0);
    }
    unsigned children=0;
    for(uint64_t child=rd64(node+0x220);child&&children++<128;child=rd64(child+0x240))
        if(!(rd32(child+0x1e0)&2))call_value(cpu,UINT64_C(0x140EC0F60),child,2);
    return 0;
}

static unsigned switch_modal_scan(uint64_t node,unsigned depth,unsigned *budget){
    if(!node||depth>16||!*budget||(rd32(node+0x30)|rd32(node+0x1e0)|rd8(node+0x34))&2)return 0;
    --*budget;const char *name=law_photo_name(node);
    if(name&&!strncmp(name,"EXT_",4)){
        return !strcmp(name,"EXT_SOCIOTECH_PANEL")||!strcmp(name,"EXT_TECHNOLOGY_PANEL")||!strcmp(name,"EXT_ECONOMY_PANEL")
            ||!strcmp(name,"EXT_SEARCH_CITIZENS_PANEL")||!strcmp(name,"EXT_CONSTRUCTION_PANEL")||!strcmp(name,"EXT_SELECTION_PANEL");
    }
    unsigned children=0;
    for(uint64_t child=rd64(node+0x220);child&&children++<512;child=rd64(child+0x240))
        if(switch_modal_scan(child,depth+1,budget))return 1;
    return 0;
}
