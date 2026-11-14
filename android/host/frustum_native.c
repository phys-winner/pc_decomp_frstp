#include "cpu64.h"
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#elif defined(_WIN32)
#include <xmmintrin.h>
#endif

typedef float float4 __attribute__((vector_size(16)));
typedef uint32_t uint4 __attribute__((vector_size(16)));
void fp_frustum_original(CPU *);
static int enabled(void) {
#ifdef __ANDROID__
    static _Atomic int cached=-1;
    int value=atomic_load_explicit(&cached,memory_order_relaxed);
    if(value<0){int saved=errno;char text[PROP_VALUE_MAX]={0};
        __system_property_get("debug.fp.native_frustum",text);errno=saved;
        value=text[0]!='0';atomic_store_explicit(&cached,value,memory_order_relaxed);}
    return value;
#else
    return 1;
#endif
}
static uint64_t control(void) {
#if defined(__aarch64__)
    uint64_t value;__asm__ volatile("mrs %0,fpcr":"=r"(value)::"memory");return value;
#else
    return _mm_getcsr();
#endif
}
static uint64_t status(void) {
#if defined(__aarch64__)
    uint64_t value;__asm__ volatile("mrs %0,fpsr":"=r"(value)::"memory");return value;
#else
    return _mm_getcsr();
#endif
}
static void restore(uint64_t value) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpsr,%0"::"r"(value):"memory");
#else
    _mm_setcsr((unsigned)value);
#endif
}
static float4 load(uint64_t address){float4 value;memcpy(&value,(const void*)(uintptr_t)address,16);return value;}
static int ordinary(float4 value) {
    uint4 bits;memcpy(&bits,&value,16);uint4 absolute=bits&(uint4){0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff};
    for(unsigned i=0;i<4;++i)if(absolute[i]>=0x7f800000u||(absolute[i]&&absolute[i]<0x00800000u))return 0;
    return 1;
}
static float4 negative(float4 value){uint4 bits,mask=(uint4)(value<(float4){0});memcpy(&bits,&value,16);bits&=mask;memcpy(&value,&bits,16);return value;}
static float4 positive(float4 value){uint4 bits,mask=(uint4)(value>(float4){0});memcpy(&bits,&value,16);bits&=mask;memcpy(&value,&bits,16);return value;}
static int overlaps(uint64_t address,uint64_t bytes,CPU*c){uint64_t cpu=(uintptr_t)c;return address<cpu+sizeof(*c)&&cpu<address+bytes;}

void L_000140E2C890(CPU *c) {
    uint64_t box=c->rcx,planes=c->rdx;
    if(!enabled()||!box||!planes||box>UINT64_MAX-32||planes>UINT64_MAX-96||
       overlaps(box,32,c)||overlaps(planes,96,c)||(c->mxcsr&0x1f80u)!=0x1f80u||c->mxcsr&~0xffffu){
        fp_frustum_original(c);return;
    }
    uint64_t fp_control=control();
#if defined(__aarch64__)
    if(fp_control&((UINT64_C(31)<<8)|(UINT64_C(1)<<15))){fp_frustum_original(c);return;}
#else
    if((fp_control&0x1f80u)!=0x1f80u){fp_frustum_original(c);return;}
#endif
    float4 lower=load(box),upper=load(box+16);
    /* shortcut: keep unusual FP values on the original; expand only with an exact oracle. */
    if(!ordinary(lower)||!ordinary(upper)){fp_frustum_original(c);return;}
    uint64_t fp_status=status();
    float4 low={0},sum={0};uint4 compared={0};unsigned index,mask=0;
    for(index=0;index<3;++index){
        float4 a=load(planes+index*32),b=load(planes+index*32+16);
        if(!ordinary(a)||!ordinary(b))goto fallback;
        float4 left=negative(a)*lower,right=positive(a)*upper;
        float4 first=right+left;
        right=positive(b)*upper;left=negative(b)*lower;
        float4 second=right+left;
        low=__builtin_shufflevector(first,second,0,4,1,5);
        float4 high=__builtin_shufflevector(first,second,2,6,3,7);
        sum=high+low;
        float4 swapped=__builtin_shufflevector(sum,sum,2,3,0,1);
        float4 dot=swapped+sum;
        if(!ordinary(dot))goto fallback;
        compared=(uint4)(dot<(float4){0});
        mask=(compared[0]>>31)|((compared[1]>>31)<<1)|((compared[2]>>31)<<2)|((compared[3]>>31)<<3);
        if(mask)break;
        __asm__ volatile("":::"memory");
    }
    memcpy(&c->xmm[0],&compared,16);memcpy(&c->xmm[1],&sum,16);memcpy(&c->xmm[2],&low,16);
    memset(&c->xmm[3],0,16);memcpy(&c->xmm[4],&lower,16);memcpy(&c->xmm[5],&upper,16);
    c->rax=mask?0:1;c->rcx=mask;c->rdx=planes+index*32;c->rsp+=8;
    if(mask){if(index)flags_sub(c,index,3,8);flags_logicz(c,0,1);c->rip=UINT64_C(0x140e2c8fc);}
    else{flags_sub(c,3,3,8);c->rip=UINT64_C(0x140e2c8a0);}
    return;
fallback:
    restore(fp_status);fp_frustum_original(c);
}
