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
static boundary trace[16];static unsigned calls,trial_index;
void dispatch(CPU *c,uint64_t target) {
    assert(calls<16);trace[calls].target=target;trace[calls].cpu=*c;trace[calls++].cpu.rip=target;
    if(target==UINT64_C(0x140ed2055) || target==UINT64_C(0x140f9a53b) ||
       target==UINT64_C(0x140f9a633) || target==UINT64_C(0x140f9a549) || target==UINT64_C(0x141affc3f)) {
        c->rip=target;return;
    }
    assert(target==UINT64_C(0x140ed2630) || target==UINT64_C(0x140e26c40) ||
           target==UINT64_C(0x140e9d330) || target==UINT64_C(0x140e26c90) ||
           target==UINT64_C(0x140f94c10) || target==UINT64_C(0x141afeb70) ||
           target==UINT64_C(0x141b01410) || target==UINT64_C(0xe5ff0000));
    c->rsp+=8;c->rax=target==UINT64_C(0xe5ff0000)?(trial_index>>1)&3:UINT64_C(0x123456789abcdef0);
    c->rcx=UINT64_C(0x1234);c->rdx^=UINT64_C(0x8badf00d12340000);c->r8=UINT64_C(0xfeedface12345678);
    c->r9=UINT64_C(0x99);c->r10=UINT64_C(0x1010);c->r11=UINT64_C(0x1111);
    c->xmm[0].u64[0]=UINT64_C(0x8877665544332211);
    if(target==UINT64_C(0x140f94c10))wrss(c->rsp+0x24,0.5f+(float)(trial_index&3));
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
#include "windows_switch_extension_baseline.h"

static void commit(uint64_t address,size_t size) {
    uint64_t low=address&~UINT64_C(4095),high=(address+size+4095)&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)low,(SIZE_T)(high-low),MEM_COMMIT,PAGE_READWRITE));
}
static const test_case *all_entry(unsigned n) {
    return n<12?&cases[n]:n<14?&switch_cases[n-12]:&switch_extension_cases[n-14];
}
int main(void) {
    fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
    image.base=VirtualAlloc((void *)(uintptr_t)image.preferred_base,image.size,MEM_RESERVE,PAGE_NOACCESS);
    assert((uintptr_t)image.base==image.preferred_base);
    fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
    for(unsigned i=0;i<19;++i){const test_case *e=all_entry(i);commit(e->va,e->size);memcpy((void *)(uintptr_t)e->va,e->bytes,e->size);}
    commit(callback_cases[0].va,callback_cases[0].size);
    memcpy((void *)(uintptr_t)callback_cases[0].va,callback_cases[0].bytes,callback_cases[0].size);
    for(unsigned i=0;i<4;++i){commit(scene_cases[i].va,scene_cases[i].size);memcpy((void *)(uintptr_t)scene_cases[i].va,scene_cases[i].bytes,scene_cases[i].size);}
    for(unsigned i=0;i<2;++i){commit(compare_cases[i].va,compare_cases[i].size);memcpy((void *)(uintptr_t)compare_cases[i].va,compare_cases[i].bytes,compare_cases[i].size);}
    for(unsigned i=0;i<9;++i){commit(bounded_cases[i].va,bounded_cases[i].size);memcpy((void *)(uintptr_t)bounded_cases[i].va,bounded_cases[i].bytes,bounded_cases[i].size);}
    commit(UINT64_C(0x142a88df8),0x140);commit(UINT64_C(0x1421f52b8),16);commit(UINT64_C(0x14400c200),8);
    wrss(UINT64_C(0x142a88df8),1.25f);wrss(UINT64_C(0x142a88dfc),2.5f);wrss(UINT64_C(0x1421f52b8),3.75f);
    for(unsigned j=0;j<4;++j)wr32(UINT64_C(0x142a88f20)+4*j,UINT32_C(0x11223344)+j);
    assert(fp_extra_aot_init(&image));
    unsigned tested=0,boundaries=0,returns=0;uint64_t random=UINT64_C(0xcafebabedeadbeef);
    void(*baseline[])(CPU*)={baseline_140ED24F5,baseline_140F9A5D2,baseline_140F9A61E,baseline_140F9A6AB,baseline_141AFFBEF};
    for(unsigned entry=0;entry<5;++entry)for(trial_index=0;trial_index<1024;++trial_index) {
        _Alignas(16) unsigned char object[1024],stack[512],payload[512],node1[0x560],node2[0x560];
        memset(object,0x6d,sizeof(object));memset(stack,0xa5,sizeof(stack));memset(payload,0x39,sizeof(payload));
        memset(node1,0,sizeof(node1));memset(node2,0,sizeof(node2));
        uint64_t vtable=UINT64_C(0xe5ff0000),record=(uintptr_t)&vtable,list[2]={(trial_index&1)?(uintptr_t)&record:0,0};
        wr64((uintptr_t)object+0xc8,(uintptr_t)list);wr32((uintptr_t)object+0xd0,trial_index&2?0:16);
        for(unsigned j=0;j<4;++j)wr64((uintptr_t)object+0x1a8+8*j,UINT64_C(0x12340000)+j);
        wr64((uintptr_t)object+0x168,(uintptr_t)payload);wrss((uintptr_t)payload+0x100,1.25f);
        wrss((uintptr_t)payload+0x104,4.5f);wrss((uintptr_t)object+0xc4,2.0f);wrss((uintptr_t)stack+128+0x24,0.5f);
        unsigned mode=trial_index%5;uint32_t descriptors[2]={mode==2?7:9,mode==3?7:9};
        wr64((uintptr_t)node1,(uintptr_t)&descriptors[0]);wr64((uintptr_t)node2,(uintptr_t)&descriptors[1]);
        wr64((uintptr_t)node1+0x548,mode>=3?(uintptr_t)node2:0);wr32((uintptr_t)object+8,7);
        wr64(UINT64_C(0x14400c200),mode?(uintptr_t)node1:0);
        CPU initial;memset(&initial,0xa5,sizeof(initial));initial.mxcsr=0x1f80;
        initial.cf=trial_index&1;initial.zf=(trial_index>>1)&1;initial.sf=(trial_index>>2)&1;initial.of=(trial_index>>3)&1;
        initial.pf=(trial_index>>4)&1;initial.af=(trial_index>>5)&1;
        for(unsigned j=0;j<16;++j){random^=random<<13;random^=random>>7;random^=random<<17;initial.xmm[j].u64[0]=random;initial.xmm[j].u64[1]=~random;}
        initial.rdi=(uintptr_t)object;initial.rsp=(uintptr_t)stack+128;initial.rip=switch_extension_cases[entry].va;
        unsigned char saved_object[1024],saved_stack[512],expected_object[1024],expected_stack[512];
        memcpy(saved_object,object,sizeof(object));memcpy(saved_stack,stack,sizeof(stack));
        CPU expected=initial;calls=0;memset(trace,0,sizeof(trace));baseline[entry](&expected);
        unsigned expected_calls=calls;boundary expected_trace[16];memcpy(expected_trace,trace,sizeof(trace));
        memcpy(expected_object,object,sizeof(object));memcpy(expected_stack,stack,sizeof(stack));
        memcpy(object,saved_object,sizeof(object));memcpy(stack,saved_stack,sizeof(stack));
        CPU actual=initial;calls=0;memset(trace,0,sizeof(trace));void(*fn)(CPU*)=fp_extra_aot_lookup(initial.rip);assert(fn);fn(&actual);
        if(calls!=expected_calls || memcmp(&actual,&expected,sizeof(actual)) || memcmp(trace,expected_trace,sizeof(trace))) {
            fprintf(stderr,"mismatch entry=%u trial=%u actual_calls=%u expected_calls=%u\n",entry,trial_index,calls,expected_calls);
            const uint64_t *a=(const uint64_t*)&actual,*b=(const uint64_t*)&expected;
            for(unsigned j=0;j<sizeof(actual)/8;++j)if(a[j]!=b[j])fprintf(stderr,"word=%u actual=%llx expected=%llx\n",j,a[j],b[j]);
        }
        assert(calls==expected_calls && !memcmp(trace,expected_trace,sizeof(trace)) && !memcmp(&actual,&expected,sizeof(actual)));
        assert(!memcmp(object,expected_object,sizeof(object)) && !memcmp(stack,expected_stack,sizeof(stack)));
        boundaries+=calls;returns+=entry==4 && (mode==1 || mode==4);++tested;
    }
    unsigned mutations=0;
    for(unsigned i=0;i<5;++i)for(unsigned byte=0;byte<switch_extension_cases[i].size;++byte) {
        unsigned char *at=(unsigned char *)(uintptr_t)switch_extension_cases[i].va+byte;*at^=1;
        assert(!fp_extra_aot_init(&image) && !fp_extra_aot_lookup(switch_extension_cases[i].va));*at^=1;
        assert(fp_extra_aot_init(&image));++mutations;
    }
    printf("{\"success\":true,\"entries\":5,\"branch_full_CPU_stack_object_cases\":%u,\"exact_dispatch_boundaries\":%u,\"RET_stack_paths\":%u,\"original_byte_mutations_rejected\":%u,\"original_bytes_executed\":false}\n",tested,boundaries,returns,mutations);
    assert(VirtualFree(image.base,0,MEM_RELEASE));return 0;
}
