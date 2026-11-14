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
static uint64_t captured;static CPU boundary;static unsigned calls,return_mode;
void dispatch(CPU *c,uint64_t target) {
    captured=target;boundary=*c;++calls;
    if(return_mode){c->rax=UINT64_C(0x9876543210abcdef);c->rip=pop64(c);}
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
static void commit(const fp_image *image,const test_case *entry) {
    uint64_t address=(uintptr_t)image->base+entry->va-image->preferred_base;
    uint64_t low=address&~UINT64_C(4095),high=(address+entry->size+4095)&~UINT64_C(4095);
    assert(VirtualAlloc((void *)(uintptr_t)low,(SIZE_T)(high-low),MEM_COMMIT,PAGE_READWRITE));
    memcpy((void *)(uintptr_t)address,entry->bytes,entry->size);
}
int main(void) {
    unsigned tested=0,returned=0,mutations=0,guarded=0;uint64_t random=UINT64_C(0x6aca94eec01dbeef);
    unsigned char *object_pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    unsigned char *table_pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);DWORD old;
    assert(object_pages&&table_pages&&VirtualProtect(object_pages+4096,4096,PAGE_NOACCESS,&old)&&VirtualProtect(table_pages+4096,4096,PAGE_NOACCESS,&old));
    uint64_t object=(uintptr_t)object_pages+4096-8,table=(uintptr_t)table_pages+4096-0x110;
    wr64(object,table);
    for(unsigned relocation=0;relocation<2;++relocation) {
        fp_image image={0};image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
        g_image_delta=(int64_t)relocation*INT64_C(0x10000000);
        image.base=VirtualAlloc((void *)(uintptr_t)GVA(image.preferred_base),image.size,MEM_RESERVE,PAGE_NOACCESS);
        assert((uintptr_t)image.base==GVA(image.preferred_base));
        fp_section section={0,(uint32_t)image.size,IMAGE_SCN_MEM_EXECUTE};image.sections=&section;image.section_count=1;
        for(unsigned i=0;i<12;++i)commit(&image,&cases[i]);
        for(unsigned i=0;i<2;++i)commit(&image,&switch_cases[i]);
        for(unsigned i=0;i<5;++i)commit(&image,&switch_extension_cases[i]);
        commit(&image,&callback_cases[0]);
        for(unsigned i=0;i<4;++i)commit(&image,&scene_cases[i]);
        for(unsigned i=0;i<2;++i)commit(&image,&compare_cases[i]);
        for(unsigned i=0;i<9;++i)commit(&image,&bounded_cases[i]);
        assert(fp_extra_aot_init(&image));
        assert(!fp_extra_aot_lookup(callback_cases[0].va+1));
        void(*fn)(CPU*)=fp_extra_aot_lookup(callback_cases[0].va);assert(fn);
        for(unsigned trial=0;trial<8192;++trial) {
            CPU c;unsigned char *p=(unsigned char *)&c;
            for(size_t i=0;i<sizeof(c);++i){random^=random<<13;random^=random>>7;random^=random<<17;p[i]=(unsigned char)random;}
            c.cf=trial&1;c.pf=(trial>>1)&1;c.af=(trial>>2)&1;c.zf=(trial>>3)&1;c.sf=(trial>>4)&1;c.of=(trial>>5)&1;
            c.mxcsr=0x1f80|(trial&0x3f);c.rcx=object;
            uint64_t stack[4]={UINT64_C(0xe5a0123456789abc),UINT64_C(0x1111222233334444),0,0};c.rsp=(uintptr_t)stack;
            uint64_t target=trial&1?UINT64_C(0x141802cf0):UINT64_C(0x7ff8deadbeef1234);wr64(table+0x108,target);
            CPU expected=c;expected.rip=callback_cases[0].va+3;expected.rax=table;
            CPU at_tail=expected;return_mode=(trial>>1)&1;
            if(return_mode){expected.rax=UINT64_C(0x9876543210abcdef);expected.rip=stack[0];expected.rsp+=8;++returned;}
            calls=0;captured=0;memset(&boundary,0,sizeof(boundary));fn(&c);
            if(calls!=1||captured!=target||memcmp(&boundary,&at_tail,sizeof(c))||memcmp(&c,&expected,sizeof(c))) {
                fprintf(stderr,"trial=%u relocation=%u calls=%u captured=%llx target=%llx\n",trial,relocation,calls,captured,target);
                const uint64_t *a=(const uint64_t *)&boundary,*b=(const uint64_t *)&at_tail;
                for(unsigned word=0;word<sizeof(c)/8;++word)if(a[word]!=b[word])fprintf(stderr,"boundary word%u got%llx expected%llx\n",word,a[word],b[word]);
                a=(const uint64_t *)&c;b=(const uint64_t *)&expected;
                for(unsigned word=0;word<sizeof(c)/8;++word)if(a[word]!=b[word])fprintf(stderr,"return word%u got%llx expected%llx\n",word,a[word],b[word]);
            }
            assert(calls==1&&captured==target&&!memcmp(&boundary,&at_tail,sizeof(c))&&!memcmp(&c,&expected,sizeof(c)));
            assert(stack[0]==UINT64_C(0xe5a0123456789abc)&&stack[1]==UINT64_C(0x1111222233334444));
            assert(rd64(object)==table&&rd64(table+0x108)==target);++tested;++guarded;
        }
        for(unsigned byte=0;byte<callback_cases[0].size;++byte) {
            unsigned char *at=(unsigned char *)(uintptr_t)GVA(callback_cases[0].va)+byte;*at^=1;
            assert(!fp_extra_aot_init(&image)&&!fp_extra_aot_lookup(callback_cases[0].va));*at^=1;assert(fp_extra_aot_init(&image));++mutations;
        }
        section.characteristics=0;assert(!fp_extra_aot_init(&image));section.characteristics=IMAGE_SCN_MEM_EXECUTE;
        assert(fp_extra_aot_init(&image));assert(VirtualFree(image.base,0,MEM_RELEASE));
    }
    VirtualFree(object_pages,0,MEM_RELEASE);VirtualFree(table_pages,0,MEM_RELEASE);
    printf("{\"success\":true,\"entries\":1,\"full_CPU_dynamic_tail_cases\":%u,\"single_RET_paths\":%u,\"guarded_8B_reads\":%u,\"original_byte_mutations_rejected\":%u,\"relocation_deltas\":2,\"original_bytes_executed\":false}\n",tested,returned,guarded,mutations);
    return 0;
}
