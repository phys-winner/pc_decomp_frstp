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
static boundary trace[8];static unsigned calls,returns;static uint32_t modes[2];
void dispatch(CPU *c,uint64_t target) {
    assert(calls<8);trace[calls].target=target;trace[calls].cpu=*c;trace[calls++].cpu.rip=target;
    if(target==UINT64_C(0x140eca026)){c->rip=target;return;}
    assert(target==UINT64_C(0x140ef3c00) || target==UINT64_C(0x140ef4d40) || target==UINT64_C(0x140ef5520));
    c->rsp+=8;
    c->rax=target==UINT64_C(0x140ef3c00)?modes[returns++]:0xbadcafe;
    c->rdx^=UINT64_C(0x8badf00d12340000);c->r8=UINT64_C(0xfeedface12345678);
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
#include "switch_original_baseline.h"

int main(void) {
    fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
    image.base=VirtualAlloc((void *)(uintptr_t)image.preferred_base,image.size,MEM_RESERVE,PAGE_NOACCESS);
    assert((uintptr_t)image.base==image.preferred_base);
    fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
    for(unsigned i=0;i<19;++i) {
        const test_case *entry=i<12?&cases[i]:i<14?&switch_cases[i-12]:&switch_extension_cases[i-14];
        uint64_t start=entry->va&~UINT64_C(4095),end=(entry->va+entry->size+4095)&~UINT64_C(4095);
        assert(VirtualAlloc((void *)(uintptr_t)start,(SIZE_T)(end-start),MEM_COMMIT,PAGE_READWRITE));
        memcpy((void *)(uintptr_t)entry->va,entry->bytes,entry->size);
    }
    uint64_t callback_page=callback_cases[0].va&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)callback_page,4096,MEM_COMMIT,PAGE_READWRITE));
    memcpy((void *)(uintptr_t)callback_cases[0].va,callback_cases[0].bytes,callback_cases[0].size);
    for(unsigned i=0;i<4;++i) {
        uint64_t page=scene_cases[i].va&~UINT64_C(4095);
        assert(VirtualAlloc((void *)(uintptr_t)page,8192,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void *)(uintptr_t)scene_cases[i].va,scene_cases[i].bytes,scene_cases[i].size);
    }
    for(unsigned i=0;i<2;++i) {
        uint64_t page=compare_cases[i].va&~UINT64_C(4095);
        assert(VirtualAlloc((void *)(uintptr_t)page,4096,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void *)(uintptr_t)compare_cases[i].va,compare_cases[i].bytes,compare_cases[i].size);
    }
    for(unsigned i=0;i<9;++i) {
        uint64_t page=bounded_cases[i].va&~UINT64_C(4095);
        assert(VirtualAlloc((void *)(uintptr_t)page,4096,MEM_COMMIT,PAGE_READWRITE));
        memcpy((void *)(uintptr_t)bounded_cases[i].va,bounded_cases[i].bytes,bounded_cases[i].size);
    }
    assert(fp_extra_aot_init(&image));
    unsigned tested=0,boundaries=0;uint64_t random=UINT64_C(0xcafebabedeadbeef);
    for(unsigned entry=0;entry<2;++entry)for(unsigned trial=0;trial<1024;++trial) {
        unsigned char object[0x380],stack[512],record[64];uint64_t payload=0x12345678,wrapper;
        memset(object,0x6d,sizeof(object));memset(stack,0xa5,sizeof(stack));memset(record,0x39,sizeof(record));
        wrapper=(trial&2)?(uintptr_t)&payload:0;wr64((uintptr_t)object+0x330,(trial&1)?(uintptr_t)&wrapper:0);
        wr64((uintptr_t)record,(trial&4)?0x1234:0);modes[0]=(trial>>3)&3;modes[1]=(trial>>5)&3;
        CPU initial;memset(&initial,0xa5,sizeof(initial));
        initial.cf=trial&1;initial.zf=(trial>>1)&1;initial.sf=(trial>>2)&1;initial.of=(trial>>3)&1;
        initial.pf=(trial>>4)&1;initial.af=(trial>>5)&1;initial.mxcsr=0x1f80;
        for(unsigned lane=0;lane<16;++lane){random^=random<<13;random^=random>>7;random^=random<<17;initial.xmm[lane].u64[0]=random;initial.xmm[lane].u64[1]=~random;}
        initial.rdi=(uintptr_t)object;initial.rdx=(uintptr_t)record;initial.rbx=(uintptr_t)record;
        initial.rsp=(uintptr_t)stack+128;initial.rip=switch_cases[entry].va;
        unsigned char saved_stack[512],saved_record[64];memcpy(saved_stack,stack,sizeof(stack));memcpy(saved_record,record,sizeof(record));
        CPU expected=initial;calls=returns=0;memset(trace,0,sizeof(trace));
        if(!entry)baseline_140EC9F6D(&expected);else baseline_140ECA00A(&expected);
        unsigned expected_calls=calls,expected_returns=returns;boundary expected_trace[8];memcpy(expected_trace,trace,sizeof(trace));
        unsigned char expected_stack[512],expected_record[64];memcpy(expected_stack,stack,sizeof(stack));memcpy(expected_record,record,sizeof(record));
        memcpy(stack,saved_stack,sizeof(stack));memcpy(record,saved_record,sizeof(record));
        CPU actual=initial;calls=returns=0;memset(trace,0,sizeof(trace));
        void(*fn)(CPU*)=fp_extra_aot_lookup(switch_cases[entry].va);assert(fn);fn(&actual);
        assert(calls==expected_calls && returns==expected_returns && !memcmp(trace,expected_trace,sizeof(trace)));
        assert(!memcmp(&actual,&expected,sizeof(actual)) && actual.rsp==initial.rsp);
        assert(!memcmp(stack,expected_stack,sizeof(stack)) && !memcmp(record,expected_record,sizeof(record)));
        assert(rd64((uintptr_t)object+0x330)==((trial&1)?(uintptr_t)&wrapper:0));
        boundaries+=calls;++tested;
    }
    unsigned mutations=0;
    for(unsigned i=0;i<2;++i)for(unsigned byte=0;byte<switch_cases[i].size;++byte) {
        unsigned char *at=(unsigned char *)(uintptr_t)switch_cases[i].va+byte;*at^=1;
        assert(!fp_extra_aot_init(&image) && !fp_extra_aot_lookup(switch_cases[i].va));*at^=1;
        assert(fp_extra_aot_init(&image));++mutations;
    }
    printf("{\"success\":true,\"missing_switch_entries\":2,\"branch_full_CPU_stack_memory_cases\":%u,\"exact_existing_dispatch_boundaries\":%u,\"original_byte_mutations_rejected\":%u,\"stack_pop_added\":false,\"original_bytes_executed\":false}\n",tested,boundaries,mutations);
    assert(VirtualFree(image.base,0,MEM_RELEASE));return 0;
}
