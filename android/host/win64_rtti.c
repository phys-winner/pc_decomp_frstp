/* Win64 __RTDynamicCast for the original SFX context public-SI closure.
 * Metadata stays data: no vtable entry or original callback is executed.
 * Microsoft RTTI's SI search looks for source AFTER target in the base
 * array. Merely finding both types would incorrectly admit direct API
 * upcasts/same-type casts that native __RTDynamicCast rejects. */
#include "win64_rtti.h"
#include "win64.h"
#include "win64_stdio.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "win64_rtti_names.inc"

typedef struct {uint32_t signature,offset,construction,type,hierarchy,self;} rtti_locator;
typedef struct {uint32_t signature,attributes,count,array;} rtti_hierarchy;
typedef struct {uint32_t type,contained;int32_t member,vbpointer,vbentry;uint32_t attributes,hierarchy;} rtti_base;
_Static_assert(sizeof(rtti_locator)==24,"Win64 COL24");
_Static_assert(sizeof(rtti_hierarchy)==16,"Win64 CHD16");
_Static_assert(sizeof(rtti_base)==28,"Win64 BCD28");
static uint64_t image_base;
static size_t image_size;
/* The game's only uncached name() callers are ShapeRenderNode's 11 queue
 * diagnostics. Names are measured by the native VCRUNTIME oracle, not parsed
 * by an approximate demangler. Cache and Win64 SLIST storage retain their ABI.
 * Only this provider writes that list; serialize publication with cache reads. */
static atomic_flag rtti_name_lock=ATOMIC_FLAG_INIT;
static _Noreturn void rtti_stop(const char *why){fprintf(stderr,"STOP Win64 RTTI %s\n",why);fflush(stderr);_Exit(73);}
int win64_rtti_init(const void *mapped_image,size_t image_bytes){
    image_base=0;image_size=0;
    uint64_t base=(uint64_t)(uintptr_t)mapped_image;
    if(!base||image_bytes<24||image_bytes>UINT32_MAX||base>UINT64_MAX-image_bytes)return 0;
    image_base=base;image_size=image_bytes;return 1;
}
static void *rtti_image(uint64_t address,size_t size){
    if(!image_base||address<image_base||address-image_base>image_size||size>image_size-(address-image_base))
        rtti_stop("metadata outside initialized main image");
    return win64_pointer(address,size,0);
}
static uint64_t rtti_relative(uint32_t relative,size_t size){
    uint64_t address=image_base+(uint64_t)relative;
    (void)rtti_image(address,size);return address;
}
static void rtti_read(uint64_t address,void *output,size_t size){memcpy(output,rtti_image(address,size),size);}
static const char *rtti_name(uint64_t descriptor){
    (void)rtti_image(descriptor,17);
    uint64_t address=descriptor+16;
    for(unsigned i=0;i<512;++i){unsigned char value;memcpy(&value,rtti_image(address+i,1),1);if(!value)return rtti_image(address,i+1);}
    rtti_stop("type descriptor name exceeds bounded profile");
}
static int rtti_same(uint64_t first,uint64_t second){return first==second||!strcmp(rtti_name(first),rtti_name(second));}
static void rtti_type_info_name(CPU *c){
    int saved_errno=errno;uint32_t saved_error=win64_get_error();
    while(atomic_flag_test_and_set_explicit(&rtti_name_lock,memory_order_acquire)){}
    uint64_t cached;memcpy(&cached,win64_pointer(c->rcx,8,0),8);
    /* Native accepts a null/invalid unused root once the cache is populated. */
    if(cached){c->rax=cached;goto done;}
    if(c->rcx>UINT64_MAX-520)rtti_stop("type_info data address overflow");
    char decorated[512];unsigned length;
    for(length=0;length<sizeof(decorated);++length){
        memcpy(&decorated[length],win64_pointer(c->rcx+8+length,1,0),1);
        if(!decorated[length])break;
    }
    if(length==sizeof(decorated))rtti_stop("type_info name exceeds bounded diagnostic profile");
    const char *name=NULL;
    for(unsigned i=0;i<sizeof(rtti_diagnostic_names)/sizeof(rtti_diagnostic_names[0]);++i)
        if(!strcmp(decorated,rtti_diagnostic_names[i].decorated)){name=rtti_diagnostic_names[i].name;break;}
    if(!name)rtti_stop("uncached type_info name outside original diagnostic closure");
    if(!c->rdx||(c->rdx&15))rtti_stop("type_info SLIST root requires 16-byte alignment");
    uint64_t root[2];void *root_storage=win64_pointer(c->rdx,16,1);
    memcpy(root,root_storage,16);
    if(root[1]&15)rtti_stop("type_info SLIST reserved bits outside Win64 profile");
    void *cache_storage=win64_pointer(c->rcx,8,1);
    size_t count=strlen(name)+1;uint64_t node_address=win64_alloc(16+count,1);
    if(!node_address){win64_stdio_set_errno(12);c->rax=0;goto done;}
    if(node_address&15)rtti_stop("type_info allocator violated SLIST alignment");
    uint64_t *node=(uint64_t *)(uintptr_t)node_address;
    node[0]=root[1];node[1]=0;memcpy((unsigned char *)node+16,name,count);
    cached=node_address+16;memcpy(cache_storage,&cached,8);
    /* Native Win64 HeaderX64: depth16, sequence48 (incremented by two),
     * followed by the 16-aligned NextEntry pointer. */
    root[0]=((root[0]+UINT64_C(0x20000))&~UINT64_C(0xffff))|((root[0]+1)&UINT64_C(0xffff));
    root[1]=node_address;memcpy(root_storage,root,16);c->rax=cached;
done:
    atomic_flag_clear_explicit(&rtti_name_lock,memory_order_release);
    win64_set_error(saved_error);errno=saved_errno;
}
int win64_rtti_call(CPU *c,const char *name){
    if(!strcmp(name,"__std_type_info_name")){rtti_type_info_name(c);return 1;}
    if(strcmp(name,"__RTDynamicCast"))return 0;
    int saved_errno=errno;
    /* Native's null fast path ignores every other argument, even isReference. */
    if(!c->rcx){c->rax=0;errno=saved_errno;return 1;}
    if((int32_t)c->rdx)rtti_stop("nonzero VfDelta outside public single-inheritance profile");
    if(c->rsp>UINT64_MAX-48)rtti_stop("fifth argument stack address overflow");
    uint64_t reference;memcpy(&reference,win64_pointer(c->rsp+40,8,0),8);
    if((uint32_t)reference)rtti_stop("reference cast requires unsupported guest bad_cast exception");
    (void)rtti_name(c->r8);(void)rtti_name(c->r9);
    uint64_t vtable,locator_address;
    memcpy(&vtable,win64_pointer(c->rcx,8,0),8);
    if(vtable<8)rtti_stop("vtable prefix underflow");
    rtti_read(vtable-8,&locator_address,8);
    rtti_locator locator;rtti_read(locator_address,&locator,sizeof(locator));
    if(locator.signature!=1||locator.offset||locator.construction||
       rtti_relative(locator.self,sizeof(locator))!=locator_address)
        rtti_stop("unsupported locator revision/offset/construction/self");
    rtti_hierarchy hierarchy;rtti_read(rtti_relative(locator.hierarchy,sizeof(hierarchy)),&hierarchy,sizeof(hierarchy));
    if(hierarchy.signature||hierarchy.attributes||!hierarchy.count||hierarchy.count>64)
        rtti_stop("multiple/virtual/ambiguous or oversized class hierarchy");
    uint64_t array=rtti_relative(hierarchy.array,hierarchy.count*4u);
    rtti_base bases[64];uint64_t types[64];
    for(unsigned i=0;i<hierarchy.count;++i){
        uint32_t relative;rtti_read(array+i*4u,&relative,4);
        rtti_read(rtti_relative(relative,sizeof(rtti_base)),&bases[i],sizeof(rtti_base));
        rtti_base *base=&bases[i];
        if(base->member||base->vbpointer!=-1||base->vbentry||
           (base->attributes!=0&&base->attributes!=0x40)||base->contained!=hierarchy.count-i-1)
            rtti_stop("nonpublic/virtual/nonzero PMD or non-chain base descriptor");
        types[i]=rtti_relative(base->type,17);(void)rtti_name(types[i]);
        if(base->attributes&0x40)(void)rtti_relative(base->hierarchy,sizeof(rtti_hierarchy));
    }
    if(types[0]!=rtti_relative(locator.type,17))rtti_stop("locator type differs from complete class");
    uint64_t result=0;
    /* Preserve native's pointer-identity pass precedence before name fallback. */
    for(unsigned pass=0;pass<2;++pass){
        for(unsigned i=0;i<hierarchy.count;++i){
            int target=pass?rtti_same(types[i],c->r9):types[i]==c->r9;
            if(target){
                for(unsigned j=i+1;j<hierarchy.count;++j)
                    if(pass?rtti_same(types[j],c->r8):types[j]==c->r8){result=c->rcx;break;}
                c->rax=result;errno=saved_errno;return 1;
            }
        }
    }
    c->rax=0;errno=saved_errno;return 1;
}
