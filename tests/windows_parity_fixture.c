#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#if defined(FP_PARITY_FALLBACK)
#undef FROSTPUNK_WINDOWS_HOST
#endif
#include "cpu64.h"

#if defined(FP_PARITY_WRAPPER)
#if defined(FP_PARITY_FALLBACK)
#define NAME(n) fallback_##n
#else
#define NAME(n) current_##n
#endif
uint32_t NAME(parity)(uint64_t value){return parity8((uint8_t)value);}
uint64_t NAME(flags)(CPU *c,uint64_t a,uint64_t b,unsigned op,unsigned size) {
    uint64_t n=c->r15;
    switch(op) {
    case 0:return flags_logicz(c,a,size);
    case 1:return flags_add(c,a,b,size);
    case 2:return flags_sub(c,a,b,size);
    case 3:return flags_adc(c,a,b,size);
    case 4:return flags_sbb(c,a,b,size);
    case 5:return flags_incs(c,a,size);
    case 6:return flags_decs(c,a,size);
    case 7:return op_shl(c,a,n,size);
    case 8:return op_shr(c,a,n,size);
    case 9:return op_sar(c,a,n,size);
    case 10:return op_shld(c,a,b,n,size);
    case 11:return op_shrd(c,a,b,n,size);
    default:abort();
    }
}
#else
uint32_t fallback_parity(uint64_t),current_parity(uint64_t);
uint64_t fallback_flags(CPU *,uint64_t,uint64_t,unsigned,unsigned),current_flags(CPU *,uint64_t,uint64_t,unsigned,unsigned);
static __declspec(noinline) unsigned native_parity(uint64_t input) {
    uint8_t value=(uint8_t)input,result;
    __asm__ volatile("testb %1,%1; setp %0" : "=q"(result) : "q"(value) : "cc");return result;
}
static uint64_t random_state=UINT64_C(0x7d9f45c31e62a08b);
static uint64_t random64(void){random_state^=random_state<<13;random_state^=random_state>>7;return random_state^=random_state<<17;}
static unsigned checked;
static void check(unsigned op,unsigned size,uint64_t a,uint64_t b,uint64_t count,unsigned carry) {
    CPU input;unsigned char *p=(unsigned char *)&input;
    for(unsigned i=0;i<sizeof(input);++i)p[i]=(unsigned char)random64();
    input.cf=carry;input.pf=random64()&1;input.r15=count;CPU old=input,now=input;
    unsigned csr=_mm_getcsr();SetLastError(0xe5ef1234);
    uint64_t expected=fallback_flags(&old,a,b,op,size),actual=current_flags(&now,a,b,op,size);
    assert(expected==actual && !memcmp(&old,&now,sizeof(now)) && GetLastError()==0xe5ef1234 && _mm_getcsr()==csr);
    unsigned n=(unsigned)count&(size==8?63:31);
    if(op<7 || (n && (op<10 || n<size*8)))assert(now.pf==native_parity(actual));
    ++checked;
}
int main(void) {
    for(unsigned v=0;v<256;++v) {
        uint64_t input=(random64()&~UINT64_C(255))|v;unsigned expected=native_parity(input);
        assert(fallback_parity(input)==expected && current_parity(input)==expected);
    }
    uint64_t edge[]={0,1,2,7,8,15,16,31,32,63,64,127,128,255,256,UINT64_C(0x7fffffffffffffff),UINT64_C(0x8000000000000000),UINT64_MAX};
    unsigned sizes[]={1,2,4,8},saved=_mm_getcsr();_mm_setcsr(0x5fc3);
    for(unsigned op=0;op<12;++op)for(unsigned sz=0;sz<4;++sz)for(unsigned carry=0;carry<2;++carry) {
        for(unsigned i=0;i<18;++i)for(unsigned j=0;j<18;++j)check(op,sizes[sz],edge[i],edge[j],edge[j],carry);
        for(unsigned n=0;n<256;++n)check(op,sizes[sz],random64(),random64(),n,carry);
    }
    _mm_setcsr(saved);
    printf("{\"success\":true,\"native_PF_values\":256,\"full_CPU_flag_cases\":%u,\"helper_operations\":12,\"operand_widths\":4,\"both_input_carries\":true,\"LastError_MXCSR\":true}\n",checked);
    return 0;
}
#endif
