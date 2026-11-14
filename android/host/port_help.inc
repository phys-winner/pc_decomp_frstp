extern void frostpunk_android_show_help(const uint16_t*,uint32_t,const uint16_t*,uint32_t,const uint16_t*,uint32_t) __attribute__((weak));

static void fp_port_gesture_help(CPU *cpu) {
    if(!frostpunk_android_show_help)return;
    const char *keys[]={"UI/Android/GestureHelp","UI/Android/GestureHelpText","UI/Android/GestureHelpClose"};
    uint64_t texts[3];uint32_t lengths[3];
    CPU call=*cpu;uint64_t stack=(cpu->rsp-512)&~UINT64_C(15);
    int64_t image_delta=g_image_delta;
    uint64_t localization=rd64(UINT64_C(0x142983988)+(uint64_t)image_delta);
    if(!localization)return;
    for(unsigned i=0;i<3;++i){
        call.rsp=stack;call.rcx=localization;call.rdx=(uintptr_t)keys[i];
        call.r8=UINT64_C(0x142980530)+(uint64_t)image_delta;call.r9=0;
        wr64(stack+0x20,1);wr64(stack+0x28,1);
        push64(&call,UINT64_C(0xe5ee0010));dispatch(&call,UINT64_C(0x140E84CD0));
        if(call.rsp!=stack||!call.rax)return;
        texts[i]=rd64(call.rax);lengths[i]=0;if(!texts[i])return;
        while(lengths[i]<8192&&rd16(texts[i]+2*lengths[i]))++lengths[i];
        if(!lengths[i]||lengths[i]==8192)return;
    }
    frostpunk_android_show_help((const uint16_t*)(uintptr_t)texts[0],lengths[0],
        (const uint16_t*)(uintptr_t)texts[1],lengths[1],(const uint16_t*)(uintptr_t)texts[2],lengths[2]);
}
