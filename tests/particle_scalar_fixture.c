/* Source-level scalar ADD/MUL equivalence only; no game execution or ADB. */
#include "cpu64.h"
#include "particle_scalar_candidate.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
static NOINLINE float baseline(float a,float b,int multiply){return multiply?frostpunk_mulss(a,b):frostpunk_addss(a,b);}
static NOINLINE float candidate(float a,float b,int multiply){return multiply?fp_particle_mulss(a,b):fp_particle_addss(a,b);}
static void read_fp(uint64_t state[2]) {
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(state[0]),"=r"(state[1]));
#else
    state[0]=_mm_getcsr();state[1]=0;
#endif
}
static void write_fp(const uint64_t state[2]) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(state[0]),"r"(state[1]):"memory");
#else
    _mm_setcsr((unsigned)state[0]);
#endif
}
static void mode_fp(uint64_t mode[2],const uint64_t original[2],unsigned index) {
#if defined(__aarch64__)
    unsigned rounding=index&3,arm=((rounding&1)<<1)|((rounding&2)>>1);
    mode[0]=(original[0]&~((UINT64_C(3)<<22)|(UINT64_C(1)<<24)|(UINT64_C(1)<<25)|
        (UINT64_C(31)<<8)|(UINT64_C(1)<<15)|UINT64_C(3)))|((uint64_t)arm<<22)|
        (index&4?UINT64_C(1)<<24:0)|(index&8?UINT64_C(1)<<25:0);
    mode[1]=index&16?UINT64_C(0x80000095):0;
#else
    (void)original;mode[0]=0x1f80|((index&3)<<13)|(index&4?64:0)|(index&8?32768:0)|(index&16?0x15:0);mode[1]=0;
#endif
}
static uint32_t rng=UINT32_C(0xf63a856c);
static uint32_t random32(void){rng=rng*UINT32_C(1664525)+UINT32_C(1013904223);return rng;}
static uint64_t cases,bit_mismatches,fp_mismatches,cpu_cases;
static uint64_t checksum=UINT64_C(14695981039346656037);
static void pair(uint32_t a,uint32_t b,unsigned mode,unsigned multiply,const uint64_t original[2]) {
    float fa,fb,r;memcpy(&fa,&a,4);memcpy(&fb,&b,4);uint32_t old_bits,new_bits;
    uint64_t environment[2],old_fp[2],new_fp[2];mode_fp(environment,original,mode);
    write_fp(environment);errno=2718;r=baseline(fa,fb,multiply);memcpy(&old_bits,&r,4);read_fp(old_fp);int old_errno=errno;
    write_fp(environment);errno=2718;r=candidate(fa,fb,multiply);memcpy(&new_bits,&r,4);read_fp(new_fp);int new_errno=errno;
    write_fp(original);
    if(old_bits!=new_bits){if(bit_mismatches<4)fprintf(stderr,"PARTICLE_BITS a=%08x b=%08x mode=%u op=%u old=%08x new=%08x\n",a,b,mode,multiply,old_bits,new_bits);++bit_mismatches;}
    if(memcmp(old_fp,new_fp,sizeof(old_fp))||old_errno!=new_errno){if(fp_mismatches<4)fprintf(stderr,"PARTICLE_FP a=%08x b=%08x mode=%u op=%u old=%" PRIx64 "/%" PRIx64 " new=%" PRIx64 "/%" PRIx64 "\n",a,b,mode,multiply,old_fp[0],old_fp[1],new_fp[0],new_fp[1]);++fp_mismatches;}
    checksum=(checksum^old_bits)*UINT64_C(1099511628211);++cases;
}
static void cpu_pair(unsigned destination,unsigned source,unsigned multiply,const uint64_t original[2]) {
    CPU a,b;unsigned char *p=(unsigned char*)&a;for(size_t i=0;i<sizeof(a);++i)p[i]=(unsigned char)random32();
    a.xmm[destination].u32[0]=random32();a.xmm[source].u32[0]=random32();b=a;
    uint64_t mode[2],old_fp[2],new_fp[2];mode_fp(mode,original,(destination+source)&31);
    write_fp(mode);a.xmm[destination].f32[0]=baseline(a.xmm[destination].f32[0],a.xmm[source].f32[0],multiply);read_fp(old_fp);
    write_fp(mode);b.xmm[destination].f32[0]=candidate(b.xmm[destination].f32[0],b.xmm[source].f32[0],multiply);read_fp(new_fp);write_fp(original);
    if(memcmp(&a,&b,sizeof(a)))++bit_mismatches;if(memcmp(old_fp,new_fp,sizeof(old_fp)))++fp_mismatches;++cpu_cases;
}
int main(void) {
    static const uint32_t edge[]={0,0x80000000,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,
        0x3f000000,0xbf000000,0x3f800000,0xbf800000,0x40000000,0xc0000000,0x7f7fffff,0xff7fffff,
        0x7f800000,0xff800000,0x7fc00000,0xffc00000,0x7fc00001,0xffc12345,0x7fffffff,0xffffffff,
        0x7f800001,0xff800001,0x7fa00000,0xffbfffff};
    uint64_t original[2];read_fp(original);
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);++i)for(unsigned j=0;j<sizeof(edge)/sizeof(edge[0]);++j)
        for(unsigned mode=0;mode<32;++mode)for(unsigned op=0;op<2;++op)pair(edge[i],edge[j],mode,op,original);
    for(unsigned i=0;i<8192;++i){uint32_t a=random32(),b=random32();for(unsigned mode=0;mode<32;++mode)for(unsigned op=0;op<2;++op)pair(a,b,mode,op,original);}
    for(unsigned d=0;d<16;++d)for(unsigned s=0;s<16;++s)for(unsigned op=0;op<2;++op)cpu_pair(d,s,op,original);
    write_fp(original);
    printf("{\"success\":%s,\"paired_cases\":%" PRIu64 ",\"full_CPU_alias_cases\":%" PRIu64 ",\"native_FP_modes\":32,\"bit_mismatches\":%" PRIu64 ",\"FP_state_mismatches\":%" PRIu64 ",\"checksum\":\"%016" PRIx64 "\",\"production_changed\":false,\"gameplay_proven\":false}\n",bit_mismatches||fp_mismatches?"false":"true",cases,cpu_cases,bit_mismatches,fp_mismatches,checksum);
    return bit_mismatches||fp_mismatches?1:0;
}
