#pragma once
#include "cpu64.h"

enum { FP_CF=1, FP_ZF=2, FP_SF=4, FP_OF=8, FP_PF=16, FP_AF=32 };

static inline uint64_t fp_flags_add(CPU *c, uint64_t a, uint64_t b, int sz, unsigned live) {
    uint64_t m=mask_sz(sz), sb=signbit_sz(sz); a&=m; b&=m;
    uint64_t r=(a+b)&m;
    if(live&FP_CF)c->cf=r<a;
    if(live&FP_ZF)c->zf=r==0;
    if(live&FP_SF)c->sf=(r&sb)!=0;
    if(live&FP_OF)c->of=((~(a^b)&(a^r))&sb)!=0;
    if(live&FP_PF)c->pf=parity8((uint8_t)r);
    if(live&FP_AF)c->af=((a^b^r)>>4)&1;
    return r;
}

static inline uint64_t fp_flags_sub(CPU *c, uint64_t a, uint64_t b, int sz, unsigned live) {
    uint64_t m=mask_sz(sz), sb=signbit_sz(sz); a&=m; b&=m;
    uint64_t r=(a-b)&m;
    if(live&FP_CF)c->cf=a<b;
    if(live&FP_ZF)c->zf=r==0;
    if(live&FP_SF)c->sf=(r&sb)!=0;
    if(live&FP_OF)c->of=(((a^b)&(a^r))&sb)!=0;
    if(live&FP_PF)c->pf=parity8((uint8_t)r);
    if(live&FP_AF)c->af=((a^b^r)>>4)&1;
    return r;
}

static inline uint64_t fp_flags_logicz(CPU *c, uint64_t r, int sz, unsigned live) {
    r&=mask_sz(sz);
    if(live&FP_CF)c->cf=0;
    if(live&FP_OF)c->of=0;
    if(live&FP_ZF)c->zf=r==0;
    if(live&FP_SF)c->sf=(r&signbit_sz(sz))!=0;
    if(live&FP_PF)c->pf=parity8((uint8_t)r);
    return r;
}
