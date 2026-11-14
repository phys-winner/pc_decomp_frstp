#include "simd_float.h"
#include <intrin.h>
#include <string.h>

typedef struct {
    uint64_t va;
    const char *instruction;
    unsigned char bytes[4];
    unsigned char operation,destination,source;
} exact_site;
enum { CVTTPD2DQ, HADDPS };
static const exact_site sites[]={
    {UINT64_C(0x141ae18d7), "cvttpd2dq xmm5, xmm5", {0x66,0x0f,0xe6,0xed}, CVTTPD2DQ, 5, 5},
    {UINT64_C(0x141ae19f0), "cvttpd2dq xmm5, xmm5", {0x66,0x0f,0xe6,0xed}, CVTTPD2DQ, 5, 5},
    {UINT64_C(0x141ae1b92), "cvttpd2dq xmm5, xmm5", {0x66,0x0f,0xe6,0xed}, CVTTPD2DQ, 5, 5},
    {UINT64_C(0x141ae1b96), "cvttpd2dq xmm6, xmm6", {0x66,0x0f,0xe6,0xf6}, CVTTPD2DQ, 6, 6},
    {UINT64_C(0x141b6245f), "cvttpd2dq xmm0, xmm0", {0x66,0x0f,0xe6,0xc0}, CVTTPD2DQ, 0, 0},
    {UINT64_C(0x141b6246a), "cvttpd2dq xmm0, xmm1", {0x66,0x0f,0xe6,0xc1}, CVTTPD2DQ, 0, 1},
    {UINT64_C(0x141b6247f), "cvttpd2dq xmm0, xmm1", {0x66,0x0f,0xe6,0xc1}, CVTTPD2DQ, 0, 1},
    {UINT64_C(0x141b6248f), "cvttpd2dq xmm0, xmm3", {0x66,0x0f,0xe6,0xc3}, CVTTPD2DQ, 0, 3},
    {UINT64_C(0x141b624ba), "cvttpd2dq xmm0, xmm0", {0x66,0x0f,0xe6,0xc0}, CVTTPD2DQ, 0, 0},
    {UINT64_C(0x141b624c5), "cvttpd2dq xmm0, xmm1", {0x66,0x0f,0xe6,0xc1}, CVTTPD2DQ, 0, 1},
    {UINT64_C(0x141b624da), "cvttpd2dq xmm0, xmm1", {0x66,0x0f,0xe6,0xc1}, CVTTPD2DQ, 0, 1},
    {UINT64_C(0x141b624e6), "cvttpd2dq xmm0, xmm3", {0x66,0x0f,0xe6,0xc3}, CVTTPD2DQ, 0, 3},
    {UINT64_C(0x141bf7473), "haddps xmm2, xmm0", {0xf2,0x0f,0x7c,0xd0}, HADDPS, 2, 0},
    {UINT64_C(0x141bf7493), "haddps xmm1, xmm0", {0xf2,0x0f,0x7c,0xc8}, HADDPS, 1, 0},
    {UINT64_C(0x141bf74ac), "haddps xmm2, xmm1", {0xf2,0x0f,0x7c,0xd1}, HADDPS, 2, 1},
    {UINT64_C(0x141bf7c43), "haddps xmm2, xmm0", {0xf2,0x0f,0x7c,0xd0}, HADDPS, 2, 0},
    {UINT64_C(0x141bf7c63), "haddps xmm1, xmm0", {0xf2,0x0f,0x7c,0xc8}, HADDPS, 1, 0},
    {UINT64_C(0x141bf7c90), "haddps xmm2, xmm1", {0xf2,0x0f,0x7c,0xd1}, HADDPS, 2, 1},
    {UINT64_C(0x141c69519), "cvttpd2dq xmm3, xmm3", {0x66,0x0f,0xe6,0xdb}, CVTTPD2DQ, 3, 3},
};

uint32_t fp_simd_float_mxcsr_mask_native(void);
uint32_t fp_cvttpd2dq_native(XMM *out,const XMM *source,uint32_t mxcsr);
uint32_t fp_haddps_native(XMM *out,const XMM *destination,const XMM *source,uint32_t mxcsr);
uint32_t fp_cmpeqps_native(XMM *,const XMM *,const XMM *,uint32_t);
uint32_t fp_cmpltps_native(XMM *,const XMM *,const XMM *,uint32_t);
uint32_t fp_cmpeqpd_native(XMM *,const XMM *,const XMM *,uint32_t);
uint32_t fp_cmpltpd_native(XMM *,const XMM *,const XMM *,uint32_t);
uint32_t fp_cmpnlepd_native(XMM *,const XMM *,const XMM *,uint32_t);
static uint32_t mxcsr_mask;
static int has_sse3;
static int has_misaligned_sse;

int fp_simd_float_init(void) {
    int features[4];__cpuidex(features,1,0);
    if(!(features[3]&(1u<<26)))return 0;
    has_sse3=(features[2]&1)!=0;
    __cpuidex(features,0,0);
    const int vendor[]={features[1],features[3],features[2]};
    has_misaligned_sse=0;
    if(!memcmp(vendor,"AuthenticAMD",12)) {
        __cpuidex(features,(int)UINT32_C(0x80000000),0);
        if((uint32_t)features[0]>=UINT32_C(0x80000001)) {
            __cpuidex(features,(int)UINT32_C(0x80000001),0);
            has_misaligned_sse=(features[2]&(1u<<7))!=0;
        }
    }
    mxcsr_mask=fp_simd_float_mxcsr_mask_native();return 1;
}
int fp_simd_packed_compare(CPU *cpu,unsigned destination,const XMM *source,unsigned operation) {
    if(!cpu || !source || destination>=16 || operation>FP_CMPPD_NLE || !mxcsr_mask)return 0;
    /* shortcut: reject unmasked/invalid MXCSR until a guest SSE exception bridge exists. */
    if((cpu->mxcsr&~mxcsr_mask) || (cpu->mxcsr&UINT32_C(0x1f80))!=UINT32_C(0x1f80))return 0;
    XMM s=*source,d=cpu->xmm[destination],result;uint32_t after;
    switch(operation) {
    case FP_CMPPS_EQ:after=fp_cmpeqps_native(&result,&d,&s,cpu->mxcsr);break;
    case FP_CMPPS_LT:after=fp_cmpltps_native(&result,&d,&s,cpu->mxcsr);break;
    case FP_CMPPD_EQ:after=fp_cmpeqpd_native(&result,&d,&s,cpu->mxcsr);break;
    case FP_CMPPD_LT:after=fp_cmpltpd_native(&result,&d,&s,cpu->mxcsr);break;
    default:after=fp_cmpnlepd_native(&result,&d,&s,cpu->mxcsr);break;
    }
    cpu->xmm[destination]=result;cpu->mxcsr=after;return 1;
}
int fp_simd_packed_compare_memory(CPU *cpu,unsigned destination,uint64_t address,unsigned operation) {
    if(!cpu || destination>=16 || operation>FP_CMPPD_NLE || !mxcsr_mask ||
       (cpu->mxcsr&~mxcsr_mask) || (cpu->mxcsr&UINT32_C(0x1f80))!=UINT32_C(0x1f80) ||
       address>UINT64_MAX-15)return 0;
    if((address&15) && !(has_misaligned_sse && (cpu->mxcsr&UINT32_C(0x20000))))return 0;
    XMM source;rd128(address,&source);
    return fp_simd_packed_compare(cpu,destination,&source,operation);
}
int fp_simd_float(CPU *cpu,const fp_image *image,uint64_t va,const char *instruction) {
    if(!cpu || !image || !image->base || !instruction || !mxcsr_mask)return 0;
    /* shortcut: reject unmasked/invalid MXCSR until a guest SSE exception bridge exists. */
    if((cpu->mxcsr&~mxcsr_mask) || (cpu->mxcsr&UINT32_C(0x1f80))!=UINT32_C(0x1f80))return 0;
    size_t lo=0,hi=sizeof(sites)/sizeof(*sites);
    while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(sites[mid].va<va)lo=mid+1;else hi=mid;}
    if(lo==sizeof(sites)/sizeof(*sites) || sites[lo].va!=va)return 0;
    const exact_site *site=&sites[lo];
    if((site->operation==HADDPS&&!has_sse3) || strcmp(instruction,site->instruction) || va<image->preferred_base)return 0;
    uint64_t offset=va-image->preferred_base,base=(uintptr_t)image->base;
    if(offset>=image->size || 4>image->size-offset || base>UINT64_MAX-offset || base+offset>UINT64_MAX-3)return 0;
    uint64_t address=base+offset;
    if(!fp_image_code(image,address) || !fp_image_code(image,address+3) ||
       memcmp((const void *)(uintptr_t)address,site->bytes,4))return 0;
    XMM source=cpu->xmm[site->source],result;
    uint32_t result_mxcsr;
    if(site->operation==CVTTPD2DQ)result_mxcsr=fp_cvttpd2dq_native(&result,&source,cpu->mxcsr);
    else {
        XMM destination=cpu->xmm[site->destination];
        result_mxcsr=fp_haddps_native(&result,&destination,&source,cpu->mxcsr);
    }
    cpu->xmm[site->destination]=result;cpu->mxcsr=result_mxcsr;return 1;
}
