extern int frostpunk_android_touch_text(const uint16_t*,uint32_t,const uint16_t*,uint32_t) __attribute__((weak));

/* Loading caption, gesture help and the game's own names of the buttons TouchControls draws: help,
 * U+001E, then the labels separated by U+001F (construction, economy, search, laws, technology,
 * build street, dismantle street, OK, cancel, dismantle building, and the street modes). */
static const char *const overlay_label_keys[]={
    "UI/Tooltips/MainScreenButtons/ConstructionButton","UI/OtherPanels/MainButtons/EconomyButtonTooltip",
    "UI/Buttons/ClickToSearch","UI/Tooltips/MainScreenButtons/BookOfLaws","UI/Tooltips/MainScreenButtons/TechnologyTree",
    "UI/OtherPanels/ConstructionPanel/BuildStreet","UI/OtherPanels/ConstructionPanel/DemolishStreet",
    "UI/Buttons/Ok","UI/Menu/Buttons/Cancel","UI/OtherPanels/ConstructionPanel/DemolishBuilding",
    "Streets/PaintingNormalStreets","Streets/DemolishingStreets","Streets/PaintingPipeStreets"};
enum { STREET_MODE_FIRST_LABEL=10 };
static uint16_t street_mode_feedback[3][128];

static unsigned street_mode_message(uint64_t text,uint64_t *detail){
    static const unsigned painters[]={2,5,3};
    if(!text)return 0;
    for(unsigned i=0;i<sizeof(street_mode_feedback)/sizeof(*street_mode_feedback);++i){
        const uint16_t *mode=street_mode_feedback[i];
        if(!*mode)continue;
        unsigned j=0;
        while(mode[j]&&mode[j]==rd16(text+2*j))++j;
        if(mode[j])continue;
        uint16_t next=rd16(text+2*j);
        if(next&&next!='\n'&&next!='\r'&&next!=' ')continue;
        while(next=='\n'||next=='\r'||next==' ')next=rd16(text+2*++j);
        *detail=text+2*j;
        return painters[i];
    }
    return 0;
}

static int overlay_resolve(CPU *cpu,uint64_t localization,const char *key,uint64_t *text,uint32_t *length){
    CPU call=*cpu;uint64_t stack=(cpu->rsp-512)&~UINT64_C(15);
    call.rsp=stack;call.rcx=localization;call.rdx=(uintptr_t)key;
    call.r8=GVA(0x142980530);call.r9=0;
    wr64(stack+0x20,1);wr64(stack+0x28,0);
    push64(&call,UINT64_C(0xe5ee0010));dispatch(&call,UINT64_C(0x140E84CD0));
    if(call.rsp!=stack||!call.rax)return 0;
    *text=rd64(call.rax);*length=0;if(!*text)return 0;
    while(*length<8192&&rd16(*text+2u**length))++*length;
    return *length<8192;
}

static void mobile_overlay_text(CPU *cpu) {
    static uint64_t previous[4];
    if(!frostpunk_android_touch_text)return;
    uint64_t localization=rd64(GVA(0x142983988)),config=rd64(GVA(0x142985AB0));
    if(!localization||!rd64(localization+0x60)||!rd32(localization+0x80))return;
    // The original resolver selects the configured language, falling back to this GUID.
    uint64_t language[4]={localization,rd64(localization+0x90),rd64(localization+0x98),config?rd64(config+0x40):0};
    if(!memcmp(language,previous,sizeof(language)))return;
    const char *keys[]={"UI/Menu/Buttons/Loading","UI/Android/GestureHelpText"};
    uint64_t texts[2];uint32_t lengths[2];
    for(unsigned i=0;i<2;++i)if(!overlay_resolve(cpu,localization,keys[i],&texts[i],&lengths[i])||!lengths[i])return;
    static uint16_t combined[8192];uint32_t used=lengths[1];
    memcpy(combined,(const void*)(uintptr_t)texts[1],used*2u);
    combined[used++]=0x1e;
    for(unsigned i=0;i<sizeof(overlay_label_keys)/sizeof(*overlay_label_keys);++i){
        uint64_t text=0;uint32_t length=0;
        if(i)combined[used++]=0x1f;
        if(!overlay_resolve(cpu,localization,overlay_label_keys[i],&text,&length))length=0;
        if(i-STREET_MODE_FIRST_LABEL<sizeof(street_mode_feedback)/sizeof(*street_mode_feedback)){
            uint16_t *mode=street_mode_feedback[i-STREET_MODE_FIRST_LABEL];
            unsigned units=length<sizeof(street_mode_feedback[0])/sizeof(*mode)?length:0;
            if(units)memcpy(mode,(const void*)(uintptr_t)text,units*2u);
            mode[units]=0;
        }
        if(length&&used+length<8000){
            memcpy(combined+used,(const void*)(uintptr_t)text,length*2u);used+=length;
        }
    }
    if(frostpunk_android_touch_text((const uint16_t*)(uintptr_t)texts[0],lengths[0],combined,used))
        memcpy(previous,language,sizeof(previous));
}
