/* Reuse the exact portable bodies already checked by the Windows CPU oracles. */
#include <string.h>
#include "extra_aot.h"

typedef struct {uint64_t va;size_t size;const char *original;void(*fn)(CPU*);} extra_entry;
#include "../../native/windows/extra_aot_bodies.inc"
#include "mobile_callbacks.inc"
static void callback_leaf_1413C2038(CPU *c){
    c->rip=UINT64_C(0x1413c2038);c->rax=rd64(c->rcx);
    c->rip=UINT64_C(0x1413c203b);dispatch_jmp(c,rd64(c->rax+UINT64_C(0x408)));
}
static const extra_entry callback_entry={UINT64_C(0x1413c2038),9,"\x48\x8b\x01\xff\xa0\x08\x04\x00\x00",callback_leaf_1413C2038};
static int verified;
static uint16_t u16(const unsigned char *p){uint16_t v;memcpy(&v,p,2);return v;}
static uint32_t u32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t u64(const unsigned char *p){uint64_t v;memcpy(&v,p,8);return v;}
static int fits(size_t at,size_t bytes,size_t size){return at<=size && bytes<=size-at;}

int fp_android_extra_aot_init(const void *mapped_pe,size_t image_size) {
    verified=0;const unsigned char *base=mapped_pe;
    if(!base || !fits(0,64,image_size) || u16(base)!=0x5a4d)return 0;
    uint32_t pe=u32(base+0x3c);
    if(!fits(pe,24,image_size) || memcmp(base+pe,"PE\0\0",4) || u16(base+pe+4)!=0x8664)return 0;
    uint16_t count=u16(base+pe+6),optional_size=u16(base+pe+20);
    size_t optional=(size_t)pe+24,sections=optional+optional_size;
    if(!count || count>96 || optional_size<112 || !fits(optional,optional_size,image_size) ||
       u16(base+optional)!=0x20b || u64(base+optional+24)!=UINT64_C(0x140000000) ||
       !fits(sections,(size_t)count*40,image_size))return 0;
    uint32_t declared=u32(base+optional+56);
    if(!declared || declared>image_size)return 0;
    const size_t count_entries=sizeof(entries)/sizeof(*entries);
    const size_t count_mobile=sizeof(mobile_callbacks)/sizeof(*mobile_callbacks);
    for(size_t i=0;i<=count_entries+count_mobile;++i) {
        const extra_entry *entry=i<count_entries?&entries[i]:
            i<count_entries+count_mobile?&mobile_callbacks[i-count_entries]:&callback_entry;
        if(entry->va<UINT64_C(0x140000000))return 0;
        uint64_t offset=entry->va-UINT64_C(0x140000000);int code=0;
        if(offset>SIZE_MAX || !fits((size_t)offset,entry->size,declared))return 0;
        for(unsigned j=0;j<count;++j) {
            const unsigned char *section=base+sections+(size_t)j*40;
            uint32_t rva=u32(section+12),bytes=u32(section+8);
            if((u32(section+36)&0x20000000) && offset>=rva &&
               offset-rva<=bytes && entry->size<=bytes-(offset-rva)){code=1;break;}
        }
        if(!code || memcmp(base+(size_t)offset,entry->original,entry->size))return 0;
    }
    verified=1;return 1;
}
void (*fp_android_extra_aot_lookup(uint64_t original_va))(CPU *) {
    if(verified && original_va==callback_entry.va)return callback_entry.fn;
    if(verified)for(size_t i=0;i<sizeof(entries)/sizeof(*entries);++i)
        if(entries[i].va==original_va)return entries[i].fn;
    if(verified) {
        size_t low=0,high=sizeof(mobile_callbacks)/sizeof(*mobile_callbacks);
        while(low<high){size_t mid=low+(high-low)/2;
            if(mobile_callbacks[mid].va<original_va)low=mid+1;else high=mid;}
        if(low<sizeof(mobile_callbacks)/sizeof(*mobile_callbacks) && mobile_callbacks[low].va==original_va)
            return mobile_callbacks[low].fn;
    }
    return NULL;
}
