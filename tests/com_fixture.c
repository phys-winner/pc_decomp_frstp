/* Use the real core LastError/TEB implementation; discard unrelated providers. */
#include "../android/host/win64.c"
#include <stdatomic.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes) {
    uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;
}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes) {
    if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);
}
static atomic_uint live_contexts;
static _Thread_local int fail_allocation;
static void *com_test_calloc(size_t n,size_t bytes) {
    if(fail_allocation)return NULL;void *p=calloc(n,bytes);if(p)atomic_fetch_add(&live_contexts,1);return p;
}
static void com_test_free(void *p) {if(p)atomic_fetch_sub(&live_contexts,1);free(p);}
#define calloc com_test_calloc
#define free com_test_free
#include "../android/host/win64_com.c"
#undef free
#undef calloc
/* This fixture tests apartments only. Class activation is unhandled here;
 * the separate SDL fixture links the real scoped DirectInput provider. */
int win64_dinput_com_call(CPU *cpu,const char *name,int initialized) {
    (void)cpu;(void)name;(void)initialized;return 0;
}
static uint32_t test_init(unsigned legacy,uint64_t reserved,uint64_t flags,void *result) {
    (void)result;CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=reserved;cpu.rdx=flags;cpu.rsp=UINT64_C(0x1234567812345678);before=cpu;
    win64_set_error(UINT32_C(0x12345678));
    if(!win64_com_call(&cpu,legacy?"CoInitialize":"CoInitializeEx")||win64_get_error()!=UINT32_C(0x12345678))abort();
    uint32_t hr=(uint32_t)cpu.rax;if(cpu.rax!=hr)abort();cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return hr;
}
static void test_uninit(void) {
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;win64_set_error(UINT32_C(0x12345678));
    if(!win64_com_call(&cpu,"CoUninitialize")||memcmp(&cpu,&before,sizeof(cpu))||win64_get_error()!=UINT32_C(0x12345678))abort();
}
static uint32_t test_query(void) {
    if(pthread_once(&apartment_once,apartment_key_initialize)||apartment_key_error)abort();
    apartment_context *context=pthread_getspecific(apartment_key);return context?context->model:UINT32_MAX;
}
#include "com_cases.h"
typedef struct {com_case_result result;int error;} worker_result;
static void *worker(void *opaque) {
    worker_result *out=opaque;out->error=com_cases(&out->result);return NULL;
}
static void *unbalanced_worker(void *unused) {
    (void)unused;if(apartment_initialize(0,2)||apartment_initialize(0,6)!=1)abort();return NULL;
}
int main(void) {
    win64_com_self_test();if(atomic_load(&live_contexts))return 2;
    pthread_t threads[4];worker_result workers[4]={0};uint64_t checks=0;
    for(unsigned i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,&workers[i]))return 3;
    for(unsigned i=0;i<4;++i){if(pthread_join(threads[i],NULL)||workers[i].error){fprintf(stderr,"COM_FIXTURE_FAIL thread=%u line=%d\n",i,workers[i].error);return 4;}
        if(workers[i].result.hash!=workers[0].result.hash)return 5;checks+=workers[i].result.checks;}
    if(atomic_load(&live_contexts))return 6;
    /* Inject allocation failure only at this module's context allocator. */
    fail_allocation=1;if(apartment_initialize(0,2)!=COM_E_OUTOFMEMORY||test_query()!=UINT32_MAX)return 7;
    fail_allocation=0;if(apartment_initialize(0,2))return 8;
    fail_allocation=1;if(apartment_initialize(0,2)!=1)return 9;
    fail_allocation=0;apartment_uninitialize();apartment_uninitialize();
    if(pthread_create(&threads[0],NULL,unbalanced_worker,NULL)||pthread_join(threads[0],NULL)||atomic_load(&live_contexts))return 10;
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;
    if(win64_com_call(&cpu,"CoGetClassObject")||memcmp(&cpu,&before,sizeof(cpu)))return 11;
    printf("{\"success\":true,\"threads\":4,\"checks\":%llu,\"per_thread_hresult_hash\":\"%016llx\",\"CPU_RSP_LastError_preserved\":true,\"thread_exit_releases_context\":true,\"allocation_failure_checked\":true,\"class_activation_supported\":false}\n",(unsigned long long)checks,(unsigned long long)workers[0].result.hash);return 0;
}
