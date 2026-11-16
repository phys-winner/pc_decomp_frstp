#include "mobile_detail.h"
#include "port_settings.h"
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#endif

static _Atomic int detail_override=-2;

static int detail_property(void){
#ifdef __ANDROID__
    int saved=errno;char value[PROP_VALUE_MAX]={0};
    __system_property_get("debug.fp.detail",value);
    unsigned long parsed=strtoul(value,NULL,0);
    int mode=!value[0]?-1:parsed==10||parsed==20||parsed==30||parsed==40?(int)parsed:0;
    errno=saved;return mode;
#else
    return -1;
#endif
}

void fp_mobile_detail_refresh(void){
    atomic_store_explicit(&detail_override,detail_property(),memory_order_relaxed);
}

unsigned fp_test_detail_mode(CPU *cpu){
    int mode=atomic_load_explicit(&detail_override,memory_order_relaxed);
    if(mode==-2){
        mode=detail_property();
        atomic_store_explicit(&detail_override,mode,memory_order_relaxed);
    }
    /* Distant small objects belong to the launcher's "buildings and objects" group. */
    if(mode<0)mode=frostpunk_mobile_graphics_reduced(FP_GFX_BUILDINGS)?40:0;
    if(!mode||!cpu||!cpu->rsp||cpu->rsp>UINT64_MAX-8)return 0;
    uint64_t address=(uintptr_t)cpu;
    if(cpu->rsp<address+sizeof(*cpu)&&address<cpu->rsp+8)return 0;
    return rd64(cpu->rsp)==UINT64_C(0x140eb3f1a)?(unsigned)mode:0;
}

float fp_test_detail_threshold(float original,unsigned mode){
    switch(mode){case 10:return .01f;case 20:return .02f;case 30:return .03f;case 40:return .04f;default:return original;}
}
