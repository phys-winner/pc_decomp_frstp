#include <windows.h>
#include <stdio.h>
#include <stddef.h>
#include <smmintrin.h>
#include "simd_integer.h"

enum { PMULLD, PMOVZXBD, PABSD, PSADBW, PMOVSXDQ };
typedef struct {
    uint64_t va;
    const char *instruction;
    unsigned char bytes[6];
    unsigned size,operation,destination,source;
} test_site;
static const test_site cases[]={
#include "simd_integer_cases.inc"
};

static unsigned tested,gates,opcode_mutations,guarded_reads;
static uint64_t random_state=UINT64_C(0x96d8fba17423e05c);
static uint64_t random64(void) {
    random_state^=random_state<<13;random_state^=random_state>>7;
    random_state^=random_state<<17;return random_state;
}
static void require(int condition,const char *what) {
    if(!condition) {fprintf(stderr,"SIMD_INTEGER_FAIL %s\n",what);exit(1);}
}
#if defined(__clang__)
__attribute__((target("sse4.1")))
#endif
static XMM native_oracle(unsigned operation,XMM destination,XMM source) {
    __m128i d,s,result;memcpy(&d,&destination,16);memcpy(&s,&source,16);
    switch(operation) {
    case PMULLD:result=_mm_mullo_epi32(d,s);break;
    case PMOVZXBD:result=_mm_cvtepu8_epi32(s);break;
    case PABSD:result=_mm_abs_epi32(s);break;
    case PMOVSXDQ:result=_mm_cvtepi32_epi64(s);break;
    default:result=_mm_sad_epu8(d,s);break;
    }
    XMM answer;memcpy(&answer,&result,16);return answer;
}
static CPU random_cpu(void) {
    CPU cpu;unsigned char *p=(unsigned char *)&cpu;
    for(size_t i=0;i<sizeof(cpu);++i)p[i]=(unsigned char)random64();
    return cpu;
}
static void reject(CPU *cpu,const CPU *before,const fp_image *image,
                   uint64_t va,const char *instruction) {
    require(!fp_simd_integer(cpu,image,va,instruction),"rejection gate accepted");
    if(cpu)require(!memcmp(cpu,before,sizeof(*cpu)),"rejection changed CPU");
    ++gates;
}
static void check_site(const test_site *site) {
    unsigned char *pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    require(pages!=NULL,"opcode guard allocation");DWORD old;
    require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old),"opcode guard protection");
    unsigned char *opcode=pages+4096-site->size;
    memcpy(opcode,site->bytes,site->size);
    fp_section section={0,site->size,IMAGE_SCN_MEM_EXECUTE};
    fp_image image={0};image.base=opcode;image.size=site->size;
    image.preferred_base=site->va;image.sections=&section;image.section_count=1;
    const uint32_t edge[]={0,1,2,UINT32_MAX,0x80000000,0x7fffffff,0xffff0000,0x0000ffff};
    for(unsigned n=0;n<4160;++n) {
        CPU cpu=random_cpu();
        if(n<64) {
            for(unsigned lane=0;lane<4;++lane) {
                cpu.xmm[site->destination].u32[lane]=edge[(n/8+lane)%8];
                cpu.xmm[site->source].u32[lane]=edge[(n%8+lane)%8];
            }
        }
        if(site->operation==PSADBW && n<4) {
            memset(&cpu.xmm[site->destination],n==1?0xff:0,16);
            memset(&cpu.xmm[site->source],n==0?0xff:0,16);
        }
        CPU expected=cpu;
        expected.xmm[site->destination]=native_oracle(site->operation,
            cpu.xmm[site->destination],cpu.xmm[site->source]);
        unsigned mxcsr=_mm_getcsr();
        require(fp_simd_integer(&cpu,&image,site->va,site->instruction),"valid instruction refused");
        require(!memcmp(&cpu,&expected,sizeof(cpu)),"native oracle/full CPU/flags mismatch");
        require(_mm_getcsr()==mxcsr,"native MXCSR changed");++tested;
    }
    CPU cpu=random_cpu(),before=cpu;
    reject(NULL,&before,&image,site->va,site->instruction);
    reject(&cpu,&before,NULL,site->va,site->instruction);
    reject(&cpu,&before,&image,site->va,NULL);
    reject(&cpu,&before,&image,site->va,"different instruction");
    reject(&cpu,&before,&image,site->va+1,site->instruction);
    for(unsigned i=0;i<site->size;++i) {
        opcode[i]^=0x80;reject(&cpu,&before,&image,site->va,site->instruction);
        opcode[i]^=0x80;++opcode_mutations;
    }
    section.characteristics=0;reject(&cpu,&before,&image,site->va,site->instruction);
    section.characteristics=IMAGE_SCN_MEM_EXECUTE;
    --section.size;reject(&cpu,&before,&image,site->va,site->instruction);++section.size;
    --image.size;reject(&cpu,&before,&image,site->va,site->instruction);++image.size;
    ++image.preferred_base;reject(&cpu,&before,&image,site->va,site->instruction);
    --image.preferred_base;
    VirtualFree(pages,0,MEM_RELEASE);
}
static void pmov_four_byte_guard(void) {
    const test_site *site=NULL;
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)
        if(cases[i].operation==PMOVZXBD && cases[i].source==1 && cases[i].destination==0) {site=&cases[i];break;}
    require(site!=NULL,"four-byte PMOV fixture site missing");
    unsigned char *pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    require(pages!=NULL,"source guard allocation");DWORD old;
    require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old),"source guard protection");
    CPU *cpu=(CPU *)(pages+4096-4-(offsetof(CPU,xmm)+sizeof(XMM)));
    fp_section section={0,site->size,IMAGE_SCN_MEM_EXECUTE};
    fp_image image={0};image.base=(void *)site->bytes;image.size=site->size;
    image.preferred_base=site->va;image.sections=&section;image.section_count=1;
    for(unsigned n=0;n<256;++n) {
        uint8_t bytes[4]={(uint8_t)n,0,0xff,(uint8_t)(255-n)};
        memcpy(&cpu->xmm[1],bytes,4);
        require(fp_simd_integer(cpu,&image,site->va,site->instruction),"four-byte PMOV refused");
        for(unsigned lane=0;lane<4;++lane)
            require(cpu->xmm[0].u32[lane]==bytes[lane],"four-byte PMOV result");
        ++guarded_reads;
    }
    VirtualFree(pages,0,MEM_RELEASE);
}
int main(void) {
    int features[4];__cpuidex(features,1,0);
    require((features[2]&(1u<<19)) && (features[2]&(1u<<9)),"native oracle needs SSE4.1/SSSE3");
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)check_site(&cases[i]);
    pmov_four_byte_guard();
    printf("{\"success\":true,\"sites\":%u,\"native_full_CPU_cases\":%u,\"negative_gates\":%u,\"opcode_mutations\":%u,\"four_byte_guard_cases\":%u,\"original_bytes_executed\":false}\n",
        (unsigned)(sizeof(cases)/sizeof(*cases)),tested,gates,opcode_mutations,guarded_reads);
    return 0;
}
