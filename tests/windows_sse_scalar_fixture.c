#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "cpu64.h"
#undef FROSTPUNK_WINDOWS_HOST
#define frostpunk_addss baseline_addss
#define frostpunk_mulss baseline_mulss
#define frostpunk_sse_result baseline_result
#undef FROSTPUNK_SSE_SCALAR_H
#include "sse_scalar.h"
#undef frostpunk_addss
#undef frostpunk_mulss
#undef frostpunk_sse_result
#define FROSTPUNK_WINDOWS_HOST 1
typedef float (*scalar_fn)(float,float);
typedef uint64_t (*oracle_fn)(uint32_t,uint32_t,uint32_t);
uint64_t fp_addss_oracle(uint32_t,uint32_t,uint32_t);
uint64_t fp_mulss_oracle(uint32_t,uint32_t,uint32_t);
static __declspec(noinline) float old_add(float a,float b){return baseline_addss(a,b);}
static __declspec(noinline) float old_mul(float a,float b){return baseline_mulss(a,b);}
__declspec(noinline) float fp_scalar_add(float a,float b){return frostpunk_addss(a,b);}
__declspec(noinline) float fp_scalar_mul(float a,float b){return frostpunk_mulss(a,b);}
static uint64_t run(scalar_fn fn,uint32_t a,uint32_t b,unsigned mode) {
    float aa,bb;memcpy(&aa,&a,4);memcpy(&bb,&b,4);unsigned saved=_mm_getcsr();_mm_setcsr(mode);
    float value=fn(aa,bb);unsigned after=_mm_getcsr();_mm_setcsr(saved);uint32_t bits;memcpy(&bits,&value,4);
    return ((uint64_t)after<<32)|bits;
}
static unsigned random_state=0x3bd54127;
static unsigned random32(void){unsigned x=random_state;x^=x<<13;x^=x>>17;x^=x<<5;return random_state=x;}
static unsigned cases,old_bits,old_flags,examples;
static scalar_fn before[]={old_add,old_mul},after[]={fp_scalar_add,fp_scalar_mul};
static oracle_fn native[]={fp_addss_oracle,fp_mulss_oracle};
static void check(unsigned op,uint32_t a,uint32_t b,unsigned mode) {
    uint64_t expected=native[op](a,b,mode),actual=run(after[op],a,b,mode),old=run(before[op],a,b,mode);
    assert(actual==expected);
    old_bits+=(uint32_t)old!=(uint32_t)expected;old_flags+=(unsigned)(old>>32)!=(unsigned)(expected>>32);++cases;
    if(old!=expected && examples++<8)printf("%s{\"op\":%u,\"a\":%u,\"b\":%u,\"mode\":%u,\"old\":%llu,\"native\":%llu}",examples>1?",":"",op,a,b,mode,old,expected);
}
typedef struct {const char *name;unsigned op,a,b,mode;} trap_case;
static const trap_case traps[]={
    {"add_snan",0,0,0x7fa00001,0x1f00},{"mul_snan",1,0x3f800000,0x7fa00001,0x1f00},
    {"mul_invalid",1,0,0x7f800000,0x1f00},{"add_denormal",0,1,0x3f800000,0x1e80},
    {"add_overflow",0,0x7f7fffff,0x7f7fffff,0x1b80},{"mul_overflow",1,0x7f7fffff,0x40000000,0x1b80},
    {"mul_underflow",1,0x00800000,0x00800000,0x1780},{"add_inexact",0,0x3f800000,0x33000000,0x0f80},
    {"mul_inexact",1,0x3f8ccccd,0x3f8ccccd,0x0f80}};
static LONG WINAPI trap_filter(EXCEPTION_POINTERS *info) {
    DWORD code=info->ExceptionRecord->ExceptionCode;CONTEXT *context=info->ContextRecord;
    unsigned csr=context->MxCsr,a=(unsigned)context->Xmm0.Low,b=(unsigned)context->Xmm1.Low;
    const unsigned char *opcode=info->ExceptionRecord->ExceptionAddress;_mm_setcsr(0x1f80);
    printf("{\"exception_code\":%lu,\"mxcsr\":%u,\"destination\":%u,\"source\":%u,\"opcode\":\"%02x%02x%02x%02x\"}\n",
        code,csr,a,b,opcode[0],opcode[1],opcode[2],opcode[3]);fflush(stdout);ExitProcess(code);
    return EXCEPTION_EXECUTE_HANDLER;
}
static int trap_main(const char *implementation,const char *name) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);SetUnhandledExceptionFilter(trap_filter);
    for(unsigned i=0;i<sizeof(traps)/sizeof(*traps);++i)if(!strcmp(name,traps[i].name)) {
        const trap_case *t=&traps[i];uint64_t value;
        if(!strcmp(implementation,"native"))value=native[t->op](t->a,t->b,t->mode);
        else if(!strcmp(implementation,"candidate"))value=run(after[t->op],t->a,t->b,t->mode);
        else if(!strcmp(implementation,"fallback"))value=run(before[t->op],t->a,t->b,t->mode);
        else return 2;
        printf("{\"exception_code\":0,\"bits\":%u,\"mxcsr\":%u}\n",(unsigned)value,(unsigned)(value>>32));return 0;
    }
    return 2;
}

float fp_rcpss_oracle(float),fp_rsqrtss_oracle(float);
void fp_rcpps_oracle(const XMM *,XMM *),fp_rsqrtps_oracle(const XMM *,XMM *);
__declspec(noinline) float fp_scalar_rcp(float v){return sse_rcp(v);}
__declspec(noinline) float fp_scalar_rsqrt(float v){return sse_rsqrt(v);}
__declspec(noinline) void fp_packed_rcp(const XMM *s,XMM *d){for(unsigned i=0;i<4;++i)d->f32[i]=sse_rcp(s->f32[i]);}
__declspec(noinline) void fp_packed_rsqrt(const XMM *s,XMM *d){for(unsigned i=0;i<4;++i)d->f32[i]=sse_rsqrt(s->f32[i]);}
static __declspec(noinline) float old_rcp(float v){return sse_flush_denormal(1.0f/sse_flush_denormal(v));}
static __declspec(noinline) float old_rsqrt(float v){return sse_flush_denormal(1.0f/sqrtf(sse_flush_denormal(v)));}
static uint32_t float_bits(float v){uint32_t result;memcpy(&result,&v,4);return result;}
static int reciprocal_main(void) {
    typedef float (*unary_fn)(float);
    typedef void (*packed_fn)(const XMM *,XMM *);
    unary_fn previous[]={old_rcp,old_rsqrt},current[]={fp_scalar_rcp,fp_scalar_rsqrt},oracle[]={fp_rcpss_oracle,fp_rsqrtss_oracle};
    packed_fn packed[]={fp_packed_rcp,fp_packed_rsqrt},packed_oracle[]={fp_rcpps_oracle,fp_rsqrtps_oracle};
    uint32_t values[544]={0,0x80000000,0x7f800000,0xff800000,0x7fc00000,0xffc00000,0x7f800001,0xff800001,
        0x7fffffff,0xffffffff,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,
        0x3f800000,0xbf800000,0x40000000,0xc0000000,0x40400000,0xc0400000,0x7f7fffff,0xff7fffff,
        0x3f7fffff,0xbf7fffff,0x3f800001,0xbf800001,0x00800001,0x80800001,0x3eaaaaab,0xbeaaaaab};
    for(unsigned i=32;i<544;++i)values[i]=random32();
    unsigned saved=_mm_getcsr(),configurations=0,scalar_cases=0,packed_cases=0,old_result[2]={0},old_status[2]={0};
    SetLastError(0xe5ef1234);
    for(unsigned masks=0;masks<64;++masks)for(unsigned rounding=0;rounding<4;++rounding)
    for(unsigned denormals=0;denormals<4;++denormals)for(unsigned sticky=0;sticky<2;++sticky) {
        unsigned mode=(masks<<7)|(rounding<<13)|((denormals&1)<<6)|((denormals&2)<<14)|(sticky?63:0);++configurations;
        for(unsigned i=0;i<544;++i)for(unsigned op=0;op<2;++op) {
            float value;memcpy(&value,&values[i],4);
            _mm_setcsr(mode);uint32_t expected=float_bits(oracle[op](value));assert(_mm_getcsr()==mode);
            _mm_setcsr(mode);assert(float_bits(current[op](value))==expected && _mm_getcsr()==mode);++scalar_cases;
            XMM input,a,b;for(unsigned lane=0;lane<4;++lane)input.u32[lane]=values[(i+lane)%544];
            _mm_setcsr(mode);packed_oracle[op](&input,&a);assert(_mm_getcsr()==mode);
            _mm_setcsr(mode);packed[op](&input,&b);assert(!memcmp(&a,&b,sizeof(a)) && _mm_getcsr()==mode);++packed_cases;
            /* Legacy approximation differences are observations, with exceptions masked. */
            if(masks==63){_mm_setcsr(mode);old_result[op]+=float_bits(previous[op](value))!=expected;old_status[op]+=_mm_getcsr()!=mode;}
        }
    }
    _mm_setcsr(saved);assert(GetLastError()==0xe5ef1234);
    printf("{\"success\":true,\"MXCSR_configurations\":%u,\"all64_exception_masks\":true,\"scalar_cases\":%u,\"packed_cases\":%u,\"native_MXCSR_unchanged\":true,\"LastError_preserved\":true,\"legacy_result_differences\":[%u,%u],\"legacy_status_differences\":[%u,%u]}\n",configurations,scalar_cases,packed_cases,old_result[0],old_result[1],old_status[0],old_status[1]);
    return 0;
}

#include "windows_cvtt_cases.inc"

int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"--cvtt"))return cvtt_main(argc-1,argv+1);
    if(argc==4 && !strcmp(argv[1],"--trap"))return trap_main(argv[2],argv[3]);
    if(argc==2 && !strcmp(argv[1],"--reciprocal"))return reciprocal_main();
    if(argc!=1)return 2;
    unsigned saved=_mm_getcsr();uint32_t special[]={0,0x80000000,0x3f800000,0xbf800000,0x7f800000,0xff800000,0x7fc00001,0xffc12345,0x7fa00001,0xffa54321,0x7f800001,0xff800001,1,0x80000001,0x007fffff,0x807fffff,0x00800000,0x80800000,0x7f7fffff,0xff7fffff,0x3f000000,0xbf000000,0x33800000,0x80800001};
    printf("{\"baseline_examples\":[");
    for(unsigned rc=0;rc<4;++rc)for(unsigned daz=0;daz<2;++daz)for(unsigned ftz=0;ftz<2;++ftz)for(unsigned sticky=0;sticky<2;++sticky) {
        unsigned mode=0x1f80|(rc<<13)|(daz<<6)|(ftz<<15)|(sticky?0x3f:0);
        for(unsigned op=0;op<2;++op) {
            for(unsigned i=0;i<24;++i)for(unsigned j=0;j<24;++j)check(op,special[i],special[j],mode);
            for(unsigned i=0;i<10000;++i){uint32_t a=random32(),b=random32();check(op,a,b,mode);}
        }
    }
    unsigned unmasked=0;
    for(unsigned masks=0;masks<64;++masks)for(unsigned rc=0;rc<4;++rc)for(unsigned daz=0;daz<2;++daz)for(unsigned ftz=0;ftz<2;++ftz)for(unsigned op=0;op<2;++op) {
        unsigned mode=(masks<<7)|(rc<<13)|(daz<<6)|(ftz<<15);check(op,0x3f800000,0x40000000,mode);++unmasked;
    }
    unsigned aliases=0;
    for(unsigned op=0;op<2;++op)for(unsigned i=0;i<256;++i) {
        CPU c;for(unsigned byte=0;byte<sizeof(c);++byte)((unsigned char *)&c)[byte]=(unsigned char)random32();CPU expected=c;
        unsigned d=i&15,s=(i>>4)&15,mode=0x1f80;uint32_t a=c.xmm[d].u32[0],b=c.xmm[s].u32[0];
        uint64_t value=native[op](a,b,mode);expected.xmm[d].u32[0]=(uint32_t)value;_mm_setcsr(mode);SetLastError(0xe5ef1234);
        c.xmm[d].f32[0]=after[op](c.xmm[d].f32[0],c.xmm[s].f32[0]);
        assert(!memcmp(&c,&expected,sizeof(c)) && _mm_getcsr()==(unsigned)(value>>32) && GetLastError()==0xe5ef1234);++aliases;
    }
    _mm_setcsr(saved);
    printf("],\"success\":true,\"cases\":%u,\"baseline_bit_differences\":%u,\"baseline_MXCSR_differences\":%u,\"candidate_native_differences\":0,\"safe_mask_cases\":%u,\"full_CPU_aliases\":%u}\n",cases,old_bits,old_flags,unmasked,aliases);
    return 0;
}
