/* Optional investigation hook; not linked or enabled in the production runtime. */
#include "win64.h"
#include <stdio.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

void fp_ui_paint_probe(uint64_t node) {
#ifdef __ANDROID__
    static unsigned calls,enabled,logged;
    static uint64_t seen[128];static uint32_t ids[128];
    if(!(calls++&127u)){
        char value[PROP_VALUE_MAX]={0};
        enabled=__system_property_get("debug.fp.paint_probe",value)>0&&value[0]=='1';
    }
    if(!enabled||logged==128)return;
    const char *name=(const char*)(uintptr_t)rd64(node+0x1d8);if(!name)return;
    unsigned law=0;
    for(uint64_t parent=node,depth=0;parent&&depth<32;++depth,parent=rd64(parent+0x230)){
        const char *ancestor=(const char*)(uintptr_t)rd64(parent+0x1d8);
        if(ancestor&&!strcmp(ancestor,"EXT_SOCIOTECH_PANEL")){law=1;break;}
    }
    if(!law)return;
    uint32_t id=rd32(node+0x1e4);
    for(unsigned i=0;i<logged;++i)if(seen[i]==node&&ids[i]==id)return;
    seen[logged]=node;ids[logged++]=id;
    unsigned count=rd32(node+0x2d8);uint64_t array=rd64(node+0x2d0);
    fprintf(stderr,"FP_PAINT_PROBE name=%s id=%u templates=%u model_present=%u model_ready=%u flags=%x size=%.3fx%.3f scale=%.3fx%.3f inline=%u own_camera=%u perspective=%u fov=%.5f ortho=%.5f\n",
        name,id,count,(unsigned)(count&&array&&rd64(array)),(unsigned)(count&&array?rd8(array+8):0),
        rd32(node+0x1e0),rdss(node+0x100),rdss(node+0x104),rdss(node+0xf0),rdss(node+0xf4),
        rd8(node+0x380),rd8(node+0x384),rd8(node+0x385),rdss(node+0x378),rdss(node+0x37c));
#else
    (void)node;
#endif
}
