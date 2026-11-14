/* Shared lifecycle cases for native Windows and the real ARM64 HLE provider. */
#include <stdint.h>
#include <stdio.h>
typedef struct {uint64_t checks,hash,last_error_changes;} com_case_result;
static void com_record(com_case_result *r,uint32_t value) {
    r->hash=(r->hash^value)*UINT64_C(1099511628211);
}
#define COM_CHECK(condition) do {++result->checks;if(!(condition))return __LINE__;} while(0)
static int com_cases(com_case_result *result) {
    static const uint64_t high_flags[]={64,256,UINT64_C(0x80000000),UINT64_C(0xffffffff),UINT64_C(0x100000000),UINT64_C(0x100000002)};
    result->hash=UINT64_C(1469598103934665603);
    test_uninit();COM_CHECK(test_query()==UINT32_MAX);
    for(uint32_t initial=0;initial<16;initial+=2){
        uint32_t model=initial&2u;
        uint32_t hr=test_init(0,0,initial,result);com_record(result,hr);COM_CHECK(hr==0);COM_CHECK(test_query()==model);
        for(unsigned trial=0;trial<38;++trial){
            uint64_t flags=trial<32?trial:high_flags[trial-32];
            uint32_t expected=((uint32_t)flags&~14u)?UINT32_C(0x80070057):(((uint32_t)flags&2u)==model?1u:UINT32_C(0x80010106));
            hr=test_init(0,0,flags,result);com_record(result,hr);COM_CHECK(hr==expected);COM_CHECK(test_query()==model);
            if(!(hr&UINT32_C(0x80000000)))test_uninit();
        }
        for(unsigned reserved=0;reserved<2;++reserved){
            uint64_t address=reserved?UINT64_C(0x100000000):1;
            hr=test_init(0,address,initial,result);com_record(result,hr);COM_CHECK(hr==UINT32_C(0x80070057));COM_CHECK(test_query()==model);
            hr=test_init(1,address,0,result);com_record(result,hr);COM_CHECK(hr==UINT32_C(0x80070057));COM_CHECK(test_query()==model);
        }
        hr=test_init(1,0,UINT64_C(0xffffffffffffffff),result);com_record(result,hr);
        COM_CHECK(hr==(model?1u:UINT32_C(0x80010106)));
        if(!(hr&UINT32_C(0x80000000)))test_uninit();
        /* Keep two successful references and prove failures never add one. */
        hr=test_init(0,0,initial^12u,result);com_record(result,hr);COM_CHECK(hr==1);
        hr=test_init(0,0,initial^2u,result);com_record(result,hr);COM_CHECK(hr==UINT32_C(0x80010106));
        test_uninit();COM_CHECK(test_query()==model);
        test_uninit();COM_CHECK(test_query()==UINT32_MAX);
        hr=test_init(0,0,initial^2u,result);com_record(result,hr);COM_CHECK(hr==0);COM_CHECK(test_query()==(model^2u));
        test_uninit();test_uninit();COM_CHECK(test_query()==UINT32_MAX);
    }
    for(unsigned reserved=0;reserved<2;++reserved){
        uint64_t address=reserved?UINT64_C(0x100000000):1;
        uint32_t hr=test_init(0,address,2,result);com_record(result,hr);COM_CHECK(hr==UINT32_C(0x80070057));COM_CHECK(test_query()==UINT32_MAX);
    }
    for(unsigned trial=1;trial<32;trial+=2){
        uint32_t hr=test_init(0,0,trial,result);com_record(result,hr);COM_CHECK(hr==UINT32_C(0x80070057));COM_CHECK(test_query()==UINT32_MAX);
    }
    return 0;
}
#undef COM_CHECK
