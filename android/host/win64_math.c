/* Bounded Win64 UCRT scalar math ABI. Values enter/leave XMM0, not GPRs.
 * UCRT's bit-level NaN/domain policy differs from raw ARM FSQRT. Positive
 * finite values use real hardware square root with the guest rounding mode;
 * native thread FP control/status are restored before returning. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "win64_math.h"
#include "win64_stdio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif
extern uint64_t win64_math_error_handler(void);
static int default_matherr_verified;
static int custom_matherr(void){
    uint64_t handler=win64_math_error_handler();
    return handler&&(!default_matherr_verified||handler!=UINT64_C(0x140d871c0));
}
static int portable_math_call(CPU *c,const char *name);
int win64_math_init(const void *mapped_pe,size_t image_size){
    default_matherr_verified=0;
    const size_t offset=0xd871c0;
    if(!mapped_pe||image_size<offset+3)return 0;
    /* The registered original140D871C0 is XOR EAX,EAX; RET: it returns0
     * with no persistent memory effects. The provider keeps its CPU ABI. */
    if(memcmp((const unsigned char*)mapped_pe+offset,"\x33\xc0\xc3",3))return 0;
    default_matherr_verified=1;return 1;
}
static void stop(const char*why){fprintf(stderr,"STOP Win64 math %s\n",why);fflush(stderr);_Exit(73);}
static uint64_t native_control(uint64_t control,unsigned rounding){
    unsigned arm_round=((rounding&1u)<<1)|((rounding&2u)>>1);
    /* Trap delivery follows guest MXCSR, never inherited native enables. */
    return(control&~((UINT64_C(3)<<22)|(UINT64_C(1)<<24)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((uint64_t)arm_round<<22);
}
static uint64_t positive_root(uint64_t bits,int wide,unsigned rounding,unsigned *flags){
#if defined(__aarch64__)
    uint64_t control,status,raised;uint64_t desired;
    __asm__ volatile("mrs %0, fpcr":"=r"(control));
    /* ARM RC: nearest,+infinity,-infinity,zero; x86 RC swaps middle two. */
    desired=native_control(control,rounding);
    if(wide){
        double x,y;memcpy(&x,&bits,8);
        __asm__ volatile("mrs %[old], fpsr\n\tmsr fpcr, %[mode]\n\tmsr fpsr, xzr\n\tfsqrt %d[result], %d[input]\n\tmrs %[raised], fpsr\n\tmsr fpcr, %[control]\n\tmsr fpsr, %[old]"
            :[result]"=&w"(y),[old]"=&r"(status),[raised]"=&r"(raised)
            :[input]"w"(x),[mode]"r"(desired),[control]"r"(control):"memory");
        memcpy(&bits,&y,8);
    }else{
        uint32_t low=(uint32_t)bits;float x,y;memcpy(&x,&low,4);
        __asm__ volatile("mrs %[old], fpsr\n\tmsr fpcr, %[mode]\n\tmsr fpsr, xzr\n\tfsqrt %s[result], %s[input]\n\tmrs %[raised], fpsr\n\tmsr fpcr, %[control]\n\tmsr fpsr, %[old]"
            :[result]"=&w"(y),[old]"=&r"(status),[raised]"=&r"(raised)
            :[input]"w"(x),[mode]"r"(desired),[control]"r"(control):"memory");
        memcpy(&low,&y,4);bits=low;
    }
    if(raised&15u)stop("unexpected ARM finite-positive sqrt exception");
    if(raised&16u)*flags|=32u;
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
    if(wide){double x,y;memcpy(&x,&bits,8);__m128d v=_mm_set_sd(x);y=_mm_cvtsd_f64(_mm_sqrt_sd(v,v));memcpy(&bits,&y,8);}
    else{uint32_t low=(uint32_t)bits;float x,y;memcpy(&x,&low,4);y=_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));memcpy(&low,&y,4);bits=low;}
    unsigned raised=_mm_getcsr();_mm_setcsr(control);*flags|=raised&63u;
#else
#error Win64 sqrt requires the tested ARM64 or native x64 hardware path.
#endif
    return bits;
}
static uint64_t positive_log2(uint64_t bits,int wide,unsigned rounding){
    int saved_errno=errno;uint64_t result;
#if defined(__aarch64__)
    uint64_t control,status;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    if(wide){double x,y;memcpy(&x,&bits,8);double(*volatile fn)(double)=log2;y=fn(x);memcpy(&result,&y,8);}
    else{uint32_t low=(uint32_t)bits;float x,y;memcpy(&x,&low,4);float(*volatile fn)(float)=log2f;y=fn(x);memcpy(&low,&y,4);result=low;}
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(control),"r"(status):"memory");
#else
    _mm_setcsr(control);
#endif
    errno=saved_errno;return result;
}
/* Only finite interior inputs reach host exp2. Win64 UCRT's quiet range
 * clamps, DAZ comparisons and errno policy are handled in integer code below.
 * Platform transcendental results have a tested ULP bound, not bit identity. */
static uint64_t finite_exp2(uint64_t bits,unsigned rounding){
    int saved_errno=errno;uint64_t result;
#if defined(__aarch64__)
    uint64_t control,status;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    double x,y;memcpy(&x,&bits,8);double(*volatile fn)(double)=exp2;y=fn(x);memcpy(&result,&y,8);
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(control),"r"(status):"memory");
#else
    _mm_setcsr(control);
#endif
    errno=saved_errno;return result;
}
static int exp2_call(CPU *c,const char *name){
    if(strcmp(name,"exp2"))return 0;
    uint64_t bits=c->xmm[0].u64[0],m=bits&UINT64_C(0x7fffffffffffffff);
    const uint64_t inf=UINT64_C(0x7ff0000000000000),quiet=UINT64_C(0x0008000000000000),one=UINT64_C(0x3ff0000000000000);
    unsigned flags=0,rc=(c->mxcsr>>13)&3u;int negative=(int)(bits>>63);
    if(m>inf){if(!(m&quiet))flags=1;bits|=quiet;}
    else if(m==inf)bits=negative?0:inf;
    else if(!m)bits=one;
    else if(negative&&m>=UINT64_C(0x4090cc0000000000))bits=0; /* UCRT x<=-1075 quiet clamp. */
    else if(!negative&&m>=UINT64_C(0x4090000000000000)){
        bits=(rc==1||rc==3)?inf-1:inf;flags=40;win64_stdio_set_errno(34);
    }else if(m<=UINT64_C(0x3c90000000000000)){
        if(m<UINT64_C(0x0010000000000000)&&(c->mxcsr&64u))bits=one;
        else{
            bits=one;
            if(!negative&&rc==2)++bits;
            if(negative&&(rc==1||rc==3))--bits;
            flags=32;if(m<UINT64_C(0x0010000000000000))flags|=2;
        }
    }else{
        int e=(int)(m>>52)-1023,integer=0,n=0;
        if(e>=0&&e<=10){unsigned shift=52u-(unsigned)e;uint64_t fraction=m&UINT64_C(0x000fffffffffffff);
            if(!(fraction&((UINT64_C(1)<<shift)-1))){integer=1;n=(int)((fraction|(UINT64_C(1)<<52))>>shift);if(negative)n=-n;}}
        if(integer){
            if(n>=-1022)bits=(uint64_t)(n+1023)<<52;
            else bits=UINT64_C(1)<<(unsigned)(n+1074);
        }else bits=finite_exp2(bits,rc);
        if(negative&&m>UINT64_C(0x408ff00000000000)){
            /* UCRT's subnormal result comparison itself raises denormal when
             * DAZ is clear; its gradual path raises underflow+precision even
             * for exact powers. DAZ leaves returned bits but compares as0. */
            flags=48;
            if(c->mxcsr&32768u)bits=0;
            if(bits&&!(c->mxcsr&64u))flags|=2;
            else win64_stdio_set_errno(34);
        }else if(!integer)flags=32;
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;c->xmm[0].u64[0]=bits;return 1;
}
static uint64_t angle_double(uint32_t bits);
static uint32_t finite_exp2f_normal(uint32_t input,unsigned rounding){
    uint64_t raw=angle_double(input);double x,wide;float output;uint32_t result;unsigned raised;int saved_errno=errno;
    memcpy(&x,&raw,8);
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,0u);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
    double(*volatile fn)(double)=exp2;wide=fn(x);
    uint64_t conversion=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]\n\tmrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[out]"=&w"(output),[raised]"=&r"(actual):[mode]"r"(conversion),[in]"w"(wide),[control]"r"(control),[status]"r"(status):"memory");
    raised=((actual&1u)?1u:0u)|((actual&2u)?4u:0u)|((actual&4u)?8u:0u)|((actual&8u)?16u:0u)|((actual&16u)?32u:0u);
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u);
    double(*volatile fn)(double)=exp2;wide=fn(x);
    unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
    output=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;memcpy(&result,&output,4);
    if(raised&31u)stop("unexpected native normal-result exp2f exception");
    return result;
}
/* shortcut: actual fractional startup inputs survived the previous bounds
 * guard. Original direct-IAT settings callers interpolate log2 inputs or
 * read continuous scalars. Support finite[-126,127], whose results stay normal;
 * wider range/error/matherr behavior still needs a separate guest bridge.
 * Tiny finite results near1 have the same measured ULP bound as other
 * fractional inputs; status/DAZ behavior is modeled independently. */
static int exp2f_call(CPU *c,const char *name){
    if(strcmp(name,"exp2f"))return 0;
    uint32_t bits=c->xmm[0].u32[0],m=bits&UINT32_C(0x7fffffff),result=UINT32_C(0x3f800000);
    unsigned flags=0,rounding=(c->mxcsr>>13)&3u;int negative=(int)(bits>>31);
    const char *why="exp2f outside proven finite[-126,127] normal-result profile";
    if(c->mxcsr&UINT32_C(0xffff0000))goto reject;
    if(m>(negative?UINT32_C(0x42fc0000):UINT32_C(0x42fe0000)))return portable_math_call(c,name);
    if(m){
        int e=(int)(m>>23)-127,integral=0;
        if(e>=0){
            unsigned shift=23u-(unsigned)e;
            if(!(m&((UINT32_C(1)<<shift)-1u))){
                int n=(int)(((m&UINT32_C(0x007fffff))|UINT32_C(0x00800000))>>shift);
                if(negative)n=-n;result=(uint32_t)(n+127)<<23;integral=1;
            }
        }
        if(!integral){
            if(m<UINT32_C(0x00800000)&&(c->mxcsr&64u))result=UINT32_C(0x3f800000);
            else{
                flags=32;if(m<UINT32_C(0x00800000))flags|=2;
                if(m<UINT32_C(0x32800000)){
                    if(negative&&(rounding==1u||rounding==3u))--result;
                    if(!negative&&rounding==2u)++result;
                }else result=finite_exp2f_normal(bits,rounding);
            }
        }
    }
    if(flags&~((c->mxcsr>>7)&63u)){why="exp2f unmasked guest floating-point exception is unsupported";goto reject;}
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
reject:
    fprintf(stderr,"MATH_INPUT name=exp2f x=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        bits,c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);return 1;
}
/* Typed powf preserves the native environment; UCRT's status policy is applied
 * separately because even its exact integral powers usually raise precision. */
static uint32_t finite_power(uint32_t bits,uint32_t power,unsigned rounding){
    int saved_errno=errno;uint32_t result;unsigned raised;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    float x,y,z;memcpy(&x,&bits,4);memcpy(&y,&power,4);
    float(*volatile fn)(float,float)=powf;z=fn(x,y);memcpy(&result,&z,4);
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(native_raised):"r"(control),"r"(status):"memory");
    raised=(unsigned)native_raised&15u;
#else
    raised=_mm_getcsr()&31u;_mm_setcsr(control);
#endif
    errno=saved_errno;
    if(raised)return 0;
    return result;
}
static void powf_domain_stop(CPU*c,const char*why){
    fprintf(stderr,"MATH_INPUT name=powf x=0x%08x y=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        (unsigned)c->xmm[0].u32[0],(unsigned)c->xmm[1].u32[0],(unsigned)c->mxcsr,
        (unsigned long long)c->rip,(unsigned long long)c->rsp);
    stop(why);
}
static int powf_call(CPU*c,const char*name){
    if(strcmp(name,"powf"))return 0;
    uint32_t x=c->xmm[0].u32[0],y=c->xmm[1].u32[0],xm=x&UINT32_C(0x7fffffff),ym=y&UINT32_C(0x7fffffff),result;
    if((xm&&(x<UINT32_C(0x00800000)||x>=UINT32_C(0x7f800000))) ||
       (ym&&(ym<UINT32_C(0x00800000)||ym>=UINT32_C(0x7f800000))))
        return portable_math_call(c,name);
    unsigned flags=0;
    if(!ym||x==UINT32_C(0x3f800000))result=UINT32_C(0x3f800000);
    else if(y==UINT32_C(0x3f800000))result=x;
    else if(!xm){
        if(y>>31)return portable_math_call(c,name);
        result=0;
        int e=(int)(ym>>23)-127;
        if((x>>31)&&e>=0&&e<=23){
            unsigned shift=23u-(unsigned)e;uint32_t mantissa=(ym&UINT32_C(0x007fffff))|UINT32_C(0x00800000);
            if(!(mantissa&((UINT32_C(1)<<shift)-1u))&&((mantissa>>shift)&1u))result=UINT32_C(0x80000000);
        }
    }else{
        flags=32;
        if(flags&~((c->mxcsr>>7)&63u))powf_domain_stop(c,"unmasked guest floating-point exception is unsupported");
        result=finite_power(x,y,(c->mxcsr>>13)&3u);
        if(result<UINT32_C(0x01000000)||result>UINT32_C(0x7f000000))
            return portable_math_call(c,name);
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
}
static void pow_domain_stop(CPU *c,const char *why) {
    fprintf(stderr,"MATH_INPUT name=pow x=0x%016llx y=0x%016llx mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        (unsigned long long)c->xmm[0].u64[0],(unsigned long long)c->xmm[1].u64[0],c->mxcsr,
        (unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);
}
static uint64_t finite_power64(CPU *c,uint64_t bits,uint64_t power,unsigned rounding) {
    int saved_errno=errno;uint64_t result;unsigned raised;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    double x,y,z;memcpy(&x,&bits,8);memcpy(&y,&power,8);
    double(*volatile fn)(double,double)=pow;z=fn(x,y);memcpy(&result,&z,8);
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(native_raised):"r"(control),"r"(status):"memory");
    raised=(unsigned)native_raised&15u;
#else
    raised=_mm_getcsr()&31u;_mm_setcsr(control);
#endif
    errno=saved_errno;
    if(raised||result<UINT64_C(0x0010000000000000)||result>=UINT64_C(0x7ff0000000000000)){
        portable_math_call(c,"pow");return c->xmm[0].u64[0];
    }
    return result;
}
/* shortcut: positive normal inputs/results cover codec roots and economy's
 * decimal tick sizes; qualify UCRT range/matherr behavior before widening it. */
static int pow_call(CPU *c,const char *name) {
    if(strcmp(name,"pow"))return 0;
    uint64_t x=c->xmm[0].u64[0],y=c->xmm[1].u64[0];
    int zero_power=!(y&UINT64_C(0x7fffffffffffffff));
    unsigned flags=0;uint64_t result,ym=y&UINT64_C(0x7fffffffffffffff);
    if((c->mxcsr&UINT32_C(0xffff0000)) || (x && (x<UINT64_C(0x0010000000000000)||x>=UINT64_C(0x7ff0000000000000)))
       || (!zero_power && (ym<UINT64_C(0x0010000000000000)||ym>=UINT64_C(0x7ff0000000000000)))
       || (!x && !zero_power && (y>>63)))
        {if(c->mxcsr&UINT32_C(0xffff0000))pow_domain_stop(c,"pow reserved MXCSR bits");return portable_math_call(c,name);}
    if(zero_power)result=UINT64_C(0x3ff0000000000000);
    else if(!x)result=0;
    else if(x==UINT64_C(0x3ff0000000000000))result=x;
    else if(y==UINT64_C(0x3ff0000000000000))result=x;
    else {
        flags=32;
        if(flags&~((c->mxcsr>>7)&63u))pow_domain_stop(c,"unmasked guest floating-point exception is unsupported");
        result=finite_power64(c,x,y,(c->mxcsr>>13)&3u);
    }
    c->mxcsr|=flags;c->xmm[0].u64[0]=result;return 1;
}
static void codec_math_stop(CPU *c,const char *name,const char *why){
    fprintf(stderr,"MATH_INPUT name=%s x=0x%016llx mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        name,(unsigned long long)c->xmm[0].u64[0],c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);
}
static uint64_t finite_codec_math(CPU *c,const char *name,uint64_t bits,unsigned kind){
    int saved_errno=errno;uint64_t result;unsigned raised,rounding=(c->mxcsr>>13)&3u;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    double x,y;memcpy(&x,&bits,8);double(*volatile fn)(double)=kind==0?log:kind==1?cos:sin;y=fn(x);memcpy(&result,&y,8);
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(native_raised):"r"(control),"r"(status):"memory");
    raised=(unsigned)native_raised&15u;
#else
    raised=_mm_getcsr()&31u;_mm_setcsr(control);
#endif
    errno=saved_errno;
    if(raised)codec_math_stop(c,name,"unexpected native codec math domain/range exception");
    return result;
}
/* shortcut: MDCT init141C5DA10 uses log(float(N)) and log(2), followed
 * by cos/sin of nonnegative binary32 table angles widened to double. The
 * admitted nonzero angle lower bound preserves UCRT's precision contract.
 * Nonzero trig is nearest-only: Android Bionic directed modes disagree after
 * float narrowing. Other rounding modes/tiny angles need a measured port. */
static int codec_math_call(CPU *c,const char *name){
    unsigned kind;if(!strcmp(name,"log"))kind=0;else if(!strcmp(name,"cos"))kind=1;else if(!strcmp(name,"sin"))kind=2;else return 0;
    uint64_t x=c->xmm[0].u64[0],magnitude=x&UINT64_C(0x7fffffffffffffff),result;unsigned flags=0;
    int invalid=(c->mxcsr&UINT32_C(0xffff0000))!=0;
    if(!kind){
        unsigned exponent=(unsigned)(x>>52);
        invalid|=x<UINT64_C(0x3ff0000000000000)||x>UINT64_C(0x41e0000000000000)||(x&UINT64_C(0x1fffffff));
        if(!invalid)invalid=(x&((UINT64_C(1)<<(1075u-exponent))-1))!=0;
    }else if(magnitude){
        invalid|=(x>>63)!=0||magnitude<UINT64_C(0x3f20000000000000)||magnitude>UINT64_C(0x4010000000000000)||(x&UINT64_C(0x1fffffff));
    }
    if(kind&&magnitude&&(c->mxcsr&UINT32_C(0x6000)))invalid=1;
    if(invalid){if(c->mxcsr&UINT32_C(0xffff0000))codec_math_stop(c,name,"double codec math reserved MXCSR bits");return portable_math_call(c,name);}
    if(!kind&&x==UINT64_C(0x3ff0000000000000))result=((c->mxcsr>>13)&3u)==1u?UINT64_C(0x8000000000000000):0;
    else if(kind&&!magnitude)result=kind==1?UINT64_C(0x3ff0000000000000):x;
    else{
        flags=32;if(flags&~((c->mxcsr>>7)&63u))codec_math_stop(c,name,"unmasked guest floating-point exception is unsupported");
        result=finite_codec_math(c,name,x,kind);
    }
    c->mxcsr|=flags;c->xmm[0].u64[0]=result;return 1;
}
/* Original codec words decode signed21bit integer mantissas with a10bit
 * exponent minus788. Every nonzero scaled result is normal binary64;
 * exact exponent replacement avoids all native FP/errno side effects. */
static int ldexp_call(CPU *c,const char *name){
    if(strcmp(name,"ldexp"))return 0;
    uint64_t x=c->xmm[0].u64[0],magnitude=x&UINT64_C(0x7fffffffffffffff),result=x;
    uint32_t low=(uint32_t)c->rdx;int32_t scale;memcpy(&scale,&low,4);
    unsigned exponent=(unsigned)(magnitude>>52);
    int invalid=(c->mxcsr&UINT32_C(0xffff0000))||scale < -788||scale > 235;
    if(magnitude){
        invalid|=magnitude<UINT64_C(0x3ff0000000000000)||magnitude>UINT64_C(0x413fffff00000000);
        if(!invalid)invalid=(magnitude&((UINT64_C(1)<<(1075u-exponent))-1))!=0;
        if(!invalid)result=(x&UINT64_C(0x800fffffffffffff))|((uint64_t)((int32_t)exponent+scale)<<52);
    }
    if(invalid){
        if(!(c->mxcsr&UINT32_C(0xffff0000)))return portable_math_call(c,name);
        fprintf(stderr,"MATH_INPUT name=ldexp x=0x%016llx exponent=%d rdx=0x%016llx mxcsr=0x%08x guest_rip=0x%llx\n",
            (unsigned long long)x,scale,(unsigned long long)c->rdx,c->mxcsr,(unsigned long long)c->rip);
        stop("ldexp outside signed21bit integer mantissa/10bit codec exponent profile");
    }
    c->xmm[0].u64[0]=result;return 1;
}
/* Exact binary32 remainder for two normal positive magnitudes. At each
 * iteration the reduced significand is below2^24, so its shift fitsU32.
 * The final dyadic remainder is representable without rounding. */
static uint32_t fmodf_positive_bits(uint32_t x,uint32_t y){
    int exponent=(int)(x>>23),divisor_exponent=(int)(y>>23);
    uint32_t value=(x&UINT32_C(0x7fffff))|UINT32_C(0x800000);
    uint32_t divisor=(y&UINT32_C(0x7fffff))|UINT32_C(0x800000);
    while(exponent>divisor_exponent){
        if(value>=divisor)value-=divisor;
        if(!value)return 0;
        value<<=1;--exponent;
    }
    if(value>=divisor)value-=divisor;
    if(!value)return 0;
    while(!(value&UINT32_C(0x800000))){value<<=1;--exponent;}
    if(exponent>0)return((uint32_t)exponent<<23)|(value&UINT32_C(0x7fffff));
    return value>>(1-exponent);
}
/* Vector141AE1400 uses+1. Other original animation/angle callers use a
 * positive duration or360. Actual startup also reached modulo9. Positive
 * normal finite divisors use exact integer remainder; zero/negative/tiny
 * divisors, nonfinite dividends and unmasked intermediate exceptions stay
 * outside this bounded profile. Native FP state never changes. */
static int fmodf_call(CPU *c,const char *name){
    if(strcmp(name,"fmodf"))return 0;
    uint32_t x=c->xmm[0].u32[0],y=c->xmm[1].u32[0],magnitude=x&UINT32_C(0x7fffffff),sign=x&UINT32_C(0x80000000),result=x;
    unsigned exponent=magnitude>>23,flags=0;const char *why="fmodf outside finite dividend/positive normal finite divisor profile";
    if(c->mxcsr&UINT32_C(0xffff0000))goto reject;
    if(y<UINT32_C(0x00800000)||y>=UINT32_C(0x7f800000)||exponent==255u)return portable_math_call(c,name);
    /* UCRT may trap on an internal quotient conversion even though its
     * masked return discards precision status. No guest exception bridge. */
    if(magnitude>y&&!(c->mxcsr&4096u)){why="fmodf unmasked intermediate precision is unsupported";goto reject;}
    if(!exponent&&magnitude){
        if(c->mxcsr&64u)result=x; /* UCRT's DAZ early-return keeps raw bits. */
        else{flags=2;if(c->mxcsr&32768u){result=sign;flags|=48;}}
    }else if(magnitude>=y){
        if(y!=UINT32_C(0x3f800000))result=sign|fmodf_positive_bits(magnitude,y);
        else if(exponent>=150u)result=sign;
        else{
            uint32_t fraction=magnitude&((UINT32_C(1)<<(150u-exponent))-1u);
            if(!fraction)result=sign;
            else{while(!(fraction&UINT32_C(0x800000))){fraction<<=1;--exponent;}
                result=sign|(exponent<<23)|(fraction&UINT32_C(0x7fffff));}
        }
    }
    /* Exact cancellation and FTZ conversion have different zero signs. */
    if(!(result&UINT32_C(0x7fffffff))&&magnitude>y&&((c->mxcsr>>13)&3u)==1u)result^=UINT32_C(0x80000000);
    if(magnitude>=y&&(result&UINT32_C(0x7fffffff))&&(result&UINT32_C(0x7fffffff))<UINT32_C(0x00800000)&&(c->mxcsr&32768u)){result=sign;flags|=48;}
    if(flags&~((c->mxcsr>>7)&63u)){why="fmodf unmasked guest floating-point exception is unsupported";goto reject;}
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
reject:
    fprintf(stderr,"MATH_INPUT name=fmodf x=0x%08x y=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        x,y,c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);return 1;
}
static uint32_t finite_expf(uint32_t bits,unsigned rounding){
    int saved_errno=errno;float input,result;memcpy(&input,&bits,4);unsigned raised;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t nearest=native_control(control,0);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(nearest):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u);
#endif
    double(*volatile fn)(double)=exp;double wide=fn((double)input);
#if defined(__aarch64__)
    uint64_t conversion=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]\n\tmrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[out]"=&w"(result),[raised]"=&r"(native_raised)
        :[mode]"r"(conversion),[in]"w"(wide),[control]"r"(control),[status]"r"(status):"memory");
    raised=(unsigned)native_raised&15u;
#else
    unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
    result=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    raised=_mm_getcsr()&31u;_mm_setcsr(control);
#endif
    errno=saved_errno;memcpy(&bits,&result,4);
#if defined(__aarch64__)
    if(raised&~8u)stop("expf unexpected native non-underflow exception");
#else
    if(raised&~16u)stop("expf unexpected native non-underflow exception");
#endif
    if(bits>UINT32_C(0x3f800000)||(raised&&bits>=UINT32_C(0x00800000)))stop("expf native result/exception outside Gaussian weight range");
    return bits;
}
/* shortcut: Gaussian setup141012970 passes negative squared-distance weights;
 * default sigma1 gives inputs -0.5..-18. No universal setter bound exists.
 * Actual input0xc4e0ffff reaches the underflow tail. UCRT clamps to zero
 * from magnitude0x42cff1b5 in every RC, raising UF|PE without changing errno.
 * The preceding gradual band uses the same normal double exp and explicit
 * float conversion; guest FTZ is applied separately to its subnormal result. */
static int expf_call(CPU *c,const char *name){
    if(strcmp(name,"expf"))return 0;
    uint32_t x=c->xmm[0].u32[0],magnitude=x&UINT32_C(0x7fffffff),result=UINT32_C(0x3f800000);
    unsigned flags=0,rounding=(c->mxcsr>>13)&3u;const char *why="expf outside nonpositive Gaussian profile";
    if(c->mxcsr&UINT32_C(0xffff0000))goto reject;
    if(x==UINT32_C(0xff800000))result=0;
    else if(magnitude){
        if(!(x>>31)||magnitude>=UINT32_C(0x7f800000))return portable_math_call(c,name);
        if(magnitude>=UINT32_C(0x42cff1b5)){
            uint64_t handler=win64_math_error_handler();
            if(handler&&(!default_matherr_verified||handler!=UINT64_C(0x140d871c0))){why="expf underflow needs an unsupported guest matherr callback";goto reject;}
            result=0;flags=48;
        }
        else if(magnitude<UINT32_C(0x00800000)&&(c->mxcsr&64u))result=UINT32_C(0x3f800000);
        else{
            flags=32;if(magnitude<UINT32_C(0x00800000))flags|=2;
            if(flags&~((c->mxcsr>>7)&63u)){why="expf unmasked guest floating-point exception is unsupported";goto reject;}
            if(magnitude<=UINT32_C(0x33000000))result=(rounding==1||rounding==3)?UINT32_C(0x3f7fffff):UINT32_C(0x3f800000);
            else{
                result=finite_expf(x,rounding);
                if(result<UINT32_C(0x00800000)){flags|=16;if(c->mxcsr&32768u)result=0;}
            }
        }
    }
    if(flags&~((c->mxcsr>>7)&63u)){why="expf unmasked guest floating-point exception is unsupported";goto reject;}
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
reject:
    fprintf(stderr,"MATH_INPUT name=expf x=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        x,c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);return 1;
}
/* Typed platform logf is used only for positive normal finite inputs. The
 * native thread control/status and errno are private to the calling host. */
static uint32_t finite_logf(uint32_t bits,unsigned rounding,int decimal){
    int saved_errno=errno;uint32_t result;unsigned raised;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|(rounding<<13));
#endif
    float x,y;memcpy(&x,&bits,4);float(*volatile fn)(float)=decimal?log10f:logf;y=fn(x);memcpy(&result,&y,4);
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(native_raised):"r"(control),"r"(status):"memory");
    raised=(unsigned)native_raised&15u;
#else
    raised=_mm_getcsr()&31u;_mm_setcsr(control);
#endif
    errno=saved_errno;
    if(raised)stop("unexpected native positive-normal logf exception");
    return result;
}
/* shortcut: the observed clamp/sqrt consumer needs signed zero and positive
 * normal finite values; another original call proves the constant2. Extend
 * negative/subnormal/NaN/infinity domains only from their own runtime proof. */
static int logf_call(CPU*c,const char*name){
    int decimal=!strcmp(name,"log10f");
    if(!decimal&&strcmp(name,"logf"))return 0;
    uint32_t bits=c->xmm[0].u32[0],magnitude=bits&UINT32_C(0x7fffffff);
    unsigned flags,rounding=(c->mxcsr>>13)&3u;
    if(!magnitude){
        if(custom_matherr())stop("guest matherr callback required for logf pole");
        bits=UINT32_C(0xff800000);flags=4;win64_stdio_set_errno(34);
    }else{
        if(bits<UINT32_C(0x00800000)||bits>=UINT32_C(0x7f800000))
            return portable_math_call(c,name);
        flags=bits==UINT32_C(0x3f800000)?0u:32u;
        if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
        bits=flags?finite_logf(bits,rounding,decimal):(rounding==1?UINT32_C(0x80000000):0u);
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;c->xmm[0].u32[0]=bits;return 1;
}
/* Small sine/cosine inputs use the same elementary double cubic/quadratic as
 * the measured default UCRT path. This preserves its incidental exact/inexact
 * status (cos of some powers of two is exact in that approximation). General
 * finite inputs use typed platform libm with a measured, nonzero ULP bound. */
static uint32_t finite_trig(uint32_t bits,int cosine,unsigned rounding,unsigned *flags){
    int saved_errno=errno;float input,result;memcpy(&input,&bits,4);
    int small=cosine!=2&&(bits&UINT32_C(0x7fffffff))<UINT32_C(0x3c000000);
    unsigned raised;
#if defined(__aarch64__)
    uint64_t control,status,native_raised;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,cosine==2?0u:rounding);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
    if(cosine==2){
        /* Native float tan reduction assumes nearest; round a double result
         * to binary32 separately so directed guest modes remain correct. */
        double(*volatile fn)(double)=tan;double wide=fn((double)input);
        uint64_t conversion=native_control(control,rounding);
        __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]"
            :[out]"=&w"(result):[mode]"r"(conversion),[in]"w"(wide):"memory");
    }else if(small){
        double x,a,b,y;
        if(cosine){
            double half=0.5,one=1.0;
            __asm__ volatile("fcvt %d[x],%s[in]\n\tfmul %d[a],%d[x],%d[half]\n\tfneg %d[a],%d[a]\n\tfmadd %d[y],%d[a],%d[x],%d[one]\n\tfcvt %s[out],%d[y]"
                :[out]"=&w"(result),[x]"=&w"(x),[a]"=&w"(a),[y]"=&w"(y)
                :[in]"w"(input),[half]"w"(half),[one]"w"(one):"memory");
        }else{
            double sixth=1.0/6.0;
            __asm__ volatile("fcvt %d[x],%s[in]\n\tfmul %d[a],%d[x],%d[x]\n\tfmul %d[b],%d[a],%d[x]\n\tfneg %d[b],%d[b]\n\tfmadd %d[y],%d[b],%d[sixth],%d[x]\n\tfcvt %s[out],%d[y]"
                :[out]"=&w"(result),[x]"=&w"(x),[a]"=&w"(a),[b]"=&w"(b),[y]"=&w"(y)
                :[in]"w"(input),[sixth]"w"(sixth):"memory");
        }
    }else{float(*volatile fn)(float)=cosine?cosf:sinf;result=fn(input);}
    __asm__ volatile("mrs %0,fpsr\n\tmsr fpcr,%1\n\tmsr fpsr,%2":"=&r"(native_raised):"r"(control),"r"(status):"memory");
    raised=((native_raised&1u)?1u:0u)|((native_raised&2u)?4u:0u)|((native_raised&4u)?8u:0u)|((native_raised&8u)?16u:0u)|((native_raised&16u)?32u:0u);
#elif defined(_M_X64) || defined(__x86_64__)
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|((cosine==2?0u:rounding)<<13));
    if(cosine==2){
        double(*volatile fn)(double)=tan;double wide=fn((double)input);
        unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
        result=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    }else if(small){
        volatile double x=(double)input;
        double(*volatile fused)(double,double,double)=fma;
        if(cosine){volatile double half=x*0.5;result=(float)fused(-half,x,1.0);}
        else{volatile double square=x*x,cube=square*x;result=(float)fused(-cube,1.0/6.0,x);}
    }else{float(*volatile fn)(float)=cosine?cosf:sinf;result=fn(input);}
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#else
#error Win64 trig requires the tested ARM64 or native x64 math path.
#endif
    memcpy(&bits,&result,4);errno=saved_errno;
    if(small)*flags|=raised;
    else{
        if(raised&31u)stop("unexpected native finite trig exception");
        *flags|=32u;
    }
    return bits;
}
static int trig_call(CPU *c,const char *name){
    int cosine;if(!strcmp(name,"sinf"))cosine=0;else if(!strcmp(name,"cosf"))cosine=1;else if(!strcmp(name,"tanf"))cosine=2;else return 0;
    uint32_t bits=c->xmm[0].u32[0],magnitude=bits&UINT32_C(0x7fffffff);
    unsigned flags=0,rounding=(c->mxcsr>>13)&3u;
    if(magnitude>UINT32_C(0x7f800000))bits|=UINT32_C(0x00400000);
    else if(magnitude==UINT32_C(0x7f800000)){
        if(custom_matherr())stop("guest matherr callback required for trig infinity");
        bits=UINT32_C(0xffc00000);flags=1;win64_stdio_set_errno(33);
    }else if(magnitude<UINT32_C(0x39000000)){
        /* UCRT returns the original sine input despite DAZ/FTZ. Discarded
         * float arithmetic raises status: sin x*(1-2^-24)+2^32, cos x+2^32.
         * Model those flags without letting ARM's joint FZ alter the result. */
        if(magnitude&&(!(c->mxcsr&64u)||magnitude>=UINT32_C(0x00800000))){
            flags=32;
            if(magnitude<UINT32_C(0x00800000))flags|=cosine==1?2u:18u;
            else if(cosine!=1&&magnitude==UINT32_C(0x00800000)){
                flags|=16u;
                int smaller=rounding==3||(rounding==1&&!(bits>>31))||(rounding==2&&(bits>>31));
                if(smaller&&!(c->mxcsr&(64u|32768u)))flags|=2u;
            }
        }
        /* Native UCRT tanf returns x below2^-27. In [2^-27,2^-13),
         * tan(x)-x is below half a binary32 ULP; its directed outward result
         * is the next float. The measured tiny status policy matches sine,
         * including min-normal and DAZ/FTZ behavior, without native FP work. */
        if(cosine==2&&magnitude>=UINT32_C(0x32000000)&&
           (rounding==((bits>>31)?1u:2u)))++bits;
        if(cosine==1)bits=UINT32_C(0x3f800000);
    }else bits=finite_trig(bits,cosine,rounding,&flags);
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;c->xmm[0].u32[0]=bits;return 1;
}
static void angles_stop(CPU *c,const char *name,const char *why){
    fprintf(stderr,"MATH_INPUT name=%s y=0x%08x x=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        name,c->xmm[0].u32[0],c->xmm[1].u32[0],c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);
}
static int angle_exponent(uint32_t bits){
    unsigned e=(bits>>23)&255u;if(e)return(int)e-127;
    int exponent=-126;uint32_t fraction=bits&UINT32_C(0x7fffff);
    while(!(fraction&UINT32_C(0x800000))){fraction<<=1;--exponent;}return exponent;
}
static uint64_t angle_double(uint32_t bits){
    uint64_t sign=(uint64_t)(bits&UINT32_C(0x80000000))<<32;unsigned e=(bits>>23)&255u;
    uint32_t fraction=bits&UINT32_C(0x7fffff);int exponent=(int)e+896;
    if(!e){if(!fraction)return sign;exponent=897;while(!(fraction&UINT32_C(0x800000))){fraction<<=1;--exponent;}}
    return sign|((uint64_t)exponent<<52)|((uint64_t)(fraction&UINT32_C(0x7fffff))<<29);
}
static uint32_t finite_angles(uint32_t ybits,uint32_t xbits,unsigned kind,unsigned rounding,unsigned *flags){
    uint64_t yraw=angle_double(ybits),xraw=angle_double(xbits);double y,x,wide;float output;uint32_t result;unsigned raised;int saved_errno=errno;
    memcpy(&y,&yraw,8);memcpy(&x,&xraw,8);
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,kind==2?rounding:0u);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
    if(kind==2)__asm__ volatile("fdiv %d[out],%d[y],%d[x]":[out]"=&w"(wide):[y]"w"(y),[x]"w"(x):"memory");
    else if(kind){double(*volatile fn)(double)=asin;wide=fn(y);}
    else{double(*volatile fn)(double,double)=atan2;wide=fn(y,x);}
    uint64_t conversion=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]\n\tmrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[out]"=&w"(output),[raised]"=&r"(actual):[mode]"r"(conversion),[in]"w"(wide),[control]"r"(control),[status]"r"(status):"memory");
    raised=((actual&1u)?1u:0u)|((actual&2u)?4u:0u)|((actual&4u)?8u:0u)|((actual&8u)?16u:0u)|((actual&16u)?32u:0u);
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u|((kind==2?rounding:0u)<<13));
    if(kind==2)wide=_mm_cvtsd_f64(_mm_div_sd(_mm_set_sd(y),_mm_set_sd(x)));
    else if(kind){double(*volatile fn)(double)=asin;wide=fn(y);}
    else{double(*volatile fn)(double,double)=atan2;wide=fn(y,x);}
    unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
    output=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;memcpy(&result,&output,4);
    if(raised&31u)stop("unexpected native matrix-angle domain/range exception");
    *flags|=kind==2?raised:32u;return result;
}
/* shortcut: the actual140E33AF8 Euler conversion and following140E33B0B
 * need finite matrix angles. Typed double libm plus explicit guest float
 * rounding has a measured ULP bound; original UCRT's tiny-ratio path uses
 * only IEEE double division/conversion. Subnormal atan2 results and custom
 * matherr handlers remain outside this profile; no guest exception bridge. */
static int angles_call(CPU *c,const char *name){
    int asin_kind;if(!strcmp(name,"atan2f"))asin_kind=0;else if(!strcmp(name,"asinf"))asin_kind=1;else return 0;
    if(c->mxcsr&UINT32_C(0xffff0000))angles_stop(c,name,"reserved matrix-angle MXCSR bits");
    uint32_t y=c->xmm[0].u32[0],x=c->xmm[1].u32[0],ay=y&UINT32_C(0x7fffffff),ax=x&UINT32_C(0x7fffffff),result;
    unsigned flags=0,rounding=(c->mxcsr>>13)&3u;int domain=0;
    if(asin_kind){
        if(ay>UINT32_C(0x7f800000))result=y|UINT32_C(0x00400000);
        else if(ay>UINT32_C(0x3f800000)){
            uint64_t handler=win64_math_error_handler();
            if(handler&&(!default_matherr_verified||handler!=UINT64_C(0x140d871c0)))angles_stop(c,name,"asinf domain needs unsupported guest matherr callback");
            result=UINT32_C(0xffc00000);flags=1;domain=1;
        }else{
            flags=32;
            if(ay<UINT32_C(0x38800000))result=y;
            else if(ay==UINT32_C(0x3f800000))result=(y&UINT32_C(0x80000000))|UINT32_C(0x3fc90fdb);
            else result=finite_angles(y,0,1,rounding,&flags);
        }
    }else{
        if((ay&&ay<UINT32_C(0x00800000))||(ax&&ax<UINT32_C(0x00800000))){
            if(c->mxcsr&64u){if(ay<UINT32_C(0x00800000))y&=UINT32_C(0x80000000);if(ax<UINT32_C(0x00800000))x&=UINT32_C(0x80000000);}
            else flags|=2;
        }
        ay=y&UINT32_C(0x7fffffff);ax=x&UINT32_C(0x7fffffff);
        int ynan=ay>UINT32_C(0x7f800000),xnan=ax>UINT32_C(0x7f800000);uint32_t sign=y&UINT32_C(0x80000000);
        if(ynan||xnan){
            if((ynan&&!(y&UINT32_C(0x00400000)))||(xnan&&!(x&UINT32_C(0x00400000))))flags|=1;
            result=(xnan?x:y)|UINT32_C(0x00400000);
        }else if(!ay){result=sign;if(x>>31){result|=UINT32_C(0x40490fdb);flags|=32;}}
        else if(!ax){result=sign|UINT32_C(0x3fc90fdb);flags|=32;}
        else if(ay==UINT32_C(0x7f800000)){
            result=sign|(ax==UINT32_C(0x7f800000)?((x>>31)?UINT32_C(0x4016cbe4):UINT32_C(0x3f490fdb)):UINT32_C(0x3fc90fdb));flags|=32;
        }else if(ax==UINT32_C(0x7f800000)){result=sign;if(x>>31){result|=UINT32_C(0x40490fdb);flags|=32;}else flags|=48;}
        else{
            int difference=angle_exponent(ay)-angle_exponent(ax);
            if(difference>26){result=sign|UINT32_C(0x3fc90fdb);flags|=32;}
            else if((x>>31)&&difference < -26){result=sign|UINT32_C(0x40490fdb);flags|=32;}
            else if(!(x>>31)&&difference < -13){
                if(difference < -125)return portable_math_call(c,name);
                result=finite_angles(y,x,2,rounding,&flags);
            }else result=finite_angles(y,x,0,rounding,&flags);
        }
    }
    if(flags&~((c->mxcsr>>7)&63u))angles_stop(c,name,"unmasked guest matrix-angle exception is unsupported");
    if(domain)win64_stdio_set_errno(33);
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
}
/* Isolated candidate only: finite normalized-vector/quaternion acosf.
 * The original quaternion path bounds finite input below0.9990000129;
 * the normalized-vector path has no final clamp, so UCRT domain/NaN
 * behavior is retained. Raised unmasked exceptions and arbitrary guest
 * matherr callbacks still need the guest exception/callback bridge. */
static uint32_t finite_acosf_candidate(uint32_t input,unsigned rounding){
    uint64_t raw=angle_double(input);double x,wide;float output;uint32_t result;unsigned raised;int saved_errno=errno;
    memcpy(&x,&raw,8);
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,0u);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
    double(*volatile fn)(double)=acos;wide=fn(x);
    uint64_t conversion=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]\n\tmrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[out]"=&w"(output),[raised]"=&r"(actual):[mode]"r"(conversion),[in]"w"(wide),[control]"r"(control),[status]"r"(status):"memory");
    raised=((actual&1u)?1u:0u)|((actual&2u)?4u:0u)|((actual&4u)?8u:0u)|((actual&8u)?16u:0u)|((actual&16u)?32u:0u);
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u);
    double(*volatile fn)(double)=acos;wide=fn(x);
    unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
    output=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;memcpy(&result,&output,4);
    if(raised&31u)stop("unexpected native normalized-vector acosf exception");
    return result;
}
static int acosf_candidate_call(CPU *c,const char *name){
    if(strcmp(name,"acosf"))return 0;
    if(c->mxcsr&UINT32_C(0xffff0000))angles_stop(c,name,"reserved normalized-vector acosf MXCSR bits");
    uint32_t input=c->xmm[0].u32[0],magnitude=input&UINT32_C(0x7fffffff),result;unsigned flags=0;int domain=0;
    if(magnitude>UINT32_C(0x7f800000))result=input|UINT32_C(0x00400000);
    else if(magnitude>UINT32_C(0x3f800000)){
        uint64_t handler=win64_math_error_handler();
        if(handler&&(!default_matherr_verified||handler!=UINT64_C(0x140d871c0)))angles_stop(c,name,"acosf domain needs unsupported guest matherr callback");
        result=UINT32_C(0xffc00000);flags=1;domain=1;
    }else if(input==UINT32_C(0x3f800000))result=0;
    else{
        flags=32;
        if(magnitude<UINT32_C(0x32800000))result=UINT32_C(0x3fc90fdb);
        else if(input==UINT32_C(0xbf800000))result=UINT32_C(0x40490fdb);
        else result=finite_acosf_candidate(input,(c->mxcsr>>13)&3u);
    }
    if(flags&~((c->mxcsr>>7)&63u))angles_stop(c,name,"unmasked guest normalized-vector acosf exception is unsupported");
    if(domain)win64_stdio_set_errno(33);
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
}
/* Actual14160C9A5 reaches this through1416089D0: atan(1/radius), with a
 * positive-radius assertion in frostpunkautomatonmovementsystem.cpp.
 * The getter's empty fallback0 can produce+Inf. No static finite upper
 * bound is assumed. Nonzero |x|<2^-125 stays outside this normal-result
 * profile because UCRT's directed/subnormal UF behavior needs separate
 * qualification. Typed double atan plus guest float rounding has a measured
 * ULP bound; it does not claim general libm bit identity. */
static uint32_t finite_atanf_normal(uint32_t input,unsigned rounding){
    uint64_t raw=angle_double(input);double x,wide;float output;uint32_t result;unsigned raised;int saved_errno=errno;
    memcpy(&x,&raw,8);
#if defined(__aarch64__)
    uint64_t control,status,actual;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    uint64_t desired=native_control(control,0u);
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,xzr"::"r"(desired):"memory");
    double(*volatile fn)(double)=atan;wide=fn(x);
    uint64_t conversion=native_control(control,rounding);
    __asm__ volatile("msr fpcr,%[mode]\n\tfcvt %s[out],%d[in]\n\tmrs %[raised],fpsr\n\tmsr fpcr,%[control]\n\tmsr fpsr,%[status]"
        :[out]"=&w"(output),[raised]"=&r"(actual):[mode]"r"(conversion),[in]"w"(wide),[control]"r"(control),[status]"r"(status):"memory");
    raised=((actual&1u)?1u:0u)|((actual&2u)?4u:0u)|((actual&4u)?8u:0u)|((actual&8u)?16u:0u)|((actual&16u)?32u:0u);
#else
    unsigned control=_mm_getcsr();_mm_setcsr(0x1f80u);
    double(*volatile fn)(double)=atan;wide=fn(x);
    unsigned status=_mm_getcsr()&63u;_mm_setcsr(0x1f80u|(rounding<<13)|status);
    output=_mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(),_mm_set_sd(wide)));
    raised=_mm_getcsr()&63u;_mm_setcsr(control);
#endif
    errno=saved_errno;memcpy(&result,&output,4);
    if(raised&31u)stop("unexpected native normal-result atanf exception");
    return result;
}
static int atanf_call(CPU *c,const char *name){
    if(strcmp(name,"atanf"))return 0;
    uint32_t input=c->xmm[0].u32[0],magnitude=input&UINT32_C(0x7fffffff),result=input;unsigned flags=0;
    const char *why="atanf outside normal-result reciprocal-radius profile";
    if(c->mxcsr&UINT32_C(0xffff0000))goto reject;
    if(magnitude>UINT32_C(0x7f800000)){
        if(!(input&UINT32_C(0x00400000)))flags=1;
        result=input|UINT32_C(0x00400000);
    }else if(magnitude==UINT32_C(0x7f800000)){
        result=(input&UINT32_C(0x80000000))|UINT32_C(0x3fc90fdb);flags=32;
    }else if(magnitude){
        if(magnitude<UINT32_C(0x01000000))return portable_math_call(c,name);
        result=finite_atanf_normal(input,(c->mxcsr>>13)&3u);flags=32;
    }
    if(flags&~((c->mxcsr>>7)&63u)){why="atanf unmasked guest floating-point exception is unsupported";goto reject;}
    c->mxcsr|=flags;c->xmm[0].u32[0]=result;return 1;
reject:
    fprintf(stderr,"MATH_INPUT name=atanf x=0x%08x mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        input,c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);stop(why);return 1;
}

#include "win64.h"
/* Original14189FD4B passes its local float output in RDX, not RCX. UCRT
 * splits binary32 by truncation without DAZ/FTZ flushing. Exact cancellation
 * for integral |x| in[1,2^23) additionally returns -0 in RC down. Preserve its
 * unquieted NaN in the integral output and quiet only the returned fraction.
 * Android profile shortcut: unmasked sNaN invalid exceptions still STOP;
 * an actual guest FP exception bridge is required before admitting those. */
static int modff_call(CPU *c,const char *name){
    if(strcmp(name,"modff"))return 0;
    uint32_t input=c->xmm[0].u32[0],m=input&UINT32_C(0x7fffffff);
    uint32_t sign=input&UINT32_C(0x80000000),fraction=input,integral=input;
    unsigned flags=0;const char *why="modff reserved MXCSR bits are unsupported";
    if(c->mxcsr&~UINT32_C(0xffff))goto reject;
    if(m>UINT32_C(0x7f800000)){
        fraction=input|UINT32_C(0x00400000);
        if(!(input&UINT32_C(0x00400000)))flags=1;
    }else if(m>=UINT32_C(0x4b000000))fraction=sign;
    else if(m<UINT32_C(0x3f800000))integral=sign;
    else{
        unsigned exponent=m>>23,shift=150u-exponent;
        uint32_t mask=(UINT32_C(1)<<shift)-1u,tail=m&mask;
        integral=input&~mask;fraction=sign;
        if(tail){
            while(!(tail&UINT32_C(0x00800000))){tail<<=1;--exponent;}
            fraction=sign|(exponent<<23)|(tail&UINT32_C(0x007fffff));
        }else if(((c->mxcsr>>13)&3u)==1u)fraction|=UINT32_C(0x80000000);
    }
    if(flags&~((c->mxcsr>>7)&63u)){
        why="modff unmasked guest invalid exception is unsupported";goto reject;
    }
    if(!c->rdx){why="modff NULL integral output is unsupported";goto reject;}
    /* Resolve the complete original four-byte store before committing CPU. */
    int saved_errno=errno;void *output=win64_pointer(c->rdx,4,1);
    memcpy(output,&integral,4);c->mxcsr|=flags;c->xmm[0].u32[0]=fraction;errno=saved_errno;return 1;
reject:
    fprintf(stderr,"MATH_INPUT name=modff x=0x%08x output=0x%llx mxcsr=0x%08x guest_rip=0x%llx rsp=0x%llx\n",
        input,(unsigned long long)c->rdx,c->mxcsr,(unsigned long long)c->rip,(unsigned long long)c->rsp);
    stop(why);return 0;
}

static uint64_t integer_round(uint64_t bits,int wide,unsigned mode,unsigned daz,unsigned*flags){
    unsigned fraction=wide?52:23,bias=wide?1023:127;uint64_t sign=UINT64_C(1)<<(wide?63:31);
    uint64_t magnitude=bits&~sign,exponent=wide?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000),quiet=UINT64_C(1)<<(fraction-1);
    if((magnitude&exponent)==exponent){
        if((magnitude&~exponent)&&mode!=2){if(!(magnitude&quiet))*flags|=1;bits|=quiet;}return bits;
    }
    if(!magnitude)return bits;
    int e=(int)(magnitude>>fraction)-(int)bias;
    if(e>=(int)fraction)return bits;
    if(e<0){
        if(!(magnitude&exponent)&&daz&&mode!=2)return bits&sign;
        int one=mode==0?(bits&sign)!=0:mode==1?(bits&sign)==0:e==-1;
        return(bits&sign)|(one?((uint64_t)bias<<fraction):0);
    }
    unsigned shift=fraction-(unsigned)e;uint64_t mask=(UINT64_C(1)<<shift)-1,tail=magnitude&mask;
    if(!tail)return bits;uint64_t rounded=magnitude&~mask;
    if((mode==0&&(bits&sign))||(mode==1&&!(bits&sign))||(mode==2&&tail>=(UINT64_C(1)<<(shift-1))))rounded+=UINT64_C(1)<<shift;
    return(bits&sign)|rounded;
}
static int family_kind(CPU*c,int kind,int wide);
static int family_call(CPU*c,const char*name){
    int kind=-1,wide=1;
    if(!strcmp(name,"log2l"))kind=0;
    else if(!strcmp(name,"log2"))kind=1;
    else if(!strcmp(name,"log2f")){kind=2;wide=0;}
    else if(!strcmp(name,"floor"))kind=3;
    else if(!strcmp(name,"floorf")){kind=3;wide=0;}
    else if(!strcmp(name,"ceil"))kind=4;
    else if(!strcmp(name,"ceilf")){kind=4;wide=0;}
    else if(!strcmp(name,"round"))kind=5;
    else if(!strcmp(name,"roundf")){kind=5;wide=0;}
    if(kind<0)return 0;
    return family_kind(c,kind,wide);
}
/* family_call after the name: log2l/log2/log2f (kind 0-2), floor (3), ceil (4), round (5). */
static int family_kind(CPU*c,int kind,int wide){
    uint64_t bits=wide?c->xmm[0].u64[0]:c->xmm[0].u32[0];unsigned flags=0;
    if(kind>=3)bits=integer_round(bits,wide,(unsigned)kind-3,(c->mxcsr>>6)&1u,&flags);
    else{
        unsigned fraction=wide?52:23,bias=wide?1023:127;
        uint64_t sign=UINT64_C(1)<<(wide?63:31),exponent=wide?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000),quiet=UINT64_C(1)<<(fraction-1);
        uint64_t magnitude=bits&~sign,mantissa=magnitude&~exponent;
        int subnormal=!(magnitude&exponent)&&magnitude;
        if((magnitude&exponent)==exponent&&mantissa){
            /* Win64 UCRT log2l returns NaNs unchanged, unlike log2/log2f. */
            if(kind){if(kind==2&&!(mantissa&quiet))flags|=1;bits|=quiet;}
        }else if(!magnitude||(bits&sign)){
            if(kind==1&&custom_matherr())stop("guest matherr callback required for log2 domain/pole error");
            if(!magnitude){bits=sign|exponent;flags=4;win64_stdio_set_errno(34);}
            else{bits=exponent|quiet|(kind?sign:0);flags=1;win64_stdio_set_errno(33);if(kind==2&&subnormal&&!(c->mxcsr&64u))flags|=2;}
        }else if(magnitude!=exponent){
            if(kind==2&&subnormal&&(c->mxcsr&64u))bits=UINT64_C(0xc3160000); /* actual UCRT float DAZ path: -150 */
            else{
                if(kind==2&&subnormal)flags|=2;
                uint64_t significant=mantissa;int e=(int)(magnitude>>fraction)-(int)bias;
                if(!subnormal)significant|=UINT64_C(1)<<fraction;
                else{e=1-(int)bias;while(!(significant&(UINT64_C(1)<<fraction))){significant<<=1;--e;}}
                if(!(significant&((UINT64_C(1)<<fraction)-1))){
                    if(wide){double x=(double)e;memcpy(&bits,&x,8);}else{float x=(float)e;uint32_t low;memcpy(&low,&x,4);bits=low;}
                    if(wide&&!e&&((c->mxcsr>>13)&3u)==1u)bits=sign;
                }else{bits=positive_log2(bits,wide,(c->mxcsr>>13)&3u);flags|=32;}
            }
        }
    }
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;if(wide)c->xmm[0].u64[0]=bits;else c->xmm[0].u32[0]=(uint32_t)bits;return 1;
}
static int fdtest_call(CPU *c,const char *name){
    unsigned kind;
    if(!strcmp(name,"_fdtest"))kind=0;else if(!strcmp(name,"_fdclass"))kind=1;else if(!strcmp(name,"_dclass"))kind=2;
    else if(!strcmp(name,"_fdsign"))kind=3;else if(!strcmp(name,"_dsign"))kind=4;else return 0;
    int saved_errno=errno;uint64_t bits=0;
    if(kind)bits=(kind==2||kind==4)?c->xmm[0].u64[0]:c->xmm[0].u32[0];else memcpy(&bits,win64_pointer(c->rcx,4,0),4);
    if(kind>=3){SET32(c->rax,bits>>(kind==4?63:31));return 1;}
    uint64_t magnitude=bits&(kind==2?UINT64_C(0x7fffffffffffffff):UINT64_C(0x7fffffff));
    uint64_t exponent=magnitude&(kind==2?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000));
    /* UCRT classifies the pointed-to bits without quieting NaNs or applying DAZ. */
    int result=exponent==(kind==2?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000))?(magnitude>exponent?2:1):exponent?-1:magnitude?-2:0;
    SET32(c->rax,result);errno=saved_errno;return 1;
}
/* The handler chain of win64_math_call, in its order. Import names are immutable pointers
 * (recomp_imports), so the handler that accepted a name is memoized per pointer: the same
 * handler then runs with the same name, without the strcmp walk through the earlier ones. */
typedef int (*math_handler)(CPU *,const char *);
#include "win64_math_extended.inc"
#include "win64_math_portable.inc"
static const math_handler math_chain[]={trig_call,family_call,exp2_call,exp2f_call,expf_call,powf_call,pow_call,
    codec_math_call,ldexp_call,fmodf_call,logf_call,angles_call,acosf_candidate_call,atanf_call,modff_call,fdtest_call,extended_math_call};
#define MATH_MEMO_SLOTS 128u
static struct { _Atomic(const char *) name; _Atomic int handler; } math_memo[MATH_MEMO_SLOTS];
static unsigned math_memo_slot(const char *name){
    return (unsigned)(((uintptr_t)name*UINT64_C(0x9E3779B97F4A7C15))>>57)&(MATH_MEMO_SLOTS-1u);
}
int win64_math_fast_powf(CPU *c){return powf_call(c,"powf");}
/* Direct handlers for win64.c's import cache: the chain handler that accepts the name (each
 * handler of math_chain declines only a name it does not own), called with that name.  The import
 * then skips the dispatcher, the provider chain and the memo; floorf, sqrtf, fmodf, atan2f and the
 * like are called hundreds of times per frame.  A handler that declines would be a wrong table row,
 * which must not pass silently. */
static int root_call(CPU*c,int wide);
static _Noreturn void fast_declined(const char *name){fprintf(stderr,"STOP Win64 math fast handler declined %s\n",name);fflush(stderr);_Exit(73);}
#define MATH_FAST(id,handler,text) static int fast_##id(CPU *c){if(!handler(c,text))fast_declined(text);return 1;}
static int fast_floorf(CPU *c){return family_kind(c,3,0);}
static int fast_floor(CPU *c){return family_kind(c,3,1);}
static int fast_ceilf(CPU *c){return family_kind(c,4,0);}
static int fast_ceil(CPU *c){return family_kind(c,4,1);}
static int fast_roundf(CPU *c){return family_kind(c,5,0);}
static int fast_round(CPU *c){return family_kind(c,5,1);}
MATH_FAST(sinf,trig_call,"sinf")
MATH_FAST(cosf,trig_call,"cosf")
MATH_FAST(tanf,trig_call,"tanf")
MATH_FAST(atan2f,angles_call,"atan2f")
MATH_FAST(asinf,angles_call,"asinf")
MATH_FAST(acosf,acosf_candidate_call,"acosf")
MATH_FAST(atanf,atanf_call,"atanf")
MATH_FAST(fmodf,fmodf_call,"fmodf")
MATH_FAST(expf,expf_call,"expf")
MATH_FAST(logf,logf_call,"logf")
MATH_FAST(log10f,logf_call,"log10f")
MATH_FAST(exp2f,exp2f_call,"exp2f")
MATH_FAST(modff,modff_call,"modff")
MATH_FAST(tanhf,extended_math_call,"tanhf")
MATH_FAST(sinhf,extended_math_call,"sinhf")
MATH_FAST(coshf,extended_math_call,"coshf")
#undef MATH_FAST
/* sqrtf/sqrt are not chain handlers: win64_math_call's own branch. */
static int fast_sqrtf(CPU *c){return root_call(c,0);}
static int fast_sqrt(CPU *c){return root_call(c,1);}
int (*win64_math_fast_import(const char *name))(CPU *){
    static const struct {const char *name;int (*handler)(CPU *);} table[]={
        {"floorf",fast_floorf},{"floor",fast_floor},{"ceilf",fast_ceilf},{"ceil",fast_ceil},{"roundf",fast_roundf},
        {"round",fast_round},{"sinf",fast_sinf},{"cosf",fast_cosf},{"tanf",fast_tanf},{"atan2f",fast_atan2f},
        {"asinf",fast_asinf},{"acosf",fast_acosf},{"atanf",fast_atanf},{"fmodf",fast_fmodf},{"expf",fast_expf},
        {"logf",fast_logf},{"log10f",fast_log10f},{"exp2f",fast_exp2f},{"modff",fast_modff},{"tanhf",fast_tanhf},
        {"sinhf",fast_sinhf},{"coshf",fast_coshf},{"sqrtf",fast_sqrtf},{"sqrt",fast_sqrt}};
    for(size_t i=0;i<sizeof(table)/sizeof(*table);++i)if(!strcmp(name,table[i].name))return table[i].handler;
    return NULL;
}
static int root_call(CPU*c,int wide);
int win64_math_call(CPU*c,const char*name){
    {
        unsigned slot=math_memo_slot(name);
        if(atomic_load_explicit(&math_memo[slot].name,memory_order_acquire)==name){
            int handler=atomic_load_explicit(&math_memo[slot].handler,memory_order_relaxed);
            if(handler>=0&&handler<(int)(sizeof(math_chain)/sizeof(*math_chain))&&math_chain[handler](c,name))return 1;
        }
    }
    if(!strcmp(name,"nan")){
        /* UCRT ignores the tag pointer; its MOVSD load also clears XMM0's high half. */
        c->xmm[0].u64[0]=UINT64_C(0x7ff8000000000000);c->xmm[0].u64[1]=0;return 1;
    }
    int wide;if(!strcmp(name,"sqrtf"))wide=0;else if(!strcmp(name,"sqrt"))wide=1;else{
        for(int handler=0;handler<(int)(sizeof(math_chain)/sizeof(*math_chain));++handler){
            if(!math_chain[handler](c,name))continue;
            unsigned slot=math_memo_slot(name);
            atomic_store_explicit(&math_memo[slot].name,NULL,memory_order_relaxed);
            atomic_store_explicit(&math_memo[slot].handler,handler,memory_order_relaxed);
            atomic_store_explicit(&math_memo[slot].name,name,memory_order_release);
            return 1;
        }
        return 0;
    }
    return root_call(c,wide);
}
/* win64_math_call's sqrtf (wide 0) / sqrt (wide 1) branch. */
static int root_call(CPU*c,int wide){
    uint64_t bits=wide?c->xmm[0].u64[0]:c->xmm[0].u32[0];
    const uint64_t sign=wide?UINT64_C(0x8000000000000000):UINT64_C(0x80000000);
    const uint64_t exponent=wide?UINT64_C(0x7ff0000000000000):UINT64_C(0x7f800000);
    const uint64_t fraction=wide?UINT64_C(0x000fffffffffffff):UINT64_C(0x007fffff);
    const uint64_t quiet=wide?UINT64_C(0x0008000000000000):UINT64_C(0x00400000);
    const uint64_t magnitude=bits&~sign;unsigned flags=0;
    if((magnitude&exponent)==exponent&&(magnitude&fraction)){
        /* UCRT explicitly quiets signaling NaNs without raising invalid. */
        bits|=quiet;
    }else if((bits&sign)&&magnitude){
        if(custom_matherr())stop("guest matherr callback required for sqrt domain error");
        bits=sign|exponent|quiet;flags=1;win64_stdio_set_errno(33);
    }else if(magnitude&&magnitude!=exponent){
        int subnormal=!(magnitude&exponent);
        if(subnormal&&(c->mxcsr&64u))bits=0; /* positive DAZ input */
        else{if(subnormal)flags|=2;bits=positive_root(bits,wide,(c->mxcsr>>13)&3u,&flags);}
    }
    /* Native unmasked exceptions need guest SEH support, never false success. */
    if(flags&~((c->mxcsr>>7)&63u))stop("unmasked guest floating-point exception is unsupported");
    c->mxcsr|=flags;
    if(wide)c->xmm[0].u64[0]=bits;else c->xmm[0].u32[0]=(uint32_t)bits;
    return 1;
}
void win64_math_self_test(void){
    for(unsigned mode=0;mode<16;++mode){
        CPU value,expected;memset(&value,0xa7,sizeof(value));
        value.mxcsr=UINT32_C(0x24)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u)|((mode&1u)?UINT32_C(0x1f80):0);
        value.xmm[0].u64[0]=(mode&2u)?0:UINT64_C(0x4024000000000000);
        value.xmm[1].u64[0]=(mode&1u)?UINT64_C(0x8000000000000000):0;
        expected=value;expected.xmm[0].u64[0]=UINT64_C(0x3ff0000000000000);
        if(!win64_math_call(&value,"pow")||memcmp(&value,&expected,sizeof(value)))stop("pow scene zero-exponent exact/fullCPU/control self-test");
    }
    fputs("CRT_POW_ZERO_SELF_TEST actual10_power0=1 signed_zero_powers=1 zero_base=1 exact1=1 full_CPU_MXCSR=1 masked_unmasked_controls=1 passed\n",stderr);
    for(unsigned r=0;r<4;++r){uint64_t control=native_control(UINT64_MAX,r);
        if(control&((UINT64_C(31)<<8)|(UINT64_C(1)<<15)|(UINT64_C(1)<<24)))stop("native ARM exception mask self-test");
        if(((control>>22)&3u)!=(((r&1u)<<1)|((r&2u)>>1)))stop("native ARM rounding map self-test");
    }
    CPU c={0};c.rsp=UINT64_C(0x12345678);c.rbx=UINT64_C(0xabcdef);c.rax=UINT64_C(0x7744);c.cf=1;
    c.mxcsr=0x1f80;c.xmm[0].u32[0]=0x40800000;c.xmm[0].u32[1]=0xaabbccdd;
    if(!win64_math_call(&c,"sqrtf")||c.xmm[0].u32[0]!=0x40000000||c.xmm[0].u32[1]!=0xaabbccdd||c.mxcsr!=0x1f80)stop("sqrtf exact/XMM lane self-test");
    c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);c.xmm[0].u64[1]=UINT64_C(0xa5b6c7d8);
    if(!win64_math_call(&c,"sqrt")||c.xmm[0].u64[0]!=UINT64_C(0x3ff6a09e667f3bcd)||c.xmm[0].u64[1]!=UINT64_C(0xa5b6c7d8)||c.mxcsr!=0x1fa0)stop("sqrt double/inexact self-test");
    c.mxcsr=0x3f80;c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);
    if(!win64_math_call(&c,"sqrt")||c.xmm[0].u64[0]!=UINT64_C(0x3ff6a09e667f3bcc))stop("sqrt directed rounding self-test");
    c.mxcsr=0x1f80;c.xmm[0].u32[0]=0xff812345;
    if(!win64_math_call(&c,"sqrtf")||c.xmm[0].u32[0]!=0xffc12345||c.mxcsr!=0x1f80)stop("UCRT signaling NaN policy self-test");
    c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);
    if(!win64_math_call(&c,"log2l")||c.xmm[0].u64[0]!=UINT64_C(0x3ff0000000000000)||c.mxcsr!=0x1f80)stop("Win64 long double/log2 exact self-test");
    c.xmm[0].u64[0]=UINT64_C(0x7ff0123456789abc);
    if(!win64_math_call(&c,"log2l")||c.xmm[0].u64[0]!=UINT64_C(0x7ff0123456789abc)||c.mxcsr!=0x1f80)stop("UCRT log2l signaling NaN policy self-test");
    c.xmm[0].u32[0]=0xbfc00000;
    if(!win64_math_call(&c,"floorf")||c.xmm[0].u32[0]!=0xc0000000)stop("floorf self-test");
    c.xmm[0].u32[0]=0xbfc00000;
    if(!win64_math_call(&c,"roundf")||c.xmm[0].u32[0]!=0xc0000000)stop("roundf ties-away self-test");
    c.mxcsr=0x1f80;c.xmm[0].u32[0]=0x80000000;
    if(!win64_math_call(&c,"sinf")||c.xmm[0].u32[0]!=0x80000000||c.mxcsr!=0x1f80)stop("sinf signed zero self-test");
    if(!win64_math_call(&c,"cosf")||c.xmm[0].u32[0]!=0x3f800000||c.mxcsr!=0x1f80)stop("cosf zero self-test");
    c.xmm[0].u32[0]=1;
    if(!win64_math_call(&c,"sinf")||c.xmm[0].u32[0]!=1||c.mxcsr!=0x1fb2)stop("sinf tiny status self-test");
    c.mxcsr=0x1f80;c.xmm[0].u64[0]=UINT64_C(0x4000000000000000);
    if(!win64_math_call(&c,"exp2")||c.xmm[0].u64[0]!=UINT64_C(0x4010000000000000)||c.mxcsr!=0x1f80)stop("exp2 exact power self-test");
    c.xmm[0].u32[0]=UINT32_C(0x3f800000);c.xmm[1].u32[0]=UINT32_C(0x3f2aaaab);
    if(!win64_math_call(&c,"powf")||c.xmm[0].u32[0]!=UINT32_C(0x3f800000)||c.mxcsr!=0x1f80)stop("bounded powf width-one self-test");
    c.mxcsr=0x3f80;c.xmm[0].u32[0]=UINT32_C(0x3f800000);
    if(!win64_math_call(&c,"logf")||c.xmm[0].u32[0]!=UINT32_C(0x80000000)||c.mxcsr!=0x3f80)stop("logf exact one/downward signed-zero self-test");
    for(unsigned mode=0;mode<16;++mode){
        CPU value=c;value.mxcsr=UINT32_C(0x1fa5)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u32[0]=UINT32_C(0x41800000);CPU expected=value;expected.xmm[0].u32[0]=UINT32_C(0x47800000);
        if(!win64_math_call(&value,"exp2f")||memcmp(&value,&expected,sizeof(value)))stop("exp2f exact16/fullCPU/control self-test");
    }
    if(c.rsp!=UINT64_C(0x12345678)||c.rbx!=UINT64_C(0xabcdef)||c.rax!=UINT64_C(0x7744)||!c.cf||win64_math_call(&c,"unknown_math"))stop("floating ABI preservation self-test");
    {
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=0x1f80;
        value.xmm[0].u32[0]=UINT32_C(0x40000000);value.xmm[1].u32[0]=UINT32_C(0x40400000);expected=value;expected.mxcsr|=32;
        if(!win64_math_call(&value,"powf")||value.xmm[0].u32[0]<UINT32_C(0x40fffffc)||value.xmm[0].u32[0]>UINT32_C(0x41000004))stop("powf normal integral-power self-test");
        expected.xmm[0].u32[0]=value.xmm[0].u32[0];if(memcmp(&value,&expected,sizeof(value)))stop("powf normal fullCPU/status self-test");
    }
    for(unsigned mode=0;mode<16;++mode) {
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1f85)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u64[0]=UINT64_C(0x4030000000000000);value.xmm[1].u64[0]=UINT64_C(0x3fe0000000000000);
        expected=value;expected.mxcsr|=32;
        if(!win64_math_call(&value,"pow")||value.xmm[0].u64[0]<UINT64_C(0x400ffffffffffffc)||value.xmm[0].u64[0]>UINT64_C(0x4010000000000004))stop("codec pow double16/result bound self-test");
        expected.xmm[0].u64[0]=value.xmm[0].u64[0];
        if(memcmp(&value,&expected,sizeof(value)))stop("codec pow double16/fullCPU/status self-test");
    }
    fputs("CRT_POW_CODEC_SELF_TEST double_XMM0_XMM1=1 full_CPU=1 controls16=1 precision32=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned kind=0;kind<3;++kind)for(unsigned mode=0;mode<16;++mode){
        if(kind&&(mode&3u))continue;
        static const char *names[]={"log","cos","sin"};
        static const uint64_t results[]={UINT64_C(0x3fe62e42fefa39ef),UINT64_C(0x3fe14a280fb5068c),UINT64_C(0x3feaed548f090cee)};
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1f85)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u64[0]=kind?UINT64_C(0x3ff0000000000000):UINT64_C(0x4000000000000000);expected=value;expected.mxcsr|=32;
        uint64_t bound=4u;
        if(!win64_math_call(&value,names[kind])||value.xmm[0].u64[0]<results[kind]-bound||value.xmm[0].u64[0]>results[kind]+bound)stop("codec log/cos/sin result bound self-test");
        expected.xmm[0].u64[0]=value.xmm[0].u64[0];if(memcmp(&value,&expected,sizeof(value)))stop("codec log/cos/sin fullCPU/status self-test");
    }
    fputs("CRT_MDCT_MATH_SELF_TEST log_cos_sin_double=1 full_CPU=1 log_controls16=1 trig_nearest_DAZ_FTZ4=1 precision32=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned mode=0;mode<16;++mode){
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1f95)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.rdx=UINT64_C(0xa5b6c7d8fffffcec);value.xmm[0].u64[0]=UINT64_C(0xc008000000000000);expected=value;expected.xmm[0].u64[0]=UINT64_C(0x8ec8000000000000);
        if(!win64_math_call(&value,"ldexp")||memcmp(&value,&expected,sizeof(value)))stop("codec ldexp exact/fullCPU/control self-test");
    }
    fputs("CRT_LDEXP_CODEC_SELF_TEST double_XMM0_int_RDX=1 ignored_RDX_upper32=1 exact_normal=1 full_CPU_MXCSR=1 controls16=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned mode=0;mode<16;++mode){
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1f95)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u32[0]=UINT32_C(0xc0b00000);value.xmm[1].u32[0]=UINT32_C(0x3f800000);expected=value;expected.xmm[0].u32[0]=UINT32_C(0xbf000000);
        if(!win64_math_call(&value,"fmodf")||memcmp(&value,&expected,sizeof(value)))stop("fmodf unit-divisor/fullCPU/control self-test");
    }
    fputs("CRT_FMODF_UNIT_SELF_TEST float_XMM0_XMM1=1 exact_signed_fraction=1 full_CPU_MXCSR=1 controls16=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned mode=0;mode<16;++mode){
        for(unsigned sample=0;sample<3;++sample){
            const uint32_t inputs[3]={0,UINT32_C(0x41580000),UINT32_C(0xc1580000)};
            const uint32_t results[3]={0,UINT32_C(0x40900000),UINT32_C(0xc0900000)};
            CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1fa4)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
            value.xmm[0].u32[0]=inputs[sample];value.xmm[1].u32[0]=UINT32_C(0x41100000);expected=value;expected.xmm[0].u32[0]=results[sample];
            if(!win64_math_call(&value,"fmodf")||memcmp(&value,&expected,sizeof(value)))stop("fmodf positive-duration/fullCPU/control self-test");
        }
    }
    fputs("CRT_FMODF_DURATION_SELF_TEST actual_0_mod9=1 advancing_signed_mod9=1 full_CPU_MXCSR=1 controls16=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned mode=0;mode<16;++mode){
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1f95)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u32[0]=UINT32_C(0xbf800000);expected=value;expected.mxcsr|=32;
        if(!win64_math_call(&value,"expf")||value.xmm[0].u32[0]<UINT32_C(0x3ebc5aae)||value.xmm[0].u32[0]>UINT32_C(0x3ebc5ab6))stop("expf normal Gaussian result bound self-test");
        expected.xmm[0].u32[0]=value.xmm[0].u32[0];if(memcmp(&value,&expected,sizeof(value)))stop("expf fullCPU/control self-test");
    }
    fputs("CRT_EXPF_GAUSSIAN_SELF_TEST float_XMM0=1 full_CPU=1 controls16=1 nearest_double_then_guest_float=1 precision32=1 no_rsp_pop=1 passed\n",stderr);
    for(unsigned mode=0;mode<16;++mode){
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=UINT32_C(0x1fa4)|((mode&3u)<<13)|((mode&4u)?64u:0u)|((mode&8u)?32768u:0u);
        value.xmm[0].u32[0]=UINT32_C(0xc4e0ffff);expected=value;expected.mxcsr|=48;expected.xmm[0].u32[0]=0;
        if(!win64_math_call(&value,"expf")||memcmp(&value,&expected,sizeof(value)))stop("expf actual underflow tail/fullCPU/status self-test");
    }
    fputs("CRT_EXPF_UNDERFLOW_SELF_TEST actual_c4e0ffff=1 zero_tail=1 UF_PE48=1 full_CPU=1 controls16=1 errno_unchanged=1 passed\n",stderr);
    {
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=0x1f80;
        value.xmm[0].u32[0]=UINT32_C(0x3e800000);expected=value;expected.mxcsr|=32;
        if(!win64_math_call(&value,"tanf")||value.xmm[0].u32[0]<UINT32_C(0x3e82bc29)||value.xmm[0].u32[0]>UINT32_C(0x3e82bc31))stop("tanf finite projection self-test");
        expected.xmm[0].u32[0]=value.xmm[0].u32[0];if(memcmp(&value,&expected,sizeof(value)))stop("tanf fullCPU/status self-test");
    }
    for(unsigned sample=0;sample<3;++sample){
        const uint32_t inputs[3]={UINT32_C(0x3f800000),UINT32_C(0xbf800000),UINT32_C(0x3f000000)};
        const uint32_t results[3]={0,UINT32_C(0x40490fdb),UINT32_C(0x3f860a92)};
        CPU value,expected;memset(&value,0xa7,sizeof(value));value.mxcsr=0x1f80;value.xmm[0].u32[0]=inputs[sample];expected=value;if(sample)expected.mxcsr|=32;
        if(!win64_math_call(&value,"acosf")||value.xmm[0].u32[0]<results[sample]-(sample==2)||value.xmm[0].u32[0]>results[sample]+(sample==2))stop("acosf normalized-vector result self-test");
        expected.xmm[0].u32[0]=value.xmm[0].u32[0];if(memcmp(&value,&expected,sizeof(value)))stop("acosf fullCPU/status self-test");
    }
    fputs("WIN64_MATH_SELF_TEST sqrtf_sqrt=1 log2_family=1 floor_ceil_round=1 sinf_cosf=1 bounded_tanf=1 exp2=1 bounded_exp2f=1 bounded_texture_powf=1 positive_normal_powf=1 bounded_logf=1 bounded_acosf=1 Win64_longdouble64=1 XMM_ABI=1 rounding=1 native_exception_mask=1 sticky_precision=1 UCRT_NaN_policy=1 no_rsp_pop=1 passed\n",stderr);
}
