#include <string.h>
#include "extra_aot.h"
typedef struct {uint64_t va;size_t size;const char *original;void(*fn)(CPU*);} extra_entry;
#include "extra_aot_bodies.inc"
static int verified;
int fp_extra_aot_init(const fp_image *image) {
    verified=0;
    if(image->preferred_base!=UINT64_C(0x140000000))return 0;
    for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i) {
        const extra_entry *e=&entries[i];uint64_t at=(uintptr_t)image->base+e->va-image->preferred_base;
        if(!fp_image_code(image,at) || !fp_image_code(image,at+e->size-1) ||
           memcmp((void *)(uintptr_t)at,e->original,e->size))return 0;
    }
    verified=1;return 1;
}
void (*fp_extra_aot_lookup(uint64_t original))(CPU *) {
    if(verified)for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i)
        if(entries[i].va==original)return entries[i].fn;
    return NULL;
}
