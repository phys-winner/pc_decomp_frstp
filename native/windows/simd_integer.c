#include "simd_integer.h"
#include <string.h>

enum { PMULLD, PMOVZXBD, PABSD, PSADBW, PMOVSXDQ };
typedef struct {
    uint64_t va;
    const char *instruction;
    unsigned char bytes[6];
    unsigned char size, operation, destination, source;
} exact_site;

static const exact_site sites[]={
    {UINT64_C(0x141b503bc), "pmulld xmm2, xmm1", {0x66,0x0f,0x38,0x40,0xd1}, 5, PMULLD, 2, 1},
    {UINT64_C(0x141b503c5), "pmulld xmm4, xmm1", {0x66,0x0f,0x38,0x40,0xe1}, 5, PMULLD, 4, 1},
    {UINT64_C(0x141b50421), "pmulld xmm2, xmm1", {0x66,0x0f,0x38,0x40,0xd1}, 5, PMULLD, 2, 1},
    {UINT64_C(0x141b5042a), "pmulld xmm4, xmm1", {0x66,0x0f,0x38,0x40,0xe1}, 5, PMULLD, 4, 1},
    {UINT64_C(0x141b5172f), "pmulld xmm4, xmm8", {0x66,0x41,0x0f,0x38,0x40,0xe0}, 6, PMULLD, 4, 8},
    {UINT64_C(0x141b51735), "pmulld xmm0, xmm8", {0x66,0x41,0x0f,0x38,0x40,0xc0}, 6, PMULLD, 0, 8},
    {UINT64_C(0x141b5178a), "pmulld xmm4, xmm8", {0x66,0x41,0x0f,0x38,0x40,0xe0}, 6, PMULLD, 4, 8},
    {UINT64_C(0x141b51790), "pmulld xmm0, xmm8", {0x66,0x41,0x0f,0x38,0x40,0xc0}, 6, PMULLD, 0, 8},
    {UINT64_C(0x141b52ea5), "pmulld xmm5, xmm4", {0x66,0x0f,0x38,0x40,0xec}, 5, PMULLD, 5, 4},
    {UINT64_C(0x141b52eaa), "pmulld xmm0, xmm4", {0x66,0x0f,0x38,0x40,0xc4}, 5, PMULLD, 0, 4},
    {UINT64_C(0x141b52f0f), "pmulld xmm5, xmm4", {0x66,0x0f,0x38,0x40,0xec}, 5, PMULLD, 5, 4},
    {UINT64_C(0x141b52f19), "pmulld xmm0, xmm4", {0x66,0x0f,0x38,0x40,0xc4}, 5, PMULLD, 0, 4},
    {UINT64_C(0x141b53940), "pmulld xmm0, xmm7", {0x66,0x0f,0x38,0x40,0xc7}, 5, PMULLD, 0, 7},
    {UINT64_C(0x141b53949), "pmulld xmm3, xmm7", {0x66,0x0f,0x38,0x40,0xdf}, 5, PMULLD, 3, 7},
    {UINT64_C(0x141b5399e), "pmulld xmm0, xmm7", {0x66,0x0f,0x38,0x40,0xc7}, 5, PMULLD, 0, 7},
    {UINT64_C(0x141b539a3), "pmulld xmm3, xmm7", {0x66,0x0f,0x38,0x40,0xdf}, 5, PMULLD, 3, 7},
    {UINT64_C(0x141bafd59), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bafd74), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bafd83), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bafd92), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bb01a9), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bb01c4), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bb01d3), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bb01e2), "pmovsxdq xmm1, xmm0", {0x66,0x0f,0x38,0x25,0xc8}, 5, PMOVSXDQ, 1, 0},
    {UINT64_C(0x141bd5cae), "pmulld xmm1, xmm0", {0x66,0x0f,0x38,0x40,0xc8}, 5, PMULLD, 1, 0},
    {UINT64_C(0x141c48184), "pmovzxbd xmm1, xmm0", {0x66,0x0f,0x38,0x31,0xc8}, 5, PMOVZXBD, 1, 0},
    {UINT64_C(0x141c48192), "pmulld xmm1, xmm1", {0x66,0x0f,0x38,0x40,0xc9}, 5, PMULLD, 1, 1},
    {UINT64_C(0x141c4819f), "pmovzxbd xmm1, xmm0", {0x66,0x0f,0x38,0x31,0xc8}, 5, PMOVZXBD, 1, 0},
    {UINT64_C(0x141c481a8), "pmulld xmm1, xmm1", {0x66,0x0f,0x38,0x40,0xc9}, 5, PMULLD, 1, 1},
    {UINT64_C(0x141c505f1), "psadbw xmm2, xmm0", {0x66,0x0f,0xf6,0xd0}, 4, PSADBW, 2, 0},
    {UINT64_C(0x141c50630), "psadbw xmm2, xmm0", {0x66,0x0f,0xf6,0xd0}, 4, PSADBW, 2, 0},
    {UINT64_C(0x141c509ce), "pmovzxbd xmm2, xmm0", {0x66,0x0f,0x38,0x31,0xd0}, 5, PMOVZXBD, 2, 0},
    {UINT64_C(0x141c509d3), "pmovzxbd xmm0, xmm1", {0x66,0x0f,0x38,0x31,0xc1}, 5, PMOVZXBD, 0, 1},
    {UINT64_C(0x141c509e7), "pmulld xmm2, xmm2", {0x66,0x0f,0x38,0x40,0xd2}, 5, PMULLD, 2, 2},
    {UINT64_C(0x141c509f4), "pmovzxbd xmm2, xmm0", {0x66,0x0f,0x38,0x31,0xd0}, 5, PMOVZXBD, 2, 0},
    {UINT64_C(0x141c509f9), "pmovzxbd xmm0, xmm1", {0x66,0x0f,0x38,0x31,0xc1}, 5, PMOVZXBD, 0, 1},
    {UINT64_C(0x141c50a02), "pmulld xmm2, xmm2", {0x66,0x0f,0x38,0x40,0xd2}, 5, PMULLD, 2, 2},
    {UINT64_C(0x141c63d1d), "pabsd xmm1, xmm0", {0x66,0x0f,0x38,0x1e,0xc8}, 5, PABSD, 1, 0},
    {UINT64_C(0x141c63d30), "pabsd xmm1, xmm0", {0x66,0x0f,0x38,0x1e,0xc8}, 5, PABSD, 1, 0},
    {UINT64_C(0x141c649cd), "pabsd xmm1, xmm0", {0x66,0x0f,0x38,0x1e,0xc8}, 5, PABSD, 1, 0},
    {UINT64_C(0x141c649e4), "pabsd xmm1, xmm0", {0x66,0x0f,0x38,0x1e,0xc8}, 5, PABSD, 1, 0},
    {UINT64_C(0x141c64e2a), "pmulld xmm1, xmm1", {0x66,0x0f,0x38,0x40,0xc9}, 5, PMULLD, 1, 1},
    {UINT64_C(0x141c64e45), "pmulld xmm1, xmm1", {0x66,0x0f,0x38,0x40,0xc9}, 5, PMULLD, 1, 1},
    {UINT64_C(0x141c67bc2), "pmulld xmm0, xmm4", {0x66,0x0f,0x38,0x40,0xc4}, 5, PMULLD, 0, 4},
    {UINT64_C(0x141c67bc7), "pmulld xmm2, xmm6", {0x66,0x0f,0x38,0x40,0xd6}, 5, PMULLD, 2, 6},
};

int fp_simd_integer(CPU *cpu,const fp_image *image,uint64_t va,const char *instruction) {
    if(!cpu || !image || !image->base || !instruction)return 0;
    size_t lo=0,hi=sizeof(sites)/sizeof(*sites);
    while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(sites[mid].va<va)lo=mid+1;else hi=mid;}
    if(lo==sizeof(sites)/sizeof(*sites) || sites[lo].va!=va)return 0;
    const exact_site *site=&sites[lo];
    if(strcmp(instruction,site->instruction) || va<image->preferred_base)return 0;
    uint64_t offset=va-image->preferred_base,base=(uintptr_t)image->base;
    if(offset>=image->size || site->size>image->size-offset ||
       base>UINT64_MAX-offset || base+offset>UINT64_MAX-(site->size-1))return 0;
    uint64_t address=base+offset;
    if(!fp_image_code(image,address) || !fp_image_code(image,address+site->size-1) ||
       memcmp((const void *)(uintptr_t)address,site->bytes,site->size))return 0;

    XMM result;
    if(site->operation==PMOVZXBD) {
        /* PMOVZXBD consumes only the low four source bytes. */
        uint8_t source[4];memcpy(source,&cpu->xmm[site->source],sizeof(source));
        for(unsigned i=0;i<4;++i)result.u32[i]=source[i];
    } else if(site->operation==PMOVSXDQ) {
        int32_t source[2];memcpy(source,&cpu->xmm[site->source],sizeof(source));
        for(unsigned i=0;i<2;++i)result.i64[i]=(int64_t)source[i];
    } else {
        XMM source=cpu->xmm[site->source];
        if(site->operation==PMULLD) {
            XMM destination=cpu->xmm[site->destination];
            for(unsigned i=0;i<4;++i)result.u32[i]=destination.u32[i]*source.u32[i];
        } else if(site->operation==PABSD) {
            for(unsigned i=0;i<4;++i) {
                uint32_t value=source.u32[i];
                result.u32[i]=(value&UINT32_C(0x80000000))?0u-value:value;
            }
        } else {
            XMM destination=cpu->xmm[site->destination];
            result.u64[0]=result.u64[1]=0;
            for(unsigned i=0;i<16;++i) {
                int difference=(int)destination.u8[i]-(int)source.u8[i];
                result.u64[i/8]+=(unsigned)(difference<0?-difference:difference);
            }
        }
    }
    cpu->xmm[site->destination]=result;return 1;
}
