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
extern void fp_test_cmp32_flags(uint32_t,uint32_t,uint64_t *);
typedef struct {uint64_t target;CPU cpu;} boundary;
static boundary trace[4];static unsigned calls,current_entry;static uint64_t initial_rsp,initial_rbx;
static const uint64_t call_targets[]={UINT64_C(0x140e54010),UINT64_C(0x140e54550),UINT64_C(0x140e54350),UINT64_C(0x140e546f0),UINT64_C(0x140e54f80)};
void dispatch(CPU *c,uint64_t target) {
    assert(calls<4);trace[calls].target=target;trace[calls].cpu=*c;trace[calls++].cpu.rip=target;
    if(target==UINT64_C(0x140dbce78)||target==UINT64_C(0x140e54f12)){c->rip=target;return;}
    assert(current_entry>=4&&target==call_targets[current_entry-4]);
    assert(c->rsp==initial_rsp-8&&c->rcx==initial_rsp+0x30+(current_entry-4)*8&&c->rdx==initial_rbx);
    assert(rd32(c->rsp+40)==rd32(initial_rsp+0xb0));
    wr64(c->rcx,UINT64_C(0x1234567800000000)+current_entry);c->rax=c->rcx;
    c->rsp+=8;c->rcx=0x1234;c->rdx^=UINT64_C(0x8badf00d12340000);c->r8=UINT64_C(0xfeedface12345678);
    c->r9=0x99;c->r10=0x1010;c->r11=0x1111;c->xmm[0].u64[0]=UINT64_C(0x8877665544332211);
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
#include "windows_bounded_extension_baseline.h"
static void commit(const fp_image *image,const test_case *entry) {
    uint64_t address=(uintptr_t)image->base+entry->va-image->preferred_base;
    uint64_t low=address&~UINT64_C(4095),high=(address+entry->size+4095)&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)low,(SIZE_T)(high-low),MEM_COMMIT,PAGE_READWRITE));memcpy((void *)(uintptr_t)address,entry->bytes,entry->size);
}
int main(void) {
    fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
    image.base=VirtualAlloc((void *)(uintptr_t)image.preferred_base,image.size,MEM_RESERVE,PAGE_NOACCESS);assert((uintptr_t)image.base==image.preferred_base);
    fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
    for(unsigned i=0;i<12;++i)commit(&image,&cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&switch_cases[i]);
    for(unsigned i=0;i<5;++i)commit(&image,&switch_extension_cases[i]);commit(&image,&callback_cases[0]);
    for(unsigned i=0;i<4;++i)commit(&image,&scene_cases[i]);for(unsigned i=0;i<2;++i)commit(&image,&compare_cases[i]);
    for(unsigned i=0;i<9;++i)commit(&image,&bounded_cases[i]);assert(fp_extra_aot_init(&image));
    void(*baselines[])(CPU*)={baseline_140DBCE94,baseline_140DBCE9B,baseline_140DBCEA2,baseline_140DBCEA9,
        baseline_140E54E92,baseline_140E54EAC,baseline_140E54EC6,baseline_140E54EE0,baseline_140E54EFA};
    const uint32_t edges[]={0,1,2,0x7f,0x80,0xff,0x100,0xffff,0x10000,0x7ffffffe,0x7fffffff,0x80000000,0x80000001,0xfffffffe,0xffffffff,0x55aa55aa};
    unsigned tested=0,native_flags_cases=0,call_cases=0,boundaries=0,mutations=0;uint64_t random=UINT64_C(0x9988776655443322);
    for(current_entry=0;current_entry<9;++current_entry)for(unsigned trial=0;trial<(current_entry<4?8448u:2048u);++trial) {
        CPU c;unsigned char *p=(unsigned char *)&c;for(size_t i=0;i<sizeof(c);++i){random^=random<<13;random^=random>>7;random^=random<<17;p[i]=(unsigned char)random;}
        c.cf=trial&1;c.pf=(trial>>1)&1;c.af=(trial>>2)&1;c.zf=(trial>>3)&1;c.sf=(trial>>4)&1;c.of=(trial>>5)&1;c.mxcsr=0x1f80;c.rip=bounded_cases[current_entry].va;
        _Alignas(16) unsigned char stack[512];memset(stack,0xa5,sizeof(stack));initial_rsp=c.rsp=(uintptr_t)stack+128;initial_rbx=c.rbx;
        wr64(c.rsp,0);wr32(c.rsp+0xb0,(uint32_t)random);unsigned char saved_stack[512];memcpy(saved_stack,stack,512);
        if(current_entry<4&&trial<256){SET32(c.rax,edges[trial/16]);SET32(c.rbx,edges[trial%16]);}
        CPU native_expected=c;
        if(current_entry<4) {
            uint64_t f;fp_test_cmp32_flags((uint32_t)c.rax,(uint32_t)c.rbx,&f);
            native_expected.cf=f&1;native_expected.pf=(f>>2)&1;native_expected.af=(f>>4)&1;native_expected.zf=(f>>6)&1;native_expected.sf=(f>>7)&1;native_expected.of=(f>>11)&1;
            int32_t a=(int32_t)c.rax,b=(int32_t)c.rbx;unsigned value=current_entry==0?a<b:current_entry==1?a<=b:current_entry==2?a>=b:a>b;
            SET8L(native_expected.rax,value);native_expected.rip=bounded_cases[current_entry].target;++native_flags_cases;
        }
        CPU expected=c;calls=0;memset(trace,0,sizeof(trace));baselines[current_entry](&expected);
        unsigned expected_calls=calls;boundary expected_trace[4];memcpy(expected_trace,trace,sizeof(trace));unsigned char expected_stack[512];memcpy(expected_stack,stack,512);
        memcpy(stack,saved_stack,512);calls=0;memset(trace,0,sizeof(trace));void(*fn)(CPU*)=fp_extra_aot_lookup(c.rip);assert(fn);fn(&c);
        assert(calls==expected_calls&&!memcmp(trace,expected_trace,sizeof(trace))&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(stack,expected_stack,512));
        assert(c.rsp==initial_rsp&&rd64(initial_rsp)==0);
        if(current_entry<4)assert(!memcmp(&c,&native_expected,sizeof(c)));else{assert(calls==2);++call_cases;}
        ++tested;boundaries+=calls;
    }
    for(unsigned i=0;i<9;++i)for(unsigned byte=0;byte<bounded_cases[i].size;++byte) {
        unsigned char *at=(unsigned char *)(uintptr_t)bounded_cases[i].va+byte;*at^=1;
        assert(!fp_extra_aot_init(&image)&&!fp_extra_aot_lookup(bounded_cases[i].va));*at^=1;assert(fp_extra_aot_init(&image));++mutations;
    }
    printf("{\"success\":true,\"entries\":9,\"canonical_full_CPU_stack_cases\":%u,\"native_integer_flags_cases\":%u,\"five_argument_call_cases\":%u,\"exact_dispatch_boundaries\":%u,\"original_byte_mutations_rejected\":%u,\"stack_pop_added\":false,\"original_bytes_executed\":false}\n",tested,native_flags_cases,call_cases,boundaries,mutations);
    VirtualFree(image.base,0,MEM_RELEASE);return 0;
}
