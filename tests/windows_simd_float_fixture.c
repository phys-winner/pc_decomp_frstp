#pragma STDC FENV_ACCESS ON
#include <windows.h>
#include <stdio.h>
#include <pmmintrin.h>
#include "simd_float.h"

enum { CVTTPD2DQ, HADDPS };
typedef struct {
    uint64_t va;
    const char *instruction;
    unsigned char bytes[4];
    unsigned operation,destination,source;
} test_site;
static const test_site cases[]={
#include "simd_float_cases.inc"
};
uint32_t fp_simd_float_mxcsr_mask_native(void);
static unsigned tested,gates,opcode_mutations,new_exceptions;
static uint64_t random_state=UINT64_C(0xc75b29e0184af36d);
static uint64_t random64(void) {
    random_state^=random_state<<13;random_state^=random_state>>7;
    random_state^=random_state<<17;return random_state;
}
static void require(int condition,const char *what) {
    if(!condition){fprintf(stderr,"SIMD_FLOAT_FAIL %s\n",what);exit(1);}
}
/* A separate compiler/intrinsic oracle; its exact MXCSR ordering is checked
   in the linked machine code by test_windows_simd_float.py. */
__declspec(noinline) uint32_t fp_float_oracle_cvt(XMM *out,const XMM *source,uint32_t mxcsr) {
    __m128d s;memcpy(&s,source,16);uint32_t saved=_mm_getcsr();
    _mm_setcsr(mxcsr);
    __m128i result=_mm_cvttpd_epi32(s);memcpy(out,&result,16);
    uint32_t after=_mm_getcsr();_mm_setcsr(saved);return after;
}
#if defined(__clang__)
__attribute__((target("sse3")))
#endif
__declspec(noinline) uint32_t fp_float_oracle_hadd(XMM *out,const XMM *destination,const XMM *source,uint32_t mxcsr) {
    __m128 d,s;memcpy(&d,destination,16);memcpy(&s,source,16);
    uint32_t saved=_mm_getcsr();_mm_setcsr(mxcsr);
    __m128 result=_mm_hadd_ps(d,s);memcpy(out,&result,16);
    uint32_t after=_mm_getcsr();_mm_setcsr(saved);return after;
}
static CPU random_cpu(void) {
    CPU cpu;unsigned char *p=(unsigned char *)&cpu;
    for(size_t i=0;i<sizeof(cpu);++i)p[i]=(unsigned char)random64();return cpu;
}
static void reject(CPU *cpu,const CPU *before,const fp_image *image,
                   uint64_t va,const char *instruction) {
    unsigned saved=_mm_getcsr();
    require(!fp_simd_float(cpu,image,va,instruction),"rejection gate accepted");
    if(cpu)require(!memcmp(cpu,before,sizeof(*cpu)),"rejection changed CPU");
    require(_mm_getcsr()==saved,"rejection changed native MXCSR");++gates;
}
static void differential(const test_site *site,const fp_image *image,CPU cpu,unsigned host_mxcsr) {
    CPU expected=cpu;_mm_setcsr(host_mxcsr);
    if(site->operation==CVTTPD2DQ)
        expected.mxcsr=fp_float_oracle_cvt(&expected.xmm[site->destination],&cpu.xmm[site->source],cpu.mxcsr);
    else expected.mxcsr=fp_float_oracle_hadd(&expected.xmm[site->destination],&cpu.xmm[site->destination],&cpu.xmm[site->source],cpu.mxcsr);
    require(_mm_getcsr()==host_mxcsr,"oracle changed native MXCSR");
    if(!(cpu.mxcsr&0x3f))new_exceptions|=expected.mxcsr&0x3f;
    require(fp_simd_float(&cpu,image,site->va,site->instruction),"valid instruction refused");
    require(_mm_getcsr()==host_mxcsr,"helper changed native MXCSR/control/status");
    if(memcmp(&cpu,&expected,sizeof(cpu))) {
        fprintf(stderr,"MISMATCH site=%llx mxcsr=%08x/%08x xmm=%016llx:%016llx/%016llx:%016llx\n",site->va,cpu.mxcsr,expected.mxcsr,
            cpu.xmm[site->destination].u64[1],cpu.xmm[site->destination].u64[0],
            expected.xmm[site->destination].u64[1],expected.xmm[site->destination].u64[0]);
        require(0,"native oracle/full CPU/flags mismatch");
    }
    ++tested;
}
static void check_site(const test_site *site,uint32_t valid_mask) {
    unsigned char *pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);DWORD old;
    require(pages!=NULL,"opcode guard allocation");
    require(VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old),"opcode guard protection");
    unsigned char *opcode=pages+4092;memcpy(opcode,site->bytes,4);
    fp_section section={0,4,IMAGE_SCN_MEM_EXECUTE};
    fp_image image={0};image.base=opcode;image.size=4;image.preferred_base=site->va;
    image.sections=&section;image.section_count=1;
    const uint64_t doubles[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),
        UINT64_C(0x3ff8000000000000),UINT64_C(0xbff8000000000000),UINT64_C(0x41dfffffffc00000),UINT64_C(0x41e0000000000000),
        UINT64_C(0xc1e0000000000000),UINT64_C(0xc1e0000000200000),UINT64_C(0xc1e0000000180000),1,
        UINT64_C(0x8000000000000001),UINT64_C(0x000fffffffffffff),UINT64_C(0x0010000000000000),
        UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000000123),
        UINT64_C(0x7ff0000000000001),UINT64_C(0xfff0000000000456),UINT64_C(0x7fefffffffffffff)};
    const uint32_t singles[]={0,0x80000000,0x3f800000,0xbf800000,0x3f000000,0x33800000,0xb3800000,
        0x7f7fffff,0xff7fffff,0x00800000,0x80800000,1,0x80000001,0x007fffff,0x807fffff,
        0x7f800000,0xff800000,0x7fc00123,0xffc00456,0x7f800001,0xff800123};
    for(unsigned mode=0;mode<32;++mode) {
        unsigned extra=valid_mask&~UINT32_C(0xffff);
        unsigned guest=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0)|((mode&16)?0x3f|extra:0);
        if(guest&~valid_mask)continue;
        unsigned host=0x1f80|(((mode+1)&3)<<13)|((mode&1)?0x35:0x0a)|
            ((mode&2)?valid_mask&0x40:0)|((mode&4)?valid_mask&0x8000:0)|((mode&1)?extra:0);
        for(unsigned i=0;i<21;++i)for(unsigned j=0;j<21;++j) {
            CPU cpu=random_cpu();cpu.mxcsr=guest;
            if(site->operation==CVTTPD2DQ) {cpu.xmm[site->source].u64[0]=doubles[i];cpu.xmm[site->source].u64[1]=doubles[j];}
            else {
                cpu.xmm[site->destination].u32[0]=singles[i];cpu.xmm[site->destination].u32[1]=singles[j];
                cpu.xmm[site->destination].u32[2]=singles[20-i];cpu.xmm[site->destination].u32[3]=singles[20-j];
                cpu.xmm[site->source].u32[0]=singles[j];cpu.xmm[site->source].u32[1]=singles[20-i];
                cpu.xmm[site->source].u32[2]=singles[i];cpu.xmm[site->source].u32[3]=singles[20-j];
            }
            differential(site,&image,cpu,host);
        }
    }
    for(unsigned n=0;n<1024;++n) {
        CPU cpu=random_cpu();cpu.mxcsr=0x1f80|((unsigned)random64()&valid_mask&~UINT32_C(0x1f80));
        differential(site,&image,cpu,0x5fb5);
    }
    const unsigned host_environments[]={0,0x3f,valid_mask&~UINT32_C(0x1f80)};
    for(unsigned i=0;i<sizeof(host_environments)/sizeof(*host_environments);++i) {
        CPU cpu=random_cpu();cpu.mxcsr=0x1f80;
        differential(site,&image,cpu,host_environments[i]);
    }
    CPU cpu=random_cpu();cpu.mxcsr=0x1f80;CPU before=cpu;
    reject(NULL,&before,&image,site->va,site->instruction);
    reject(&cpu,&before,NULL,site->va,site->instruction);
    reject(&cpu,&before,&image,site->va,NULL);
    reject(&cpu,&before,&image,site->va,"different instruction");
    reject(&cpu,&before,&image,site->va+1,site->instruction);
    for(unsigned i=0;i<4;++i) {
        opcode[i]^=0x80;reject(&cpu,&before,&image,site->va,site->instruction);opcode[i]^=0x80;++opcode_mutations;
    }
    for(unsigned i=0;i<6;++i) {
        cpu=before;cpu.mxcsr&=~(1u<<(7+i));CPU saved=cpu;reject(&cpu,&saved,&image,site->va,site->instruction);
    }
    cpu=before;cpu.mxcsr|=0x80000000;CPU invalid=cpu;reject(&cpu,&invalid,&image,site->va,site->instruction);
    cpu=before;section.characteristics=0;reject(&cpu,&before,&image,site->va,site->instruction);
    section.characteristics=IMAGE_SCN_MEM_EXECUTE;
    section.size=3;reject(&cpu,&before,&image,site->va,site->instruction);section.size=4;
    image.size=3;reject(&cpu,&before,&image,site->va,site->instruction);
    VirtualFree(pages,0,MEM_RELEASE);
}
int main(void) {
    uint32_t initial=_mm_getcsr();CPU cpu={0};cpu.mxcsr=0x1f80;CPU before=cpu;
    fp_section section={0,4,IMAGE_SCN_MEM_EXECUTE};
    fp_image image={0};image.base=(void *)cases[0].bytes;image.size=4;image.preferred_base=cases[0].va;
    image.sections=&section;image.section_count=1;
    reject(&cpu,&before,&image,cases[0].va,cases[0].instruction);
    require(fp_simd_float_init(),"native SSE2 missing");
    int features[4];__cpuidex(features,1,0);require(features[2]&1,"oracle needs native SSE3");
    uint32_t valid_mask=fp_simd_float_mxcsr_mask_native();
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)check_site(&cases[i],valid_mask);
    require((new_exceptions&0x3b)==0x3b,"missing newly raised invalid/denormal/overflow/underflow/precision coverage");
    _mm_setcsr(initial);
    printf("{\"success\":true,\"sites\":%u,\"native_full_CPU_cases\":%u,\"negative_gates\":%u,\"opcode_mutations\":%u,\"new_exception_bits\":%u,\"mxcsr_mask\":%u,\"original_bytes_executed\":false}\n",
        (unsigned)(sizeof(cases)/sizeof(*cases)),tested,gates,opcode_mutations,new_exceptions,valid_mask);
    return 0;
}
