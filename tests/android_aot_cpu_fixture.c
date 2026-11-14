/* Native x64 generates oracle digests; AArch64 checks the same deterministic CPU stream. */
#include <stdio.h>
#include <stdlib.h>
#include "../android/host/aot_cpu.c"
#include "../android/host/aot_cpu_float.c"
#include "aot_cpu_sha256.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <smmintrin.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#include "aot_cpu_golden.h"
#endif

int64_t g_image_delta;
static unsigned tested,integer_tested,floating_dispatch_tested,gates,mutations,guarded,memory_cases;
static uint64_t random_state=UINT64_C(0x41f782bae3c9506d);
static fp_test_sha packed_digest,integer_digest,floating_dispatch_digest;
static void require(int ok,const char *reason){if(!ok){fprintf(stderr,"AOT_CPU_FAIL %s\n",reason);exit(1);}}
static uint64_t random64(void){random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return random_state;}
static CPU random_cpu(void){CPU c;unsigned char *p=(unsigned char *)&c;for(size_t i=0;i<sizeof(c);++i)p[i]=(unsigned char)random64();memset(p+444,0,4);return c;}
void frostpunk_unsupported(uint64_t va,const char *text){fprintf(stderr,"AOT_CPU_STOP %llx %s\n",(unsigned long long)va,text);exit(1);}
typedef struct {uint64_t va;unsigned destination,source,operation,memory;int source_gpr;uint64_t source_disp,source_va;void(*repaired)(CPU*);} compare_case;
#include "aot_cpu_compare_cases.inc"
#ifdef _WIN32
uint32_t fp_cmpeqps_native(XMM*,const XMM*,const XMM*,uint32_t);
uint32_t fp_cmpltps_native(XMM*,const XMM*,const XMM*,uint32_t);
uint32_t fp_cmpeqpd_native(XMM*,const XMM*,const XMM*,uint32_t);
uint32_t fp_cmpltpd_native(XMM*,const XMM*,const XMM*,uint32_t);
uint32_t fp_cmpnlepd_native(XMM*,const XMM*,const XMM*,uint32_t);
uint32_t fp_cvttpd2dq_native(XMM*,const XMM*,uint32_t);
uint32_t fp_haddps_native(XMM*,const XMM*,const XMM*,uint32_t);
static void packed_oracle(CPU *c,unsigned d,const XMM *source,unsigned op){
    uint32_t(*f[])(XMM*,const XMM*,const XMM*,uint32_t)={fp_cmpeqps_native,fp_cmpltps_native,fp_cmpeqpd_native,fp_cmpltpd_native,fp_cmpnlepd_native};
    XMM a=c->xmm[d],b=*source;c->mxcsr=f[op](&c->xmm[d],&a,&b,c->mxcsr);
}
__attribute__((target("sse4.1"))) static XMM integer_oracle(unsigned op,XMM a,XMM b){__m128i d,s,r;memcpy(&d,&a,16);memcpy(&s,&b,16);
    switch(op){case 0:r=_mm_mullo_epi32(d,s);break;case 1:r=_mm_cvtepu8_epi32(s);break;case 2:r=_mm_abs_epi32(s);break;case 3:r=_mm_sad_epu8(d,s);break;default:r=_mm_cvtepi32_epi64(s);break;}XMM out;memcpy(&out,&r,16);return out;}
static void environment(uint64_t *a,uint64_t *b){*a=_mm_getcsr();*b=0;}
#else
static void environment(uint64_t *a,uint64_t *b){__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(*a),"=r"(*b));}
#endif
static void preserve(uint64_t a,uint64_t b){uint64_t x,y;environment(&x,&y);require(x==a&&y==b,"host FP control/status changed");}
static void differential(CPU c,unsigned d,const XMM *source,unsigned op,void(*function)(CPU*),int normalize) {
    CPU before=c;uint64_t control,status;environment(&control,&status);
#ifdef _WIN32
    CPU expected=c;packed_oracle(&expected,d,source,op);
#endif
    if(function)function(&c);else require(fp_aot_packed_compare(&c,d,source,op),"packed helper refused");
    preserve(control,status);
#ifdef _WIN32
    require(!memcmp(&c,&expected,sizeof(c)),"packed native full CPU mismatch");
#endif
    if(normalize>=0){((uint64_t *)&c.rax)[normalize]=UINT64_C(0x4ea0000000000000)+(unsigned)normalize;}
    sha_add(&packed_digest,&c,sizeof(c));++tested;(void)before;
}
static const uint32_t edges[]={0,0x80000000,0x3f800000,0xbf800000,0x3f000000,0x7f7fffff,0xff7fffff,0x00800000,0x80800000,1,0x80000001,0x007fffff,0x807fffff,0x7f800000,0xff800000,0x7fc00123,0xffc00456,0x7f800001,0xff800123};
static const uint64_t doubles[]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),UINT64_C(0x3fe0000000000000),UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff),UINT64_C(0x0010000000000000),UINT64_C(0x8010000000000000),1,UINT64_C(0x8000000000000001),UINT64_C(0x000fffffffffffff),UINT64_C(0x800fffffffffffff),UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),UINT64_C(0x7ff8000000000123),UINT64_C(0xfff8000000000456),UINT64_C(0x7ff0000000000001),UINT64_C(0xfff0000000000123)};
static void packed_tests(void){
    for(unsigned n=0;n<sizeof(compare_cases)/sizeof(*compare_cases);++n){const compare_case *s=&compare_cases[n];
        for(unsigned mode=0;mode<32;++mode)for(unsigned i=0;i<19;++i)for(unsigned j=0;j<19;++j){
            CPU c=random_cpu();c.mxcsr=0x1f80|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0)|((mode&16)?0x3f:0);
            _Alignas(16) XMM memory;XMM *input=s->memory?&memory:&c.xmm[s->source];
            if(s->operation<2)for(unsigned k=0;k<4;++k){c.xmm[s->destination].u32[k]=edges[(i+k)%19];input->u32[k]=edges[(j+3-k)%19];}
            else for(unsigned k=0;k<2;++k){c.xmm[s->destination].u64[k]=doubles[(i+k)%19];input->u64[k]=doubles[(j+1-k)%19];}
            if(s->memory){if(s->source_gpr<0)g_image_delta=(int64_t)(uintptr_t)input-(int64_t)s->source_va;
                else ((uint64_t *)&c.rax)[s->source_gpr]=(uintptr_t)input-s->source_disp;++memory_cases;}
            differential(c,s->destination,input,s->operation,s->repaired,s->memory?s->source_gpr:-1);
        }
    }
    for(unsigned op=0;op<5;++op)for(unsigned d=0;d<16;++d)for(unsigned s=0;s<16;++s)for(unsigned n=0;n<64;++n){
        CPU c=random_cpu();c.mxcsr=0x1f80|((unsigned)random64()&0xffff&~0x1f80u);
        /* Pass a pointer into the by-value CPU actually consumed by differential. */
        CPU expected=c;uint64_t control,status;environment(&control,&status);
#ifdef _WIN32
        packed_oracle(&expected,d,&c.xmm[s],op);
#endif
        require(fp_aot_packed_compare(&c,d,&c.xmm[s],op),"aliased generic compare refused");preserve(control,status);
#ifdef _WIN32
        require(!memcmp(&c,&expected,sizeof(c)),"aliased packed native CPU mismatch");
#endif
        sha_add(&packed_digest,&c,sizeof(c));++tested;
    }
}
static void integer_tests(void){
    static const uint32_t edge[]={0,1,2,UINT32_MAX,0x80000000,0x7fffffff,0xffff0000,0x0000ffff};
    for(unsigned site=0;site<sizeof(fp_aot_sites)/sizeof(*fp_aot_sites);++site){const fp_aot_site *s=&fp_aot_sites[site];if(s->operation>5)continue;
        for(unsigned n=0;n<4160;++n){CPU c=random_cpu();
            if(n<64){if(s->operation==5)for(unsigned k=0;k<4;++k)c.xmm[s->source].u32[k]=edge[(n+k)%8];
                else for(unsigned k=0;k<4;++k){c.xmm[s->destination].u32[k]=edge[(n/8+k)%8];c.xmm[s->source].u32[k]=edge[(n%8+k)%8];}}
#ifdef _WIN32
            CPU expected=c;if(s->operation==5){uint64_t value=c.xmm[s->source].u32[s->lane];memcpy((unsigned char *)&expected+s->destination*8,&value,8);}
            else expected.xmm[s->destination]=integer_oracle(s->operation,c.xmm[s->destination],c.xmm[s->source]);
#endif
            uint64_t control,status;environment(&control,&status);require(fp_aot_cpu_instruction(&c,s->va,s->text),"integer helper refused");preserve(control,status);
#ifdef _WIN32
            require(!memcmp(&c,&expected,sizeof(c)),"integer native full CPU mismatch");
#endif
            sha_add(&integer_digest,&c,sizeof(c));++integer_tested;
        }
        CPU c=random_cpu(),before=c;require(!fp_aot_cpu_instruction(&c,s->va,"incorrect instruction"),"text mismatch accepted");require(!memcmp(&c,&before,sizeof(c)),"rejection changed CPU");++gates;
    }
}
static void float_dispatch_tests(void){
    for(unsigned i=0;i<sizeof(fp_aot_sites)/sizeof(*fp_aot_sites);++i){const fp_aot_site *s=&fp_aot_sites[i];if(s->operation<6)continue;
        for(unsigned n=0;n<128;++n){CPU c=random_cpu();c.mxcsr=0x1f80|((unsigned)random64()&0xffff&~0x1f80u);
#ifdef _WIN32
            CPU expected=c;XMM source=c.xmm[s->source],destination=c.xmm[s->destination];
            expected.mxcsr=s->operation==6?fp_cvttpd2dq_native(&expected.xmm[s->destination],&source,c.mxcsr):fp_haddps_native(&expected.xmm[s->destination],&destination,&source,c.mxcsr);
#endif
            uint64_t control,status;environment(&control,&status);require(fp_aot_cpu_instruction(&c,s->va,s->text),"float VA dispatch refused");preserve(control,status);
#ifdef _WIN32
            require(!memcmp(&c,&expected,sizeof(c)),"float dispatch native full CPU mismatch");
#endif
            sha_add(&floating_dispatch_digest,&c,sizeof(c));++floating_dispatch_tested;
        }
        for(unsigned bit=7;bit<=12;++bit){CPU c=random_cpu();c.mxcsr=0x1f80&~(1u<<bit);CPU before=c;
            require(!fp_aot_cpu_instruction(&c,s->va,s->text),"float dispatch accepted unmasked mode");require(!memcmp(&c,&before,sizeof(c)),"float dispatch rejection changed CPU");++gates;}
    }
}
static void *mapping(size_t bytes){
#ifdef _WIN32
    return VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
#else
    void *p=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return p==MAP_FAILED?NULL:p;
#endif
}
static void guard(void *p,size_t bytes){
#ifdef _WIN32
    DWORD old;require(VirtualProtect(p,bytes,PAGE_NOACCESS,&old),"guard protection");
#else
    require(!mprotect(p,bytes,PROT_NONE),"guard protection");
#endif
}
static void guard_tests(void){
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
#endif
    unsigned char *p=mapping(page*2);require(p!=NULL,"guard mapping");guard(p+page,page);
    XMM *source=(XMM *)(p+page-16);source->u64[0]=1;source->u64[1]=UINT64_C(0x7ff0000000000001);
    for(unsigned op=0;op<5;++op){CPU c=random_cpu();c.mxcsr=0x1f80;CPU expected=c;
#ifdef _WIN32
        packed_oracle(&expected,0,source,op);
#endif
        require(fp_aot_packed_compare_memory(&c,0,(uintptr_t)source,op),"16B guard refused");
#ifdef _WIN32
        require(!memcmp(&c,&expected,sizeof(c)),"guarded packed result");
#endif
        ++guarded;
        c=expected;c.mxcsr=0x1f80;CPU before=c;require(!fp_aot_packed_compare_memory(&c,0,(uintptr_t)source-1,op),"unaligned accepted");require(!memcmp(&c,&before,sizeof(c)),"alignment rejection mutation");++gates;
        for(unsigned bit=7;bit<=12;++bit){c=before;c.mxcsr&=~(1u<<bit);CPU saved=c;require(!fp_aot_packed_compare_memory(&c,0,(uintptr_t)(p+page),op),"unmasked guard accepted");require(!memcmp(&c,&saved,sizeof(c)),"unmasked mutated");++gates;}
        c=before;c.mxcsr|=0x20000;before=c;require(!fp_aot_packed_compare(&c,0,source,op),"AMD MM accepted");require(!memcmp(&c,&before,sizeof(c)),"invalid MXCSR mutation");++gates;
    }
    const fp_aot_site *site=NULL;for(unsigned i=0;i<sizeof(fp_aot_sites)/sizeof(*fp_aot_sites);++i)if(fp_aot_sites[i].operation==1&&fp_aot_sites[i].source==1&&fp_aot_sites[i].destination==0){site=&fp_aot_sites[i];break;}
    require(site!=NULL,"PMOVZXBD guard site");CPU *c=(CPU *)(p+page-4-(offsetof(CPU,xmm)+16));
    for(unsigned n=0;n<256;++n){unsigned char input[4]={(unsigned char)n,0,255,(unsigned char)(255-n)};memcpy(&c->xmm[1],input,4);
        require(fp_aot_cpu_instruction(c,site->va,site->text),"four-byte guard refused");for(unsigned k=0;k<4;++k)require(c->xmm[0].u32[k]==input[k],"four-byte guard mismatch");++guarded;}
}
int main(void){
    require(sizeof(CPU)==448&&offsetof(CPU,xmm)==184&&offsetof(CPU,mxcsr)==440,"CPU layout");
    fp_test_sha known;char check[65];sha_init(&known);sha_add(&known,"abc",3);sha_hex(&known,check);require(!strcmp(check,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"SHA256 vector");
    size_t size=0x1d00000;unsigned char *image=mapping(size);require(image!=NULL,"synthetic NX image");
    wr16((uintptr_t)image,0x5a4d);wr32((uintptr_t)image+0x3c,0x80);wr32((uintptr_t)image+0x80,0x4550);wr16((uintptr_t)image+0x86,1);wr16((uintptr_t)image+0x94,0xf0);wr16((uintptr_t)image+0x98,0x20b);wr64((uintptr_t)image+0xb0,UINT64_C(0x140000000));
    memcpy(image+0x188,".text\0\0\0",8);wr32((uintptr_t)image+0x190,(uint32_t)size-0x1000);wr32((uintptr_t)image+0x194,0x1000);wr32((uintptr_t)image+0x1ac,0x60000020);
    for(unsigned i=0;i<sizeof(fp_aot_gates)/sizeof(*fp_aot_gates);++i){const fp_aot_gate *g=&fp_aot_gates[i];memcpy(image+(size_t)(g->va-UINT64_C(0x140000000)),g->bytes,g->size);}
    require(fp_aot_cpu_init(image,size),"valid source gates");sha_init(&packed_digest);sha_init(&integer_digest);sha_init(&floating_dispatch_digest);packed_tests();integer_tests();guard_tests();float_dispatch_tests();
    for(unsigned i=0;i<sizeof(fp_aot_gates)/sizeof(*fp_aot_gates);++i){const fp_aot_gate *g=&fp_aot_gates[i];unsigned char *at=image+(size_t)(g->va-UINT64_C(0x140000000));
        for(unsigned b=0;b<g->size;++b){at[b]^=0x80;require(!fp_aot_cpu_init(image,size),"opcode mutation accepted");at[b]^=0x80;++mutations;}}
    require(fp_aot_cpu_init(image,size),"source restore");char packed[65],integer[65],floating[65];sha_hex(&packed_digest,packed);sha_hex(&integer_digest,integer);sha_hex(&floating_dispatch_digest,floating);
#ifndef _WIN32
    require(!strcmp(packed,FP_AOT_PACKED_GOLDEN)&&!strcmp(integer,FP_AOT_INTEGER_GOLDEN)&&!strcmp(floating,FP_AOT_FLOAT_DISPATCH_GOLDEN),"native oracle digest mismatch");
#endif
    printf("{\"success\":true,\"packed_full_CPU_cases\":%u,\"integer_cases\":%u,\"floating_dispatch_cases\":%u,\"guarded_reads\":%u,\"memory_cases\":%u,\"negative_gates\":%u,\"opcode_mutations\":%u,\"packed_sha256\":\"%s\",\"integer_sha256\":\"%s\",\"floating_dispatch_sha256\":\"%s\",\"original_bytes_executed\":false}\n",tested,integer_tested,floating_dispatch_tested,guarded,memory_cases,gates,mutations,packed,integer,floating);return 0;
}
