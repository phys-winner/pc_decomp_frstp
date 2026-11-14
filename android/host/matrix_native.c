#include "cpu64.h"
#include <string.h>
#ifdef __ANDROID__
#include <errno.h>
#include <stdatomic.h>
#include <sys/system_properties.h>
#endif

typedef float matrix_vector __attribute__((vector_size(16)));
typedef uint32_t matrix_bits __attribute__((vector_size(16)));
void fp_original_L_000140DFA630(CPU *c);

static int matrix_enabled(void) {
#ifdef __ANDROID__
    static _Atomic int enabled=-1;
    int value=atomic_load_explicit(&enabled,memory_order_relaxed);
    if(value<0){
        char setting[PROP_VALUE_MAX]={0};int saved=errno;
        __system_property_get("debug.fp.native_matrix",setting);errno=saved;
        value=setting[0]!='0';atomic_store_explicit(&enabled,value,memory_order_relaxed);
    }
    return value;
#else
    return 1;
#endif
}

static matrix_vector matrix_load(uint64_t address) {
    matrix_vector value;memcpy(&value,(const void *)(uintptr_t)address,sizeof(value));return value;
}
static void matrix_store(uint64_t address,matrix_vector value) {
    memcpy((void *)(uintptr_t)address,&value,sizeof(value));
}
static int matrix_overlap(uint64_t a,uint64_t bytes,uint64_t b,uint64_t other_bytes) {
    return a<b+other_bytes && b<a+bytes;
}
static int matrix_finite(uint64_t address) {
    const matrix_bits exponent={0x7f800000,0x7f800000,0x7f800000,0x7f800000};
    matrix_bits found={0};
    for(unsigned i=0;i<4;++i){matrix_bits bits;memcpy(&bits,(const void *)(uintptr_t)(address+i*16),16);
        found|=(matrix_bits)((bits&exponent)==exponent);
    }
    return !(found[0]|found[1]|found[2]|found[3]);
}

void L_000140DFA630(CPU *c) {
    uint64_t output=c->rcx,left=c->rdx,right=c->r8,stack=c->rsp;
    uint64_t cpu=(uintptr_t)c;
    /* shortcut: retain non-finite and unusual aliases in the original until their vector ordering is proven. */
    if(!matrix_enabled()||!output||!left||!right||output>UINT64_MAX-64||left>UINT64_MAX-64||right>UINT64_MAX-64||
       stack<56||stack>UINT64_MAX-8||
       matrix_overlap(output,64,cpu,sizeof(*c))||matrix_overlap(left,64,cpu,sizeof(*c))||
       matrix_overlap(right,64,cpu,sizeof(*c))||matrix_overlap(stack-56,56,cpu,sizeof(*c))||
       matrix_overlap(output,64,stack-56,56)||matrix_overlap(left,64,stack-56,56)||
       matrix_overlap(right,64,stack-56,56)||(output!=right&&matrix_overlap(output,64,right,64))||
       !matrix_finite(left)||!matrix_finite(right)) {
        fp_original_L_000140DFA630(c);return;
    }
    c->rip=UINT64_C(0x140dfa630);
    c->rsp=flags_sub(c,stack,56,8);
    wrxm(c->rsp+32,c->xmm[6]);wrxm(c->rsp+16,c->xmm[7]);wrxm(c->rsp,c->xmm[8]);
    matrix_vector row=matrix_load(right);
    matrix_vector a3=matrix_load(left+48),a2=matrix_load(left+32);
    matrix_vector a1=matrix_load(left+16),a0=matrix_load(left);
    matrix_vector p0,p1,p2,p3,result,previous={0};
    for(unsigned i=0;i<4;++i) {
        if(i)row=matrix_load(right+i*16);
        p3=a3*row[3];p2=a2*row[2];p1=a1*row[1];p0=a0*row[0];
        /* Preserve the original pairwise sum, without reassociation or FMA. */
        p1=p1+p0;result=(p3+p2)+p1;
        matrix_store(output+i*16,result);
        if(i==2)previous=result;
    }
    memcpy(&c->xmm[0],&p2,16);memcpy(&c->xmm[1],&p1,16);memcpy(&c->xmm[2],&p0,16);
    memcpy(&c->xmm[3],&previous,16);memcpy(&c->xmm[4],&result,16);memcpy(&c->xmm[5],&a3,16);
    c->rsp=flags_add(c,c->rsp,56,8);c->rsp+=8;
}
