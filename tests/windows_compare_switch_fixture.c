#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <emmintrin.h>
#include <stdio.h>
#include <string.h>
#include "extra_aot.h"
typedef struct {uint64_t va,target;size_t size;const char *bytes;} test_case;
#include "windows_extra_aot_cases.h"
#include "windows_switch_aot_cases.h"
#include "windows_switch_extension_cases.h"
int64_t g_image_delta;
extern void fp_test_comiss_flags(const XMM *,const XMM *,uint64_t *);
static CPU captured;static uint64_t tail;static unsigned calls,epilog;
static void restore_frame(CPU *c) {
    c->xmm[6]=rdxm(c->rsp+0x20);c->rbx=rd64(c->rsp+0x40);c->rsi=rd64(c->rsp+0x48);
    c->rsp=flags_add(c,c->rsp,0x30,8);c->rdi=pop64(c);c->rsp+=8;
}
void dispatch(CPU *c,uint64_t target) {
    captured=*c;captured.rip=target;tail=target;++calls;c->rip=target;
    assert(target==UINT64_C(0x140dbd114)||target==UINT64_C(0x140dbd117));
    if(epilog){if(target==UINT64_C(0x140dbd114))SET8L(c->rax,!c->cf&&!c->zf);restore_frame(c);}
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
static void commit(const fp_image *image,const test_case *entry) {
    uint64_t address=(uintptr_t)image->base+entry->va-image->preferred_base;
    uint64_t low=address&~UINT64_C(4095),high=(address+entry->size+4095)&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)low,(SIZE_T)(high-low),MEM_COMMIT,PAGE_READWRITE));
    memcpy((void *)(uintptr_t)address,entry->bytes,entry->size);
}
int main(void) {
    fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
    image.base=VirtualAlloc((void *)(uintptr_t)image.preferred_base,image.size,MEM_RESERVE,PAGE_NOACCESS);
    assert((uintptr_t)image.base==image.preferred_base);fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
    for(unsigned i=0;i<12;++i)commit(&image,&cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&switch_cases[i]);
    for(unsigned i=0;i<5;++i)commit(&image,&switch_extension_cases[i]);commit(&image,&callback_cases[0]);
    for(unsigned i=0;i<4;++i)commit(&image,&scene_cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&compare_cases[i]);
    for(unsigned i=0;i<9;++i)commit(&image,&bounded_cases[i]);assert(fp_extra_aot_init(&image));
    const uint32_t edges[]={0,0x80000000,1,0x80000001,0x7fffff,0x807fffff,0x800000,0x80800000,0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00001,0xffc00001,0x7f800001,0xff800001};
    uint64_t random=UINT64_C(0xbadcafe140dbd134);unsigned tested=0,restored=0,mutations=0;unsigned host=_mm_getcsr();
    for(unsigned entry=0;entry<2;++entry)for(unsigned a=0;a<18;++a)for(unsigned b=0;b<18;++b)for(unsigned mode=0;mode<64;++mode) {
        CPU c;unsigned char *p=(unsigned char *)&c;for(size_t i=0;i<sizeof(c);++i){random^=random<<13;random^=random>>7;random^=random<<17;p[i]=(unsigned char)random;}
        c.cf=mode&1;c.pf=(mode>>1)&1;c.af=(mode>>2)&1;c.zf=(mode>>3)&1;c.sf=(mode>>4)&1;c.of=(mode>>5)&1;
        c.mxcsr=0x1f80;c.xmm[0].u32[0]=edges[a];c.xmm[6].u32[0]=edges[b];
        _Alignas(16) unsigned char stack[256];memset(stack,0x5a,sizeof(stack));c.rsp=(uintptr_t)stack+64;
        wr64(c.rsp,0);wr64(c.rsp+0x38,UINT64_C(0xe5a076543210abcd));
        uint64_t native_flags;_mm_setcsr(0x1f80);fp_test_comiss_flags(&c.xmm[0],&c.xmm[6],&native_flags);
        CPU expected=c;expected.cf=native_flags&1;expected.pf=(native_flags>>2)&1;expected.af=(native_flags>>4)&1;
        expected.zf=(native_flags>>6)&1;expected.sf=(native_flags>>7)&1;expected.of=(native_flags>>11)&1;
        if(!entry)SET8L(expected.rax,!expected.cf);expected.rip=compare_cases[entry].target;
        CPU expected_boundary=expected;epilog=mode&1;
        if(epilog){if(entry)SET8L(expected.rax,!expected.cf&&!expected.zf);restore_frame(&expected);++restored;}
        calls=0;tail=0;void(*fn)(CPU*)=fp_extra_aot_lookup(compare_cases[entry].va);assert(fn);fn(&c);
        assert(calls==1&&tail==compare_cases[entry].target&&!memcmp(&captured,&expected_boundary,sizeof(c))&&!memcmp(&c,&expected,sizeof(c)));
        assert(rd64((uintptr_t)stack+64)==0&&rd64((uintptr_t)stack+64+0x38)==UINT64_C(0xe5a076543210abcd));++tested;
    }
    for(unsigned i=0;i<2;++i)for(unsigned byte=0;byte<compare_cases[i].size;++byte) {
        unsigned char *at=(unsigned char *)(uintptr_t)compare_cases[i].va+byte;*at^=1;
        assert(!fp_extra_aot_init(&image)&&!fp_extra_aot_lookup(compare_cases[i].va));*at^=1;assert(fp_extra_aot_init(&image));++mutations;
    }
    _mm_setcsr(host);printf("{\"success\":true,\"entries\":2,\"native_compare_flags_full_CPU_cases\":%u,\"existing_frame_epilog_paths\":%u,\"original_byte_mutations_rejected\":%u,\"stack_pop_added\":false,\"original_bytes_executed\":false}\n",tested,restored,mutations);
    VirtualFree(image.base,0,MEM_RELEASE);return 0;
}
