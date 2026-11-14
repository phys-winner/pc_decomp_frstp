#include "cpu64.h"
#include <string.h>
typedef float float4 __attribute__((vector_size(16)));
typedef uint32_t uint4 __attribute__((vector_size(16)));
void fp_bounds_original(CPU *);
static float4 load4(uint64_t address){float4 value;memcpy(&value,(void*)(uintptr_t)address,16);return value;}
static int overlap(uint64_t a,uint64_t n,uint64_t b,uint64_t m){return a<b+m&&b<a+n;}
static int bounded(float4 value){
    uint4 bits;memcpy(&bits,&value,16);bits&=(uint4){0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff};
    uint4 bad=(uint4)((bits!=(uint4){0})&((bits<(uint4){0x2f800000,0x2f800000,0x2f800000,0x2f800000})|
        (bits>(uint4){0x4f800000,0x4f800000,0x4f800000,0x4f800000})));
    return !(bad[0]|bad[1]|bad[2]|bad[3]);
}
static int ordinary(float4 value){
    uint4 bits;memcpy(&bits,&value,16);bits&=(uint4){0x7fffffff,0x7fffffff,0x7fffffff,0x7fffffff};
    uint4 bad=(uint4)((bits>=(uint4){0x7f800000,0x7f800000,0x7f800000,0x7f800000})|
        ((bits!=(uint4){0})&(bits<(uint4){0x00800000,0x00800000,0x00800000,0x00800000})));
    return !(bad[0]|bad[1]|bad[2]|bad[3]);
}
static float4 min4(float4 a,float4 b){uint4 aa,bb,mask=(uint4)(a<b);memcpy(&aa,&a,16);memcpy(&bb,&b,16);aa=(aa&mask)|(bb&~mask);memcpy(&a,&aa,16);return a;}
static float4 max4(float4 a,float4 b){uint4 aa,bb,mask=(uint4)(a>b);memcpy(&aa,&a,16);memcpy(&bb,&b,16);aa=(aa&mask)|(bb&~mask);memcpy(&a,&aa,16);return a;}
void L_000140E2C200(CPU *c){
    uint64_t out=c->rcx,box=c->rdx,matrix=c->r8,stack=c->rsp,cpu=(uintptr_t)c,flag=GVA(0x14296FAE0);
    if(!out||!box||!matrix||out>UINT64_MAX-32||box>UINT64_MAX-32||matrix>UINT64_MAX-64||
       stack<152||stack>UINT64_MAX-24||flag>UINT64_MAX-4||
       overlap(out,32,cpu,sizeof(*c))||overlap(box,32,cpu,sizeof(*c))||overlap(matrix,64,cpu,sizeof(*c))||
       overlap(stack-152,176,cpu,sizeof(*c))||overlap(flag,4,cpu,sizeof(*c))||
       overlap(out,32,stack-152,176)||overlap(box,32,stack-152,176)||overlap(matrix,64,stack-152,176)||
       overlap(flag,4,stack-152,176)||(c->mxcsr&~0x3fu)!=0x1f80u||rd32(flag)){
        fp_bounds_original(c);return;
    }
#ifdef __aarch64__
    uint64_t control;__asm__ volatile("mrs %0,fpcr":"=r"(control)::"memory");
    if(control){fp_bounds_original(c);return;}
#else
    fp_bounds_original(c);return;
#endif
    float4 lower=load4(box),upper=load4(box+16);
    float4 m0=load4(matrix),m1=load4(matrix+16),m2=load4(matrix+32),translation=load4(matrix+48);
    float4 minimum=load4(out),maximum=load4(out+16);
    /* shortcut: inputs outside [2^-32,2^32] use the original; widen only with an exact FP oracle.
     * Products/sums are multiples of 2^-87 and below 2^66, so no new subnormals, NaNs or infinities arise. */
    if(!bounded(lower)||!bounded(upper)||!bounded(m0)||!bounded(m1)||!bounded(m2)||!bounded(translation)||
       !ordinary(minimum)||!ordinary(maximum)){fp_bounds_original(c);return;}
    float4 ux=__builtin_shufflevector(upper,upper,0,0,0,0)*m0;
    float4 uz=__builtin_shufflevector(upper,upper,2,2,2,2)*m2;
    float4 lx=__builtin_shufflevector(lower,lower,0,0,0,0)*m0;
    float4 ly=__builtin_shufflevector(lower,lower,1,1,1,1)*m1;
    float4 lz=__builtin_shufflevector(lower,lower,2,2,2,2)*m2;
    float4 uy=__builtin_shufflevector(upper,upper,1,1,1,1)*m1;
    float4 pairs[4]={lx+ly,uy+lx,ux+ly,ux+uy};
    float4 last_low,last_high,previous_high;
    for(unsigned i=0;i<4;++i){
        float4 low=(pairs[i]+lz)+translation,high=(pairs[i]+uz)+translation;
        minimum=min4(min4(minimum,low),high);maximum=max4(max4(maximum,low),high);
        if(i==2)previous_high=high;
        last_low=low;last_high=high;
    }
    wr64(stack+8,c->rbx);wr64(stack+16,c->rsi);wr64(stack-8,c->rdi);
    for(unsigned i=0;i<7;++i)wrxm(stack-24-16u*i,c->xmm[7+i]);
    memcpy((void*)(uintptr_t)out,&minimum,16);memcpy((void*)(uintptr_t)(out+16),&maximum,16);
    memcpy(&c->xmm[0],&last_low,16);memcpy(&c->xmm[1],&previous_high,16);
    memcpy(&c->xmm[3],&lx,16);memcpy(&c->xmm[5],&ly,16);
    c->r11=stack-8;c->rsp=stack+8;c->rip=UINT64_C(0x140e2c27e);flags_sub(c,0,0,4);
    (void)last_high;
}
