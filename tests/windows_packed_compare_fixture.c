#pragma STDC FENV_ACCESS ON
#include <windows.h>
#include <stdio.h>
#include <emmintrin.h>
#include "simd_float.h"

typedef struct {
    uint64_t va;
    unsigned destination,source,operation,memory;
    int source_gpr;
    uint64_t source_disp,source_va;
    void (*repaired)(CPU *);
    void (*legacy)(CPU *);
} compare_site;
void frostpunk_unsupported(uint64_t va,const char *text) {
    fprintf(stderr,"PACKED_COMPARE_UNEXPECTED_STOP %llx %s\n",va,text);exit(1);
}
#include "packed_compare_overlay_cases.inc"

uint32_t fp_simd_float_mxcsr_mask_native(void);
int64_t g_image_delta;
static unsigned common_compare_cases;
static unsigned tested,negative_gates,legacy_wrong_lanes,legacy_wrong_sites,guarded_reads,memory_cases,mm_cases;
static uint64_t random_state=UINT64_C(0x41f782bae3c9506d);
static uint64_t random64(void) {
    random_state^=random_state<<13;random_state^=random_state>>7;
    random_state^=random_state<<17;return random_state;
}
static CPU random_cpu(void) {
    CPU cpu;unsigned char *p=(unsigned char *)&cpu;
    for(size_t i=0;i<sizeof(cpu);++i)p[i]=(unsigned char)random64();return cpu;
}
static void require(int condition,const char *text) {
    if(!condition){fprintf(stderr,"PACKED_COMPARE_FAIL %s\n",text);exit(1);}
}
#define ORACLE(name,type,intrinsic) \
__declspec(noinline) uint32_t name(XMM *out,const XMM *destination,const XMM *source,uint32_t mxcsr) { \
    type d,s;memcpy(&d,destination,16);memcpy(&s,source,16); \
    uint32_t saved=_mm_getcsr();_mm_setcsr(mxcsr); \
    type result=intrinsic(d,s);memcpy(out,&result,16); \
    uint32_t after=_mm_getcsr();_mm_setcsr(saved);return after; \
}
ORACLE(fp_compare_oracle_eqps,__m128,_mm_cmpeq_ps)
ORACLE(fp_compare_oracle_ltps,__m128,_mm_cmplt_ps)
ORACLE(fp_compare_oracle_eqpd,__m128d,_mm_cmpeq_pd)
ORACLE(fp_compare_oracle_ltpd,__m128d,_mm_cmplt_pd)
ORACLE(fp_compare_oracle_nlepd,__m128d,_mm_cmpnle_pd)
static uint32_t oracle(XMM *out,const XMM *d,const XMM *s,unsigned mxcsr,unsigned operation) {
    switch(operation) {
    case FP_CMPPS_EQ:return fp_compare_oracle_eqps(out,d,s,mxcsr);
    case FP_CMPPS_LT:return fp_compare_oracle_ltps(out,d,s,mxcsr);
    case FP_CMPPD_EQ:return fp_compare_oracle_eqpd(out,d,s,mxcsr);
    case FP_CMPPD_LT:return fp_compare_oracle_ltpd(out,d,s,mxcsr);
    default:return fp_compare_oracle_nlepd(out,d,s,mxcsr);
    }
}
static void differential_source(CPU cpu,unsigned d,const XMM *source,unsigned operation,void(*repaired)(CPU *),unsigned host_mxcsr,int source_register) {
    if(source_register>=0)source=&cpu.xmm[source_register];
    CPU expected=cpu;_mm_setcsr(host_mxcsr);
    expected.mxcsr=oracle(&expected.xmm[d],&cpu.xmm[d],source,cpu.mxcsr,operation);
    require(_mm_getcsr()==host_mxcsr,"oracle native MXCSR restoration");
    if(repaired)repaired(&cpu);
    else require(fp_simd_packed_compare(&cpu,d,source,operation),"generic helper refused");
    require(_mm_getcsr()==host_mxcsr,"helper native MXCSR/status restoration");
    require(!memcmp(&cpu,&expected,sizeof(cpu)),"native oracle/full CPU/guest flags/MXCSR mismatch");++tested;
}
static void differential(CPU cpu,unsigned d,unsigned s,unsigned operation,void(*repaired)(CPU *),unsigned host) {
    differential_source(cpu,d,&cpu.xmm[s],operation,repaired,host,(int)s);
}
static void prepare_memory(CPU *cpu,const compare_site *site,const XMM *source) {
    if(!site->memory)return;
    if(site->source_gpr<0)g_image_delta=(int64_t)(uintptr_t)source-(int64_t)site->source_va;
    else ((uint64_t *)&cpu->rax)[site->source_gpr]=(uintptr_t)source-site->source_disp;
}
static const uint32_t edges[]={0,0x80000000,0x3f800000,0xbf800000,0x3f000000,0x7f7fffff,0xff7fffff,
    0x00800000,0x80800000,1,0x80000001,0x007fffff,0x807fffff,0x7f800000,0xff800000,0x7fc00123,0xffc00456,0x7f800001,0xff800123};

static const uint64_t double_edges[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),
    UINT64_C(0x3fe0000000000000),UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff),
    UINT64_C(0x0010000000000000),UINT64_C(0x8010000000000000),1,UINT64_C(0x8000000000000001),
    UINT64_C(0x000fffffffffffff),UINT64_C(0x800fffffffffffff),UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),
    UINT64_C(0x7ff8000000000123),UINT64_C(0xfff8000000000456),UINT64_C(0x7ff0000000000001),UINT64_C(0xfff0000000000123)};
static void check_overlay(const compare_site *site,uint32_t mask) {
    for(unsigned mode=0;mode<32;++mode) {
        unsigned guest=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0)|((mode&16)?0x3f:0);
        if(guest&~mask)continue;
        for(unsigned i=0;i<19;++i)for(unsigned j=0;j<19;++j) {
            CPU cpu=random_cpu();cpu.mxcsr=guest;_Alignas(16) XMM memory;
            XMM *source=site->memory?&memory:&cpu.xmm[site->source];
            if(site->operation<=FP_CMPPS_LT)for(unsigned lane=0;lane<4;++lane) {
                cpu.xmm[site->destination].u32[lane]=edges[(i+lane)%19];
                source->u32[lane]=edges[(j+3-lane)%19];
            } else for(unsigned lane=0;lane<2;++lane) {
                cpu.xmm[site->destination].u64[lane]=double_edges[(i+lane)%19];
                source->u64[lane]=double_edges[(j+1-lane)%19];
            }
            prepare_memory(&cpu,site,source);
            differential_source(cpu,site->destination,source,site->operation,site->repaired,0x5fb5,site->memory?-1:(int)site->source);
            if(site->memory)memory_cases++;
        }
    }
    CPU cpu={0};cpu.mxcsr=0x1f80;_Alignas(16) XMM memory;
    XMM *source=site->memory?&memory:&cpu.xmm[site->source];
    if(site->operation<=FP_CMPPS_LT) {
        cpu.xmm[site->destination].f32[0]=-1;cpu.xmm[site->destination].f32[1]=1;
        cpu.xmm[site->destination].f32[2]=-2;cpu.xmm[site->destination].f32[3]=2;
        source->f32[0]=0;source->f32[1]=0;source->f32[2]=-2;source->f32[3]=3;
    } else {
        cpu.xmm[site->destination].f64[0]=1;cpu.xmm[site->destination].f64[1]=-1;
        source->f64[0]=2;source->f64[1]=-1;
    }
    prepare_memory(&cpu,site,source);
    CPU expected=cpu;expected.mxcsr=oracle(&expected.xmm[site->destination],&cpu.xmm[site->destination],source,cpu.mxcsr,site->operation);
    site->legacy(&cpu);unsigned wrong=0;
    for(unsigned lane=0;lane<4;++lane)wrong+=cpu.xmm[site->destination].u32[lane]!=expected.xmm[site->destination].u32[lane];
    legacy_wrong_lanes+=wrong;
    /* Self-comparisons can coincidentally retain zero upper lanes; NaN/mixed
       vectors above still check every repaired result against real hardware. */
    legacy_wrong_sites+=wrong!=0;
}
static void check_common_compare(uint32_t mask) {
    const uint32_t safe[]={0,0x80000000,0x00800000,0x80800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000};
    const uint32_t exceptional[]={1,0x007fffff,0x80000001,0x807fffff,0x7fc00000,0xffc00000,0x7f800001,0xff800001};
    const unsigned hosts[]={0x1f80,0x1fbf,0x9fc0,0x5fb5,0,0x3f,0x1e80,0x1f00};
    for(unsigned operation=FP_CMPPS_EQ;operation<=FP_CMPPS_LT;++operation)for(unsigned mode=0;mode<32;++mode) {
        unsigned guest=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0)|((mode&16)?0x3f:0);
        if(guest&~mask)continue;
        for(unsigned special=0;special<=8;++special)for(unsigned lane=0;lane<4;++lane)for(unsigned side=0;side<2;++side) {
            CPU cpu=random_cpu();cpu.mxcsr=guest;
            for(unsigned i=0;i<4;++i) {cpu.xmm[0].u32[i]=safe[(mode+i)%8];cpu.xmm[1].u32[i]=safe[(mode+7-i)%8];}
            if(special)cpu.xmm[side].u32[lane]=exceptional[special-1];
            unsigned host=hosts[(mode+special+lane+side)%8]&mask;
            differential(cpu,0,1,operation,NULL,host);
            differential(cpu,0,0,operation,NULL,host);common_compare_cases+=2;
        }
    }
}
static void reject(unsigned destination,const XMM *source,unsigned operation,CPU cpu) {
    CPU before=cpu;unsigned host=_mm_getcsr();
    require(!fp_simd_packed_compare(&cpu,destination,source,operation),"negative gate accepted");
    require(!memcmp(&cpu,&before,sizeof(cpu)),"negative gate changed CPU");
    require(_mm_getcsr()==host,"negative gate changed native MXCSR");++negative_gates;
}
int main(void) {
    unsigned initial=_mm_getcsr();require(fp_simd_float_init(),"native SSE2 missing");
    uint32_t mask=fp_simd_float_mxcsr_mask_native();
    check_common_compare(mask);
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)check_overlay(&cases[i],mask);
    for(unsigned operation=0;operation<=FP_CMPPD_NLE;++operation)
        for(unsigned d=0;d<16;++d)for(unsigned s=0;s<16;++s)for(unsigned n=0;n<64;++n) {
            CPU cpu=random_cpu();cpu.mxcsr=0x1f80|((unsigned)random64()&mask&~UINT32_C(0x1f80));
            differential(cpu,d,s,operation,NULL,n&1?0x3f:0x5fb5);
        }
    CPU cpu=random_cpu();cpu.mxcsr=0x1f80;
    reject(16,&cpu.xmm[0],FP_CMPPS_LT,cpu);reject(0,NULL,FP_CMPPS_LT,cpu);
    reject(0,&cpu.xmm[0],FP_CMPPD_NLE+1,cpu);
    for(unsigned i=0;i<6;++i) {CPU unmasked=cpu;unmasked.mxcsr&=~(1u<<(7+i));reject(0,&cpu.xmm[0],FP_CMPPS_LT,unmasked);}
    cpu.mxcsr|=0x80000000;reject(0,&cpu.xmm[0],FP_CMPPS_LT,cpu);
    unsigned char *pages=VirtualAlloc(NULL,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);DWORD old;
    require(pages!=NULL&&VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old),"source guard allocation");
    XMM *source=(XMM *)(pages+4096-16);source->u64[0]=UINT64_C(0x7ff0000000000001);source->u64[1]=UINT64_C(0x8000000000000000);
    for(unsigned operation=0;operation<=FP_CMPPD_NLE;++operation) {
        cpu=random_cpu();cpu.mxcsr=0x1f80;CPU expected=cpu;
        expected.mxcsr=oracle(&expected.xmm[0],&cpu.xmm[0],source,cpu.mxcsr,operation);
        require(fp_simd_packed_compare(&cpu,0,source,operation),"guarded source refused");
        require(!memcmp(&cpu,&expected,sizeof(cpu)),"guarded source mismatch");++guarded_reads;
    }
    
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i)if(cases[i].memory) {
        cpu=random_cpu();cpu.mxcsr=0x1f80;prepare_memory(&cpu,&cases[i],source);CPU expected=cpu;
        expected.mxcsr=oracle(&expected.xmm[cases[i].destination],&cpu.xmm[cases[i].destination],source,cpu.mxcsr,cases[i].operation);
        cases[i].repaired(&cpu);require(!memcmp(&cpu,&expected,sizeof(cpu)),"actual overlay guarded memory EA mismatch");++guarded_reads;
    }
    int features[4];__cpuidex(features,0,0);int vendor[]={features[1],features[3],features[2]};int mm=0;
    if(!memcmp(vendor,"AuthenticAMD",12)){__cpuidex(features,(int)0x80000001u,0);mm=(features[2]&(1u<<7))!=0;}
    unsigned char *unaligned=pages+4096-17;memmove(unaligned,source,16);
    for(unsigned operation=0;operation<=FP_CMPPD_NLE;++operation) {
        cpu=random_cpu();cpu.mxcsr=0x1f80;CPU before=cpu;
        require(!fp_simd_packed_compare_memory(&cpu,0,(uintptr_t)unaligned,operation),"unaligned legacy memory accepted without MM");
        require(!memcmp(&cpu,&before,sizeof(cpu)),"alignment rejection changed CPU");++negative_gates;
        cpu.mxcsr=0x21f80;CPU expected=cpu;
        if(mm&&(mask&0x20000)) {
            expected.mxcsr=oracle(&expected.xmm[0],&cpu.xmm[0],(const XMM *)unaligned,cpu.mxcsr,operation);
            require(fp_simd_packed_compare_memory(&cpu,0,(uintptr_t)unaligned,operation),"native supported AMD MM refused");
            require(!memcmp(&cpu,&expected,sizeof(cpu)),"AMD MM result mismatch");++mm_cases;
        } else {
            require(!fp_simd_packed_compare_memory(&cpu,0,(uintptr_t)unaligned,operation),"unsupported AMD MM accepted");
            require(!memcmp(&cpu,&expected,sizeof(cpu)),"AMD MM rejection changed CPU");++negative_gates;
        }
        cpu=before;cpu.mxcsr&=~0x80u;expected=cpu;
        require(!fp_simd_packed_compare_memory(&cpu,0,(uintptr_t)pages+4096,operation),"unmasked memory accepted");
        require(!memcmp(&cpu,&expected,sizeof(cpu)),"unmasked memory rejection changed CPU");++negative_gates;
        cpu=before;cpu.mxcsr|=0x80000000u;expected=cpu;unsigned rejected_host=_mm_getcsr();
        require(!fp_simd_packed_compare_memory(&cpu,0,(uintptr_t)pages+4096,operation),"reserved memory MXCSR accepted");
        require(!memcmp(&cpu,&expected,sizeof(cpu)) && _mm_getcsr()==rejected_host,"reserved memory rejection changed state");++negative_gates;
    }
    VirtualFree(pages,0,MEM_RELEASE);require(legacy_wrong_lanes>=27,"legacy bug counterexamples missing");
    _mm_setcsr(initial);
    printf("{\"success\":true,\"overlay_sites\":%u,\"native_full_CPU_cases\":%u,\"common_compare_cases\":%u,\"negative_gates\":%u,\"legacy_wrong_lanes\":%u,\"guarded_16B_reads\":%u,\"memory_operand_cases\":%u,\"AMD_MM_cases\":%u,\"legacy_wrong_sites\":%u,\"original_bytes_executed\":false}\n",
        (unsigned)(sizeof(cases)/sizeof(*cases)),tested,common_compare_cases,negative_gates,legacy_wrong_lanes,guarded_reads,memory_cases,mm_cases,legacy_wrong_sites);return 0;
}
