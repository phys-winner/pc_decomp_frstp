#include <stdio.h>
#include <stdlib.h>
#include "../android/host/aot_cpu_float.c"
#include "aot_cpu_sha256.h"
#ifdef _WIN32
#include <xmmintrin.h>
uint32_t fp_cvttpd2dq_native(XMM*,const XMM*,uint32_t);
uint32_t fp_haddps_native(XMM*,const XMM*,const XMM*,uint32_t);
#else
#include "aot_cpu_float_golden.h"
#endif
typedef struct {uint64_t va;const char *text;unsigned operation,destination,source;} float_site;
#include "aot_cpu_float_cases.inc"
static fp_test_sha digest;
static unsigned tested,rejected,raised;
static uint64_t rng=UINT64_C(0xc75b29e0184af36d);
static uint64_t random64(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static void require(int ok,const char *text){if(!ok){fprintf(stderr,"AOT_FLOAT_FAIL %s case=%u\n",text,tested);exit(1);}}
static CPU random_cpu(void){CPU c;unsigned char *p=(unsigned char *)&c;for(size_t i=0;i<sizeof(c);++i)p[i]=(unsigned char)random64();memset(p+444,0,4);return c;}
static void get_environment(uint64_t *a,uint64_t *b){
#ifdef _WIN32
    *a=_mm_getcsr();*b=0;
#else
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(*a),"=r"(*b));
#endif
}
static void set_environment(uint64_t a,uint64_t b){
#ifdef _WIN32
    _mm_setcsr((unsigned)a);(void)b;
#else
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(a),"r"(b):"memory");
#endif
}
static void host_pattern(unsigned mode){
#ifdef _WIN32
    set_environment(0x1f80|(((mode+1)&3)<<13)|((mode&1)?0x35:0x0a)|((mode&2)?0x40:0)|((mode&4)?0x8000:0),0);
#else
    set_environment(((uint64_t)((mode+1)&3)<<22)|((mode&2)?UINT64_C(1)<<24:0)|((mode&4)?UINT64_C(1)<<25:0),mode&1?0x9f:0x12);
#endif
}
static void differential(const float_site *site,CPU c,unsigned mode){
    host_pattern(mode);uint64_t before,status;get_environment(&before,&status);
    unsigned initial_flags=c.mxcsr&63;
#ifdef _WIN32
    CPU expected=c;XMM source=c.xmm[site->source],destination=c.xmm[site->destination];
    expected.mxcsr=site->operation==6?fp_cvttpd2dq_native(&expected.xmm[site->destination],&source,c.mxcsr):
        fp_haddps_native(&expected.xmm[site->destination],&destination,&source,c.mxcsr);
    if(!initial_flags)raised|=expected.mxcsr&63;
#endif
    require(fp_aot_float_operation(&c,site->operation,site->destination,site->source),"valid operation refused");
    uint64_t after,after_status;get_environment(&after,&after_status);require(before==after&&status==after_status,"host FPCR/FPSR/MXCSR not restored");
#ifdef _WIN32
    if(memcmp(&c,&expected,sizeof(c))){fprintf(stderr,"site=%llx opcode=%u guestCSR=%x expectedCSR=%x gotCSR=%x result=%016llx:%016llx expected=%016llx:%016llx\n",
        (unsigned long long)site->va,site->operation,mode,expected.mxcsr,c.mxcsr,
        (unsigned long long)c.xmm[site->destination].u64[1],(unsigned long long)c.xmm[site->destination].u64[0],
        (unsigned long long)expected.xmm[site->destination].u64[1],(unsigned long long)expected.xmm[site->destination].u64[0]);
        require(0,"native full CPU mismatch");}
#else
    if(!initial_flags)raised|=c.mxcsr&63;
#endif
    sha_add(&digest,&c,sizeof(c));++tested;
}
static void reject(const float_site *s,CPU c){CPU before=c;uint64_t a,b,x,y;get_environment(&a,&b);
    require(!fp_aot_float_operation(&c,s->operation,s->destination,s->source),"invalid mode accepted");
    get_environment(&x,&y);require(a==x&&b==y&&!memcmp(&c,&before,sizeof(c)),"rejection mutated CPU/environment");++rejected;}
static void check_site(const float_site *s){
    static const uint64_t doubles[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),
        UINT64_C(0x3ff8000000000000),UINT64_C(0xbff8000000000000),UINT64_C(0x41dfffffffc00000),UINT64_C(0x41e0000000000000),
        UINT64_C(0xc1e0000000000000),UINT64_C(0xc1e0000000200000),UINT64_C(0xc1e0000000180000),1,
        UINT64_C(0x8000000000000001),UINT64_C(0x000fffffffffffff),UINT64_C(0x0010000000000000),
        UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000000123),
        UINT64_C(0x7ff0000000000001),UINT64_C(0xfff0000000000456),UINT64_C(0x7fefffffffffffff)};
    static const uint32_t singles[]={0,0x80000000,0x3f800000,0xbf800000,0x3f000000,0x33800000,0xb3800000,
        0x7f7fffff,0xff7fffff,0x00800000,0x80800000,1,0x80000001,0x007fffff,0x807fffff,
        0x7f800000,0xff800000,0x7fc00123,0xffc00456,0x7f800001,0xff800123};
    for(unsigned mode=0;mode<32;++mode)for(unsigned i=0;i<21;++i)for(unsigned j=0;j<21;++j){
        CPU c=random_cpu();c.mxcsr=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0)|((mode&16)?0x3f:0);
        if(s->operation==6){c.xmm[s->source].u64[0]=doubles[i];c.xmm[s->source].u64[1]=doubles[j];}
        else{c.xmm[s->destination].u32[0]=singles[i];c.xmm[s->destination].u32[1]=singles[j];c.xmm[s->destination].u32[2]=singles[20-i];c.xmm[s->destination].u32[3]=singles[20-j];
            c.xmm[s->source].u32[0]=singles[j];c.xmm[s->source].u32[1]=singles[20-i];c.xmm[s->source].u32[2]=singles[i];c.xmm[s->source].u32[3]=singles[20-j];}
        differential(s,c,mode);
    }
    for(unsigned n=0;n<1024;++n){CPU c=random_cpu();c.mxcsr=0x1f80|((unsigned)random64()&0xffff&~0x1f80u);differential(s,c,n&31);}
    for(unsigned n=0;n<3;++n){CPU c=random_cpu();c.mxcsr=0x1f80;differential(s,c,n*15);}
    for(unsigned bit=7;bit<=12;++bit){CPU c=random_cpu();c.mxcsr=0x1f80&~(1u<<bit);reject(s,c);}
    CPU c=random_cpu();c.mxcsr=0x21f80;reject(s,c);c.mxcsr=0x80001f80;reject(s,c);
}
int main(void){
    require(sizeof(CPU)==448&&offsetof(CPU,xmm)==184&&offsetof(CPU,mxcsr)==440,"CPU layout");
    uint64_t saved,status;get_environment(&saved,&status);sha_init(&digest);
    for(unsigned i=0;i<sizeof(float_cases)/sizeof(*float_cases);++i)check_site(&float_cases[i]);
    const float_site alias={0,"haddps xmm5, xmm5",7,5,5};
    for(unsigned i=0;i<4096;++i){CPU c=random_cpu();c.mxcsr=0x1f80|((unsigned)random64()&0xffff&~0x1f80u);differential(&alias,c,i&31);}
    char result[65];sha_hex(&digest,result);set_environment(saved,status);
#ifndef _WIN32
    require(!strcmp(result,FP_AOT_FLOAT_GOLDEN),"native oracle SHA256 mismatch");
#endif
    printf("{\"success\":true,\"sites\":19,\"native_full_CPU_cases\":%u,\"additional_HADD_alias_cases\":4096,\"negative_gates\":%u,\"raised_exception_bits\":%u,\"float_sha256\":\"%s\",\"original_bytes_executed\":false}\n",tested,rejected,raised,result);return 0;
}
