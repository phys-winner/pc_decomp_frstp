/* Exact masked CVTTPD2DQ/HADDPS for the statically lifted guest. No game bytes execute. */
#include "aot_cpu.h"
#include <string.h>

static int float_mode(uint32_t mode){return !(mode&~UINT32_C(0xffff)) && (mode&UINT32_C(0x1f80))==UINT32_C(0x1f80);}
static uint32_t truncate_double(uint64_t bits,uint32_t mode,uint32_t *flags) {
    uint64_t absolute=bits&UINT64_C(0x7fffffffffffffff),fraction=absolute&UINT64_C(0x000fffffffffffff);
    unsigned exponent=(unsigned)(absolute>>52),negative=(unsigned)(bits>>63);
    if(exponent==0x7ff){*flags|=1;return UINT32_C(0x80000000);}
    if(!absolute || (!exponent&&(mode&0x40)))return 0;
    int power=(int)exponent-1023;
    if(power<0){*flags|=32;return 0;}
    if(power>31){*flags|=1;return UINT32_C(0x80000000);}
    uint64_t mantissa=fraction|UINT64_C(0x0010000000000000);
    unsigned shift=52-(unsigned)power;uint64_t value=mantissa>>shift;
    if(value>UINT64_C(0x7fffffff)+negative){*flags|=1;return UINT32_C(0x80000000);}
    if(mantissa&((UINT64_C(1)<<shift)-1))*flags|=32;
    return negative?0u-(uint32_t)value:(uint32_t)value;
}
static uint32_t finite_add(uint32_t a,uint32_t b,uint32_t guest,uint32_t *flags) {
    uint32_t result;
#if defined(__aarch64__)
    uint64_t control,status,raised;
    __asm__ volatile("mrs %0,fpcr":"=r"(control));
    unsigned rounding=(guest>>13)&3u,arm=((rounding&1u)<<1)|((rounding&2u)>>1);
    /* Clear FZ, DN, exception enables, FIZ/AH; DAZ/FTZ and NaNs are handled as bits.
       Keep the operation and FPCR/FPSR save/restore inside one compiler barrier. */
    uint64_t mode=(control&~((UINT64_C(3)<<22)|(UINT64_C(1)<<24)|(UINT64_C(1)<<25)|
        (UINT64_C(31)<<8)|(UINT64_C(1)<<15)|UINT64_C(3)))|((uint64_t)arm<<22);
    __asm__ volatile("mrs %[status],fpsr\n\tmsr fpcr,%[mode]\n\tmsr fpsr,xzr\n\t"
        "fmov s0,%w[a]\n\tfmov s1,%w[b]\n\tfadd s0,s0,s1\n\tfmov %w[result],s0\n\t"
        "mrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[result]"=&r"(result),[status]"=&r"(status),[raised]"=&r"(raised)
        :[a]"r"(a),[b]"r"(b),[mode]"r"(mode),[control]"r"(control):"v0","v1","memory");
    *flags|=(uint32_t)((raised&1)|((raised&2)<<1)|((raised&4)<<1)|((raised&8)<<1)|((raised&16)<<1)|((raised&128)>>6));
#elif defined(__x86_64__) || defined(_M_X64)
    uint32_t saved,after,mode=UINT32_C(0x1f80)|(guest&UINT32_C(0x6000));
    __asm__ volatile("stmxcsr %[saved]\n\tldmxcsr %[mode]\n\tmovss %[a],%%xmm0\n\t"
        "addss %[b],%%xmm0\n\tmovss %%xmm0,%[result]\n\tstmxcsr %[after]\n\tldmxcsr %[saved]"
        :[saved]"=m"(saved),[after]"=m"(after),[result]"=m"(result)
        :[mode]"m"(mode),[a]"m"(a),[b]"m"(b):"xmm0","memory");
    *flags|=after&63u;
#else
#error Static HADDPS requires AArch64 or the native x64 source oracle test path.
#endif
    return result;
}
static uint32_t add_float(uint32_t a,uint32_t b,uint32_t mode,uint32_t *flags) {
    uint32_t aa=a&UINT32_C(0x7fffffff),bb=b&UINT32_C(0x7fffffff);
    if(aa>UINT32_C(0x7f800000)||bb>UINT32_C(0x7f800000)) {
        if((aa>UINT32_C(0x7f800000)&&!(aa&UINT32_C(0x400000)))||
           (bb>UINT32_C(0x7f800000)&&!(bb&UINT32_C(0x400000))))*flags|=1;
        return (aa>UINT32_C(0x7f800000)?a:b)|UINT32_C(0x400000);
    }
    unsigned da=(aa-1)<UINT32_C(0x007fffff),db=(bb-1)<UINT32_C(0x007fffff);
    if(mode&0x40){if(da){a&=UINT32_C(0x80000000);aa=0;}if(db){b&=UINT32_C(0x80000000);bb=0;}}
    else if(da||db)*flags|=2;
    if(aa==UINT32_C(0x7f800000)&&bb==UINT32_C(0x7f800000)&&((a^b)>>31)) {
        *flags|=1;return UINT32_C(0xffc00000);
    }
    uint32_t result=finite_add(a,b,mode,flags),absolute=result&UINT32_C(0x7fffffff);
    if((mode&0x8000)&&absolute&&absolute<UINT32_C(0x00800000)) {
        result&=UINT32_C(0x80000000);*flags|=UINT32_C(0x30);
    }
    return result;
}
int fp_aot_float_operation(CPU *c,unsigned operation,unsigned destination,unsigned source) {
    if(!c||destination>=16||source>=16||(operation!=6&&operation!=7)||!float_mode(c->mxcsr))return 0;
    /* shortcut: masked exceptions only; reject without touching CPU until guest SSE trap delivery exists. */
    XMM s=c->xmm[source],d=c->xmm[destination],result;uint32_t flags=0;
    if(operation==6){result.u64[0]=result.u64[1]=0;
        result.u32[0]=truncate_double(s.u64[0],c->mxcsr,&flags);
        result.u32[1]=truncate_double(s.u64[1],c->mxcsr,&flags);
    }else {
        result.u32[0]=add_float(d.u32[0],d.u32[1],c->mxcsr,&flags);
        result.u32[1]=add_float(d.u32[2],d.u32[3],c->mxcsr,&flags);
        result.u32[2]=add_float(s.u32[0],s.u32[1],c->mxcsr,&flags);
        result.u32[3]=add_float(s.u32[2],s.u32[3],c->mxcsr,&flags);
    }
    c->xmm[destination]=result;c->mxcsr|=flags;return 1;
}
