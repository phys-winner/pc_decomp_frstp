#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
static LOCAL uint64_t fixture_handler;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return fixture_handler;}
#include "expf_matherr.inc"
static unsigned matherr_checks;
static int handler_image(int corrupt){
    const size_t size=0xd871c3;unsigned char *image=calloc(1,size);if(!image)return 0;
    memcpy(image+0xd871c0,"\x33\xc0\xc3",3);if(corrupt>=0)image[0xd871c0+(unsigned)corrupt]^=1;
    int okay=win64_math_init(image,size);free(image);return okay;
}
static int handler_gates(void){
    if(win64_math_init(NULL,0)||win64_math_init("",0xd871c2))return 0;matherr_checks+=2;
    for(int i=0;i<3;++i){if(!handler_image(-1)||handler_image(i))return 0;matherr_checks+=2;}
    if(!handler_image(-1))return 0;++matherr_checks;
    for(unsigned i=0;i<64;++i){
        CPU c,expected;memset(&c,(int)(i*3+1),sizeof(c));expected=c;
        expected.rax=0;expected.rsp+=8;expected.rip=UINT64_C(0x140d871c2);
        expected.cf=0;expected.of=0;expected.sf=0;expected.zf=1;expected.pf=1;
        baseline_matherr(&c);if(memcmp(&c,&expected,sizeof(c)))return 0;++matherr_checks;
    }return 1;
}
typedef struct {uint32_t x,y,result,mxcsr,expected_mxcsr;int32_t expected_errno;uint32_t tag,reserved;} row;
_Static_assert(sizeof(row)==32,"expf row32");
static row *rows;static size_t count;static unsigned worst[4];
static int import_math(CPU *c,const char *dll,const char *name){
    int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "expf_route.inc"
    return 0;
}
static void environment(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static int one(const row *r,size_t index,int statistics){
    fixture_handler=index&4?UINT64_C(0x140d871c0):0;
    CPU c,expected;memset(&c,0xa7,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u32[0]=r->x;c.xmm[1].u32[0]=r->y;expected=c;
    expected.xmm[0].u32[0]=r->result;expected.mxcsr=r->expected_mxcsr;if(index&1)expected.rsp+=8;
    guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=index&1?import_math(&c,"api-ms-win-crt-math-l1-1-0.dll","expf"):win64_math_call(&c,"expf");
    environment(after);int native_errno=errno;uint32_t result=c.xmm[0].u32[0],distance=result>r->result?result-r->result:r->result-result;expected.xmm[0].u32[0]=result;
    if(statistics&&distance>worst[(r->mxcsr>>13)&3u])worst[(r->mxcsr>>13)&3u]=distance;
    int okay=handled&&distance<=4&&!memcmp(&c,&expected,sizeof(c))&&guest_errno==r->expected_errno&&native_errno==2718&&!memcmp(before,after,sizeof(before));
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    if(!okay)fprintf(stderr,"EXPF_FAIL x=%08x y=%08x value=%08x expected=%08x CSR=%x/%x errno=%d/%d host_errno=%d CPU=%d hostFP=%d\n",
        r->x,r->y,c.xmm[0].u32[0],r->result,c.mxcsr,r->expected_mxcsr,guest_errno,r->expected_errno,native_errno,
        memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)));
    return okay;
}
static unsigned consumer_batches,consumer_worst;
static void paired(const uint32_t values[6],uint32_t sigma_bits,uint32_t csr,uint32_t output[3]){
    uint64_t saved[2];environment(saved);float sigma,factor,k;uint32_t kb=0x3ecc4229;memcpy(&sigma,&sigma_bits,4);memcpy(&k,&kb,4);
#if defined(__aarch64__)
    unsigned rc=(csr>>13)&3u;uint64_t mode=(uint64_t)(((rc&1u)<<1)|((rc&2u)>>1))<<22;
    __asm__ volatile("msr fpcr,%[mode]\n\tmsr fpsr,xzr\n\tfdiv %s[out],%s[a],%s[b]"
        :[out]"=&w"(factor):[mode]"r"(mode),[a]"w"(k),[b]"w"(sigma):"memory");
#else
    _mm_setcsr(0x1f80u|(csr&0x6000u));factor=_mm_cvtss_f32(_mm_div_ss(_mm_set_ss(k),_mm_set_ss(sigma)));
#endif
    static const unsigned pair[3][2]={{1,0},{3,2},{4,5}};
    for(unsigned i=0;i<3;++i){float a,b,result;memcpy(&a,&values[pair[i][0]],4);memcpy(&b,&values[pair[i][1]],4);
#if defined(__aarch64__)
        __asm__ volatile("fadd %s[out],%s[a],%s[b]\n\tfmul %s[out],%s[out],%s[f]"
            :[out]"=&w"(result):[a]"w"(a),[b]"w"(b),[f]"w"(factor):"memory");
#else
        result=_mm_cvtss_f32(_mm_mul_ss(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)),_mm_set_ss(factor)));
#endif
        memcpy(&output[i],&result,4);
    }set_environment(saved);
}
static int consumer(const row *group){
    uint32_t actual[6],reference[6],a[3],b[3];
    for(unsigned i=0;i<6;++i){
        if(group[i].tag!=(0x80000000u|i)||group[i].y!=group[0].y)return 0;
        CPU c={0};c.mxcsr=group[i].mxcsr;c.xmm[0].u32[0]=group[i].x;win64_math_call(&c,"expf");actual[i]=c.xmm[0].u32[0];reference[i]=group[i].result;
    }
    paired(actual,group[0].y,group[0].mxcsr,a);paired(reference,group[0].y,group[0].mxcsr,b);
    for(unsigned i=0;i<3;++i){unsigned d=a[i]>b[i]?a[i]-b[i]:b[i]-a[i];if(d>consumer_worst)consumer_worst=d;
        if(d>8){fprintf(stderr,"EXPF_CONSUMER_FAIL sigma=%08x pair=%u value=%08x expected=%08x ULP=%u\n",group[0].y,i,a[i],b[i],d);return 0;}}
    ++consumer_batches;return 1;
}
#if !defined(_WIN32)
static void *worker(void *context){
    size_t seed=(size_t)(uintptr_t)context;uint64_t saved[2],wanted[2];environment(saved);wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*37)%count],i,0)){set_environment(saved);return(void*)1;}
    set_environment(saved);return NULL;
}
#endif
int main(int argc,char **argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};c.mxcsr=0x1f80;c.rip=UINT64_C(0x141012a27);c.xmm[0].u32[0]=0x3fc00000;c.xmm[1].u32[0]=0x3f800000;
        const char *reason=argv[2];c.xmm[0].u32[0]=0xbf800000;
        if(!strcmp(reason,"positive"))c.xmm[0].u32[0]=0x3f800000;
        else if(!strcmp(reason,"unmasked_underflow")){c.xmm[0].u32[0]=0xc2b00000;c.mxcsr&=~2048u;}
        else if(!strcmp(reason,"unmasked_tail_underflow")){c.xmm[0].u32[0]=0xc4e0ffff;c.mxcsr&=~2048u;}
        else if(!strcmp(reason,"unmasked_tail_precision")){c.xmm[0].u32[0]=0xc4e0ffff;c.mxcsr&=~4096u;}
        else if(!strcmp(reason,"NaN"))c.xmm[0].u32[0]=0x7fc00001;
        else if(!strcmp(reason,"sNaN"))c.xmm[0].u32[0]=0xff800001;
        else if(!strcmp(reason,"infinity"))c.xmm[0].u32[0]=0x7f800000;
        else if(!strcmp(reason,"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(reason,"unmasked_precision"))c.mxcsr&=~4096u;
        else if(!strcmp(reason,"unmasked_denormal")){c.xmm[0].u32[0]=0x80000001;c.mxcsr&=~256u;}
        else if(!strcmp(reason,"handler_before_init")){fixture_handler=UINT64_C(0x140d871c0);c.xmm[0].u32[0]=0xc4e0ffff;}
        else if(!strcmp(reason,"handler_bad_init")){if(!handler_image(-1)||handler_image(0))return 2;fixture_handler=UINT64_C(0x140d871c0);c.xmm[0].u32[0]=0xc4e0ffff;}
        else if(!strcmp(reason,"handler_unknown")){if(!handler_image(-1))return 2;fixture_handler=UINT64_C(0x140d871c1);c.xmm[0].u32[0]=0xc4e0ffff;}
        else return 2;win64_math_call(&c,"expf");return 99;
    }
    win64_math_self_test();if(!handler_gates())return 7;FILE *input=fopen(argv[1],"rb");if(!input)return 3;fseek(input,0,SEEK_END);long bytes=ftell(input);rewind(input);
    if(bytes<=0||bytes%sizeof(row))return 4;count=(size_t)bytes/sizeof(row);rows=malloc((size_t)bytes);if(!rows||fread(rows,(size_t)bytes,1,input)!=1)return 5;fclose(input);
    unsigned failures=0;uint64_t saved[2],wanted[2];environment(saved);
    for(size_t i=0;i<count;++i){
#if defined(__aarch64__)
        wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=UINT32_C(0x9fd5)|((i&3u)<<13);wanted[1]=0;
#endif
        set_environment(wanted);if(!one(&rows[i],i,1)&&++failures>=12)break;
    }set_environment(saved);
    if(!failures)for(size_t i=0;i+5<count;++i)if(rows[i].tag==0x80000000u){if(!consumer(&rows[i]))++failures;i+=5;}
    unsigned concurrent=0;
#if !defined(_WIN32)
    if(!failures){pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;
        for(unsigned i=0;i<4;++i){void *result;if(pthread_join(threads[i],&result)||result)++failures;}concurrent=8192;}
#endif
    printf("{\"success\":%s,\"cases\":%zu,\"accepted_ULP_le4\":%zu,\"failures\":%u,\"concurrent_cases\":%u,\"max_ULP_by_RC\":[%u,%u,%u,%u],\"paired_Gaussian_batches\":%u,\"paired_max_ULP\":%u,\"float_XMM0_only\":true,\"full_CPU_import_pop\":true,\"exact_guest_MXCSR_errno\":true,\"native_FP_restored\":true,\"nonpositive_profile_underflow\":true,\"matherr_gates_and_canonical_checks\":%u}\n",
        failures?"false":"true",count,failures?0:count,failures,concurrent,worst[0],worst[1],worst[2],worst[3],consumer_batches,consumer_worst,matherr_checks);free(rows);return failures?1:0;
}
