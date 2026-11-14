#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "extra_aot.h"
typedef struct {uint64_t va,target;size_t size;const char *bytes;} test_case;
#include "windows_extra_aot_cases.h"
#include "windows_switch_aot_cases.h"
#include "windows_switch_extension_cases.h"
int64_t g_image_delta;
typedef struct {uint64_t target;CPU cpu;} boundary;
static boundary trace[8];static unsigned calls;
void dispatch(CPU *c,uint64_t target) {
    assert(calls<8);trace[calls].target=target;trace[calls].cpu=*c;trace[calls++].cpu.rip=target;
    if(target==UINT64_C(0x14108dbe5)||target==UINT64_C(0x14108dea6)){c->rip=target;return;}
    assert(target==UINT64_C(0xe5ff0000)||target==UINT64_C(0x140e1dea0)||target==UINT64_C(0x14108e7a0));
    c->rsp+=8;c->rax=UINT64_C(0x123456789abcdef0);c->rcx=0x1234;c->rdx^=UINT64_C(0xbadcafec01d0000);
    c->r8=0x8888;c->r9=0x9999;c->xmm[0].u64[0]=UINT64_C(0x5544332211);
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
#include "windows_scene_switch_baseline.h"
static void commit(const fp_image *image,const test_case *entry) {
    uint64_t address=(uintptr_t)image->base+entry->va-image->preferred_base;
    uint64_t low=address&~UINT64_C(4095),high=(address+entry->size+4095)&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)low,(SIZE_T)(high-low),MEM_COMMIT,PAGE_READWRITE));
    memcpy((void *)(uintptr_t)address,entry->bytes,entry->size);
}
int main(void) {
    fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
    image.base=VirtualAlloc((void *)(uintptr_t)image.preferred_base,image.size,MEM_RESERVE,PAGE_NOACCESS);
    assert((uintptr_t)image.base==image.preferred_base);
    fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
    for(unsigned i=0;i<12;++i)commit(&image,&cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&switch_cases[i]);
    for(unsigned i=0;i<5;++i)commit(&image,&switch_extension_cases[i]);commit(&image,&callback_cases[0]);
    for(unsigned i=0;i<4;++i)commit(&image,&scene_cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&compare_cases[i]);
    for(unsigned i=0;i<9;++i)commit(&image,&bounded_cases[i]);assert(fp_extra_aot_init(&image));
    void(*baselines[])(CPU*)={baseline_14108DEA6,baseline_14108DEEA,baseline_14108E010,baseline_14108E1B7};
    unsigned tested=0,boundaries=0,mutations=0;uint64_t random=UINT64_C(0xcafebabe14108e01);
    for(unsigned entry=0;entry<4;++entry)for(unsigned trial=0;trial<2048;++trial) {
        _Alignas(16) unsigned char object[512],stack[512],reader[2][256],payload[512];
        memset(object,0x6d,sizeof(object));memset(stack,0xa5,sizeof(stack));memset(reader,0x31,sizeof(reader));
        for(unsigned i=0;i<128;++i)wr32((uintptr_t)payload+i*4,UINT32_C(0x3f800000)+((trial+i)&7)*UINT32_C(0x80000));
        uint64_t vtable[2]={0,UINT64_C(0xe5ff0000)},ref=(uintptr_t)vtable;
        uint64_t chosen=(trial&1)?(uintptr_t)&ref:0;wr64((uintptr_t)payload+(entry==2?25:32),chosen);
        wr64((uintptr_t)object+0xf0,(trial&2)?(uintptr_t)&ref:0);
        for(unsigned j=0;j<2;++j){wr64((uintptr_t)reader[j]+0xc0,(uintptr_t)payload);wr32((uintptr_t)reader[j]+0xc8,trial&4?0:512);}
        CPU c;unsigned char *p=(unsigned char *)&c;
        for(size_t i=0;i<sizeof(c);++i){random^=random<<13;random^=random>>7;random^=random<<17;p[i]=(unsigned char)random;}
        c.cf=trial&1;c.pf=(trial>>1)&1;c.af=(trial>>2)&1;c.zf=(trial>>3)&1;c.sf=(trial>>4)&1;c.of=(trial>>5)&1;
        c.mxcsr=0x1f80;c.rip=scene_cases[entry].va;c.rbx=(uintptr_t)object;c.rsp=(uintptr_t)stack+128;c.rbp=c.rsp+96;
        c.r8=(uintptr_t)reader[0];c.r9=(uintptr_t)reader[(trial>>3)&1];c.rdi=chosen;
        for(unsigned lane=0;lane<4;++lane){c.xmm[6].u32[lane]=UINT32_C(0x3f800000);c.xmm[7].u32[lane]=UINT32_C(0x40000000);}
        wr64(c.rsp,UINT64_C(0x406954e1c335fde8));
        unsigned char saved_object[512],saved_stack[512],saved_reader[2][256],saved_payload[512];
        memcpy(saved_object,object,512);memcpy(saved_stack,stack,512);memcpy(saved_reader,reader,sizeof(reader));memcpy(saved_payload,payload,512);
        CPU expected=c;calls=0;memset(trace,0,sizeof(trace));baselines[entry](&expected);
        unsigned expected_calls=calls;boundary expected_trace[8];memcpy(expected_trace,trace,sizeof(trace));
        unsigned char expected_object[512],expected_stack[512],expected_reader[2][256];
        memcpy(expected_object,object,512);memcpy(expected_stack,stack,512);memcpy(expected_reader,reader,sizeof(reader));
        memcpy(object,saved_object,512);memcpy(stack,saved_stack,512);memcpy(reader,saved_reader,sizeof(reader));memcpy(payload,saved_payload,512);
        calls=0;memset(trace,0,sizeof(trace));void(*fn)(CPU*)=fp_extra_aot_lookup(c.rip);assert(fn);fn(&c);
        if(calls!=expected_calls||memcmp(&c,&expected,sizeof(c))||memcmp(trace,expected_trace,sizeof(trace)))fprintf(stderr,"scene mismatch entry=%u trial=%u calls=%u expected=%u\n",entry,trial,calls,expected_calls);
        assert(calls==expected_calls&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(trace,expected_trace,sizeof(trace)));
        assert(c.rsp==(uintptr_t)stack+128&&rd64(c.rsp)==UINT64_C(0x406954e1c335fde8));
        assert(!memcmp(object,expected_object,512)&&!memcmp(stack,expected_stack,512)&&!memcmp(reader,expected_reader,sizeof(reader))&&!memcmp(payload,saved_payload,512));
        ++tested;boundaries+=calls;
    }
    for(unsigned i=0;i<4;++i)for(unsigned byte=0;byte<scene_cases[i].size;++byte) {
        unsigned char *at=(unsigned char *)(uintptr_t)scene_cases[i].va+byte;*at^=1;
        assert(!fp_extra_aot_init(&image)&&!fp_extra_aot_lookup(scene_cases[i].va));*at^=1;assert(fp_extra_aot_init(&image));++mutations;
    }
    printf("{\"success\":true,\"entries\":4,\"full_CPU_stack_reader_object_cases\":%u,\"exact_dispatch_boundaries\":%u,\"original_byte_mutations_rejected\":%u,\"stack_pop_added\":false,\"original_bytes_executed\":false}\n",tested,boundaries,mutations);
    VirtualFree(image.base,0,MEM_RELEASE);return 0;
}
