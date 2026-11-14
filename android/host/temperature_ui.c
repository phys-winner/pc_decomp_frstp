#include "temperature_ui.h"
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif

static uint64_t child(uint64_t parent,const char *name){
    if(!parent)return 0;
    unsigned count=0;
    for(uint64_t node=rd64(parent+0x220);node&&count++<512;node=rd64(node+0x240)){
        const char *actual=(const char*)(uintptr_t)rd64(node+0x1d8);
        if(actual&&!strcmp(actual,name))return node;
    }
    return 0;
}

/* The frost picture's backdrop filter rendered as a checkerboard on the Mali tablet (2026-11-13), but on
 * Adreno/Turnip it is the PC look: frosted glass inside the thermometer ring.  Without the filter its
 * blur_texture child spans the whole top bar as a grey smear at -40 C, so drop it only on Mali.
 * debug.fp.temperature_backdrop=1/0 forces keep/drop (read once). */
static int keep_backdrop(void){
#ifdef __ANDROID__
    static _Atomic int cached=-1;
    int value=atomic_load_explicit(&cached,memory_order_relaxed);
    if(value<0){
        char text[PROP_VALUE_MAX]={0},hardware[PROP_VALUE_MAX]={0};int saved=errno;
        if(__system_property_get("debug.fp.temperature_backdrop",text)>0&&(text[0]=='0'||text[0]=='1'))value=text[0]=='1';
        else value=!(__system_property_get("ro.hardware.vulkan",hardware)>0&&!strcmp(hardware,"mali"));
        atomic_store_explicit(&cached,value,memory_order_relaxed);errno=saved;
    }
    return value;
#else
    return 1;
#endif
}

void fp_temperature_ui_before(CPU *cpu){
    static const char *const path[]={"FRAME","HUD","TOP","EXT_RESOURCES_PANEL","ANI","TEMPERATURE_OVERLAY_BUTTON","freeze_ball_blur"};
    if(keep_backdrop())return;
    uint64_t node=cpu->rcx;
    for(unsigned i=0;i<sizeof(path)/sizeof(*path);++i){node=child(node,path[i]);if(!node)return;}
    if(rd64(node)!=GVA(0x141DA2AA8)||!(rd32(node+0x1e0)&UINT32_C(0x80000000)))return;
    // shortcut: Keep the alpha-only frost picture until its backdrop filter is corrected.
    CPU command=*cpu;command.rsp=(command.rsp-512)&~UINT64_C(15);
    command.rcx=node;command.rdx=UINT32_C(0x80000000);command.r8=0;
    push64(&command,UINT64_C(0xe5ee0010));dispatch(&command,UINT64_C(0x140EBE470));
}
