extern int frostpunk_android_touch_text(const uint16_t*,uint32_t,const uint16_t*,uint32_t) __attribute__((weak));

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
    CPU call=*cpu;uint64_t stack=(cpu->rsp-512)&~UINT64_C(15);
    for(unsigned i=0;i<2;++i){
        call.rsp=stack;call.rcx=localization;call.rdx=(uintptr_t)keys[i];
        call.r8=GVA(0x142980530);call.r9=0;
        wr64(stack+0x20,1);wr64(stack+0x28,0);
        push64(&call,UINT64_C(0xe5ee0010));dispatch(&call,UINT64_C(0x140E84CD0));
        if(call.rsp!=stack||!call.rax)return;
        texts[i]=rd64(call.rax);lengths[i]=0;if(!texts[i])return;
        while(lengths[i]<8192&&rd16(texts[i]+2*lengths[i]))++lengths[i];
        if(!lengths[i]||lengths[i]==8192)return;
    }
    if(frostpunk_android_touch_text((const uint16_t*)(uintptr_t)texts[0],lengths[0],
                                  (const uint16_t*)(uintptr_t)texts[1],lengths[1]))
        memcpy(previous,language,sizeof(previous));
}
