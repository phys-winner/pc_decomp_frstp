#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdint.h>
#include <stdio.h>
static uint32_t test_init(unsigned legacy,uint64_t reserved,uint64_t flags,void *result);
static void test_uninit(void) { CoUninitialize(); }
static uint32_t test_query(void) {
    APTTYPE type;APTTYPEQUALIFIER qualifier;HRESULT hr=CoGetApartmentType(&type,&qualifier);
    if(hr==CO_E_NOTINITIALIZED)return UINT32_MAX;
    if(FAILED(hr))return UINT32_MAX-1;
    /* Another native MTA thread may expose an implicit MTA, even though this
     * thread has no explicit CoInitialize reference to balance. */
    if(qualifier==APTTYPEQUALIFIER_IMPLICIT_MTA)return UINT32_MAX;
    return type==APTTYPE_MTA?0u:(type==APTTYPE_STA||type==APTTYPE_MAINSTA?2u:UINT32_MAX-2);
}
#include "com_cases.h"
static uint32_t test_init(unsigned legacy,uint64_t reserved,uint64_t flags,void *opaque) {
    com_case_result *result=opaque;
    SetLastError(UINT32_C(0x12345678));
    HRESULT hr=legacy?CoInitialize((void *)(uintptr_t)reserved):CoInitializeEx((void *)(uintptr_t)reserved,(DWORD)flags);
    if(GetLastError()!=UINT32_C(0x12345678))++result->last_error_changes;
    return (uint32_t)hr;
}
typedef struct {com_case_result result;int error;} worker_result;
static DWORD WINAPI worker(void *opaque) {
    worker_result *out=opaque;out->error=com_cases(&out->result);return 0;
}
int main(void) {
    HANDLE threads[4];worker_result workers[4]={0};uint64_t checks=0,changes=0;
    for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,&workers[i],0,NULL);if(!threads[i])return 2;}
    if(WaitForMultipleObjects(4,threads,TRUE,INFINITE)!=WAIT_OBJECT_0)return 3;
    for(unsigned i=0;i<4;++i){CloseHandle(threads[i]);if(workers[i].error){fprintf(stderr,"COM_ORACLE_FAIL thread=%u line=%d\n",i,workers[i].error);return 4;}
        if(workers[i].result.hash!=workers[0].result.hash)return 5;checks+=workers[i].result.checks;changes+=workers[i].result.last_error_changes;}
    printf("{\"success\":true,\"threads\":4,\"checks\":%llu,\"per_thread_hresult_hash\":\"%016llx\",\"native_incidental_LastError_changes\":%llu}\n",(unsigned long long)checks,(unsigned long long)workers[0].result.hash,(unsigned long long)changes);return 0;
}
