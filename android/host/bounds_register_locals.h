#pragma once
#include "cpu64.h"

static inline int fp_bounds_separate(uint64_t address,unsigned bytes,const CPU*c) {
    uint64_t cpu=(uintptr_t)c;
    return address<=UINT64_MAX-bytes && !(address<cpu+sizeof(*c) && cpu<address+bytes);
}

static inline int fp_bounds_disjoint(uint64_t a,unsigned an,uint64_t b,unsigned bn) {
    return !(a<b+bn && b<a+an);
}

static inline int fp_bounds_finite(uint64_t address,unsigned floats) {
    for(unsigned i=0;i<floats;++i) {
        uint32_t bits=rd32(address+4*i)&0x7fffffffu;
        if(bits>=0x7f800000u)return 0;
    }
    return 1;
}
