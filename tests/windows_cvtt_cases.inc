/* Separate scalar CVTT oracle; rounded CVT and packed conversions are outside this mode. */
#define DECLARE(n) uint32_t n##_ss32(float);uint64_t n##_ss64(float);uint32_t n##_sd32(double);uint64_t n##_sd64(double)
DECLARE(cvtt_native);
#undef DECLARE
#define CVTT_PAIR(name,type,result,helper) \
__declspec(noinline) result cvtt_after_##name(type v){return (result)helper(v);} \
static __declspec(noinline) result cvtt_before_##name(type v){return (result)(helper)(v);}
CVTT_PAIR(ss32,float,uint32_t,sse_cvtt_i32)
CVTT_PAIR(ss64,float,uint64_t,sse_cvtt_i64)
CVTT_PAIR(sd32,double,uint32_t,sse_cvtt_i32)
CVTT_PAIR(sd64,double,uint64_t,sse_cvtt_i64)
#undef CVTT_PAIR
static void cvtt_test_generic_selection(void) {
    float f[]={1,2};double d[]={3,4};unsigned i=0;
    assert(sse_cvtt_i32(f[i++])==1 && i==1);i=0;assert(sse_cvtt_i64(d[i++])==3 && i==1);
    int32_t (*p32)(double)=sse_cvtt_i32;int64_t (*p64)(double)=sse_cvtt_i64;
    assert(p32(2.0)==2 && p64(3.0)==3 && sse_cvtt_i32(4)==4 && sse_cvtt_i64(5)==5);
}
static uint64_t cvtt_state=UINT64_C(0x42756389abdcfe01);
static uint64_t cvtt_random64(void){cvtt_state^=cvtt_state<<13;cvtt_state^=cvtt_state>>7;return cvtt_state^=cvtt_state<<17;}
static uint64_t cvtt_invoke(unsigned variant,unsigned op,uint64_t bits) {
    float f;double d;uint32_t low=(uint32_t)bits;memcpy(&f,&low,4);memcpy(&d,&bits,8);
#define SELECT(n) switch(op){case 0:return n##_ss32(f);case 1:return n##_ss64(f);case 2:return n##_sd32(d);case 3:return n##_sd64(d);default:abort();}
    if(variant==0){SELECT(cvtt_native)}if(variant==1){SELECT(cvtt_after)}SELECT(cvtt_before)
#undef SELECT
}
static unsigned cvtt_cases,cvtt_old_bits[4],cvtt_old_status[4];
static void cvtt_check(unsigned op,uint64_t bits,unsigned mode,int compare_old) {
    unsigned saved=_mm_getcsr();_mm_setcsr(mode);uint64_t expected=cvtt_invoke(0,op,bits);unsigned expected_status=_mm_getcsr();
    _mm_setcsr(mode);uint64_t actual=cvtt_invoke(1,op,bits);unsigned actual_status=_mm_getcsr();assert(actual==expected && actual_status==expected_status);
    if(compare_old){_mm_setcsr(mode);uint64_t old=cvtt_invoke(2,op,bits);cvtt_old_bits[op]+=old!=expected;cvtt_old_status[op]+=_mm_getcsr()!=expected_status;}
    _mm_setcsr(saved);++cvtt_cases;
}
static volatile uint64_t cvtt_destination=UINT64_C(0xe5ef1234aabbccdd);
static unsigned cvtt_trap_op;
static LONG WINAPI cvtt_fault(EXCEPTION_POINTERS *info) {
    unsigned csr=info->ContextRecord->MxCsr;uint64_t input=info->ContextRecord->Xmm0.Low;
    const unsigned char *opcode=info->ExceptionRecord->ExceptionAddress;unsigned length=(cvtt_trap_op&1)?5:4;
    _mm_setcsr(0x1f80);printf("{\"exception_code\":%lu,\"mxcsr\":%u,\"input\":%llu,\"destination\":%llu,\"opcode\":\"",info->ExceptionRecord->ExceptionCode,csr,input,cvtt_destination);
    for(unsigned i=0;i<length;++i)printf("%02x",opcode[i]);puts("\"}");fflush(stdout);ExitProcess(info->ExceptionRecord->ExceptionCode);return EXCEPTION_EXECUTE_HANDLER;
}
static int cvtt_trap_main(unsigned variant,unsigned op,const char *name) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);SetUnhandledExceptionFilter(cvtt_fault);cvtt_trap_op=op;
    uint64_t bits=0;unsigned mode=0x1f00;
    if(!strcmp(name,"qnan"))bits=op<2?UINT64_C(0x7fc12345):UINT64_C(0x7ff8123456789abc);
    else if(!strcmp(name,"snan"))bits=op<2?UINT64_C(0x7fa12345):UINT64_C(0x7ff0123456789abc);
    else if(!strcmp(name,"infinity"))bits=op<2?UINT64_C(0x7f800000):UINT64_C(0x7ff0000000000000);
    else if(!strcmp(name,"range")){uint64_t limits[]={UINT64_C(0x4f000000),UINT64_C(0x5f000000),UINT64_C(0x41e0000000000000),UINT64_C(0x43e0000000000000)};bits=limits[op];}
    else if(!strcmp(name,"inexact")){mode=0x0f80;bits=op<2?UINT64_C(0x3fc00000):UINT64_C(0x3ff8000000000000);}
    else if(!strcmp(name,"denormal")){mode=0x1e80;bits=1;}
    else return 2;
    unsigned saved=_mm_getcsr();_mm_setcsr(mode);cvtt_destination=cvtt_invoke(variant,op,bits);unsigned status=_mm_getcsr();_mm_setcsr(saved);
    printf("{\"exception_code\":0,\"mxcsr\":%u,\"result\":%llu}\n",status,cvtt_destination);return 0;
}
int cvtt_main(int argc,char **argv) {
    if(argc==5 && !strcmp(argv[1],"--trap"))return cvtt_trap_main((unsigned)strtoul(argv[2],NULL,10),(unsigned)strtoul(argv[3],NULL,10),argv[4]);
    assert(argc==1);cvtt_test_generic_selection();SetLastError(0xe5ef1234);
    uint32_t floats[544]={0,0x80000000,0x3f800000,0xbf800000,0x3fc00000,0xbfc00000,0x7f800000,0xff800000,
        0x7fc12345,0xffc12345,0x7fa12345,0xffa12345,1,0x80000001,0x007fffff,0x807fffff,
        0x00800000,0x80800000,0x7f7fffff,0xff7fffff,0x4effffff,0x4f000000,0x4f000001,0xcf000000,
        0xceffffff,0xcf000001,0x5effffff,0x5f000000,0xdf000000,0x5f000001,0xdeffffff,0xdf000001};
    uint64_t doubles[552]={0,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0xbff0000000000000),UINT64_C(0x3ff8000000000000),UINT64_C(0xbff8000000000000),UINT64_C(0x7ff0000000000000),UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff8123456789abc),UINT64_C(0xfff8123456789abc),UINT64_C(0x7ff0123456789abc),UINT64_C(0xfff0123456789abc),1,UINT64_C(0x8000000000000001),UINT64_C(0x000fffffffffffff),UINT64_C(0x800fffffffffffff),
        UINT64_C(0x0010000000000000),UINT64_C(0x8010000000000000),UINT64_C(0x7fefffffffffffff),UINT64_C(0xffefffffffffffff),UINT64_C(0x41dfffffffffffff),UINT64_C(0x41e0000000000000),UINT64_C(0x41e0000000000001),UINT64_C(0xc1e0000000000000),
        UINT64_C(0xc1dfffffffffffff),UINT64_C(0xc1e0000000000001),UINT64_C(0x43dfffffffffffff),UINT64_C(0x43e0000000000000),UINT64_C(0xc3e0000000000000),UINT64_C(0x43e0000000000001),UINT64_C(0xc3dfffffffffffff),UINT64_C(0xc3e0000000000001),
        UINT64_C(0xc1e0000000080000),UINT64_C(0xc1e0000000100000),UINT64_C(0x41dfffffffe00000),UINT64_C(0x41dffffffff00000),UINT64_C(0x3fefffffffffffff),UINT64_C(0xbfefffffffffffff),UINT64_C(0x3ff0000000000001),UINT64_C(0xbff0000000000001)};
    for(unsigned i=32;i<544;++i)floats[i]=(uint32_t)cvtt_random64();for(unsigned i=40;i<552;++i)doubles[i]=cvtt_random64();
    for(unsigned rc=0;rc<4;++rc)for(unsigned modebits=0;modebits<4;++modebits)for(unsigned sticky=0;sticky<2;++sticky){unsigned mode=0x1f80|(rc<<13)|((modebits&1)<<6)|((modebits&2)<<14)|(sticky?63:0);for(unsigned op=0;op<4;++op)for(unsigned i=0;i<(op<2?544:552);++i)cvtt_check(op,op<2?floats[i]:doubles[i],mode,1);}
    unsigned masked_cases=cvtt_cases;
    for(unsigned masks=0;masks<64;++masks)for(unsigned rc=0;rc<4;++rc)for(unsigned modebits=0;modebits<4;++modebits)for(unsigned sticky=0;sticky<2;++sticky)for(unsigned op=0;op<4;++op){unsigned mode=(masks<<7)|(rc<<13)|((modebits&1)<<6)|((modebits&2)<<14)|(sticky?63:0);cvtt_check(op,op<2?UINT64_C(0x3f800000):UINT64_C(0x3ff0000000000000),mode,0);}
    assert(GetLastError()==0xe5ef1234);
    printf("{\"success\":true,\"masked_cases\":%u,\"all64_masks_safe_cases\":%u,\"candidate_native_differences\":0,\"LastError_preserved\":true,\"generic_evaluates_once\":true,\"legacy_result_differences\":[%u,%u,%u,%u],\"legacy_status_differences\":[%u,%u,%u,%u]}\n",masked_cases,cvtt_cases-masked_cases,cvtt_old_bits[0],cvtt_old_bits[1],cvtt_old_bits[2],cvtt_old_bits[3],cvtt_old_status[0],cvtt_old_status[1],cvtt_old_status[2],cvtt_old_status[3]);return 0;
}
