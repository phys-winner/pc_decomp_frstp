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
static uint64_t tail;static unsigned calls;
void dispatch(CPU *c,uint64_t target){(void)c;tail=target;++calls;}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
extern uint64_t fp_test_add24(uint64_t,uint64_t *);
static int32_t displacement(const char *p){int32_t d;memcpy(&d,p,4);return d;}

int main(void) {
    unsigned tested=0,add=0,memory=0;
    const uint64_t edge[]={0,1,23,24,25,UINT64_MAX,UINT64_MAX-23,UINT64_MAX-24,
        UINT64_C(0x7fffffffffffffff),UINT64_C(0x7fffffffffffffe7),UINT64_C(0x7fffffffffffffe8),UINT64_C(0x8000000000000000)};
    for(unsigned relocation=0;relocation<2;++relocation) {
        fp_image image={0};fp_section sections[35];image.preferred_base=UINT64_C(0x140000000);image.size=0x4539000;
        g_image_delta=(int64_t)relocation*INT64_C(0x10000000);
        image.base=VirtualAlloc((void *)(uintptr_t)GVA(image.preferred_base),image.size,MEM_RESERVE,PAGE_NOACCESS);
        assert((uintptr_t)image.base==GVA(image.preferred_base));image.sections=sections;image.section_count=35;
        for(unsigned i=0;i<12;++i) {
            uint32_t page=(uint32_t)(cases[i].va-image.preferred_base)&~4095u;
            assert(VirtualAlloc((char *)image.base+page,4096,MEM_COMMIT,PAGE_READWRITE));
            sections[i]=(fp_section){page,4096,IMAGE_SCN_MEM_EXECUTE};
            memcpy((void *)(uintptr_t)GVA(cases[i].va),cases[i].bytes,cases[i].size);
        }
        for(unsigned i=0;i<7;++i) {
            const test_case *entry=i<2?&switch_cases[i]:&switch_extension_cases[i-2];
            uint32_t page=(uint32_t)(entry->va-image.preferred_base)&~4095u;
            assert(VirtualAlloc((char *)image.base+page,8192,MEM_COMMIT,PAGE_READWRITE));
            sections[12+i]=(fp_section){page,8192,IMAGE_SCN_MEM_EXECUTE};
            memcpy((void *)(uintptr_t)GVA(entry->va),entry->bytes,entry->size);
        }
        uint32_t callback_page=(uint32_t)(callback_cases[0].va-image.preferred_base)&~4095u;
        assert(VirtualAlloc((char *)image.base+callback_page,4096,MEM_COMMIT,PAGE_READWRITE));
        sections[19]=(fp_section){callback_page,4096,IMAGE_SCN_MEM_EXECUTE};
        memcpy((void *)(uintptr_t)GVA(callback_cases[0].va),callback_cases[0].bytes,callback_cases[0].size);
        for(unsigned i=0;i<4;++i) {
            uint32_t page=(uint32_t)(scene_cases[i].va-image.preferred_base)&~4095u;
            assert(VirtualAlloc((char *)image.base+page,8192,MEM_COMMIT,PAGE_READWRITE));
            sections[20+i]=(fp_section){page,8192,IMAGE_SCN_MEM_EXECUTE};
            memcpy((void *)(uintptr_t)GVA(scene_cases[i].va),scene_cases[i].bytes,scene_cases[i].size);
        }
        for(unsigned i=0;i<2;++i) {
            uint32_t page=(uint32_t)(compare_cases[i].va-image.preferred_base)&~4095u;
            assert(VirtualAlloc((char *)image.base+page,4096,MEM_COMMIT,PAGE_READWRITE));
            sections[24+i]=(fp_section){page,4096,IMAGE_SCN_MEM_EXECUTE};
            memcpy((void *)(uintptr_t)GVA(compare_cases[i].va),compare_cases[i].bytes,compare_cases[i].size);
        }
        for(unsigned i=0;i<9;++i) {
            uint32_t page=(uint32_t)(bounded_cases[i].va-image.preferred_base)&~4095u;
            assert(VirtualAlloc((char *)image.base+page,4096,MEM_COMMIT,PAGE_READWRITE));
            sections[26+i]=(fp_section){page,4096,IMAGE_SCN_MEM_EXECUTE};
            memcpy((void *)(uintptr_t)GVA(bounded_cases[i].va),bounded_cases[i].bytes,bounded_cases[i].size);
        }
        assert(VirtualAlloc((void *)(uintptr_t)GVA(UINT64_C(0x143ffa000)),4096,MEM_COMMIT,PAGE_READWRITE));
        assert(fp_extra_aot_init(&image));
        for(unsigned entry=0;entry<12;++entry) {
            const test_case *t=&cases[entry];void(*fn)(CPU*)=fp_extra_aot_lookup(t->va);assert(fn);
            uint64_t random=UINT64_C(0x917acfeb12345678);
            for(unsigned i=0;i<4108;++i) {
                random^=random<<13;random^=random>>7;random^=random<<17;
                uint64_t value=i<sizeof(edge)/sizeof(*edge)?edge[i]:random,flags;
                CPU c,expected;memset(&c,0xa5,sizeof(c));c.rcx=value;expected=c;expected.rip=t->va;
                unsigned char object[256],object_expected[256];memset(object,0xa5,sizeof(object));memcpy(object_expected,object,sizeof(object));
                uint64_t wrapper=UINT64_C(0xfedcba9876543210);
                if(t->va==UINT64_C(0x140fee310)) {
                    expected.rcx=fp_test_add24(value,&flags);expected.cf=flags&1;expected.pf=(flags>>2)&1;
                    expected.af=(flags>>4)&1;expected.zf=(flags>>6)&1;expected.sf=(flags>>7)&1;expected.of=(flags>>11)&1;++add;
                } else if(t->va==UINT64_C(0x141a868e0)) {
                    c.rcx=(uintptr_t)object;expected.rcx=wrapper;expected.r8=0;expected.rdx=0x44;
                    expected.cf=expected.of=expected.sf=0;expected.zf=expected.pf=1;
                    wr64(GVA(UINT64_C(0x143ffa818)),(uintptr_t)&wrapper);object_expected[0xe0]=1;++memory;
                } else if(t->va==UINT64_C(0x141cac080)) {
                    expected.rdx=8;expected.r8=2;
                    expected.r9=GVA(t->va+12+displacement(t->bytes+8));
                    expected.rcx=GVA(t->va+19+displacement(t->bytes+15));
                } else if(t->size==12) {
                    expected.rcx=GVA(t->va+7+displacement(t->bytes+3));
                } else assert(t->size==5 && (unsigned char)t->bytes[0]==0xe9);
                calls=0;tail=0;fn(&c);
                if(calls!=1 || tail!=t->target || memcmp(&c,&expected,sizeof(c))) {
                    fprintf(stderr,"mismatch va=0x%llx iteration=%u relocation=%u calls=%u tail=0x%llx expected=0x%llx\n",t->va,i,relocation,calls,tail,t->target);
                    uint64_t *a=(uint64_t *)&c,*b=(uint64_t *)&expected;
                    for(unsigned k=0;k<sizeof(c)/8;++k)if(a[k]!=b[k])fprintf(stderr,"word %u got=%llx expected=%llx\n",k,a[k],b[k]);
                }
                assert(calls==1 && tail==t->target && !memcmp(&c,&expected,sizeof(c)));
                assert(!memcmp(object,object_expected,sizeof(object)));++tested;
            }
            assert(!fp_extra_aot_lookup(t->va+1));
        }
        for(unsigned i=0;i<12;++i) {
            unsigned char *last=(unsigned char *)(uintptr_t)GVA(cases[i].va)+cases[i].size-1;*last^=1;
            assert(!fp_extra_aot_init(&image) && !fp_extra_aot_lookup(cases[i].va));*last^=1;
            assert(fp_extra_aot_init(&image));
        }
        image.preferred_base+=1;assert(!fp_extra_aot_init(&image));
        assert(VirtualFree(image.base,0,MEM_RELEASE));
    }
    printf("{\"success\":true,\"entries\":12,\"full_CPU_tail_cases\":%u,\"native_ADD_flags_cases\":%u,\"memory_XOR_cases\":%u,\"original_byte_gates\":24,\"data_relocation_deltas\":2,\"original_bytes_executed\":false}\n",tested,add,memory);
    return 0;
}
