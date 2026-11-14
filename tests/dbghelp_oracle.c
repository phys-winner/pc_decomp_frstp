#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include "dbghelp_cases.h"
int main(void) {
    uint64_t hash=UINT64_C(1469598103934665603);unsigned checks=0;
    /* No SymInitialize in this process: compare only the actual uninitialized
     * cleanup behavior, not Android's unsupported-initialization policy. */
    for(unsigned n=0;n<64;++n)for(unsigned i=0;i<sizeof(dbghelp_handles)/sizeof(*dbghelp_handles);++i){
        SetLastError(UINT32_C(0x12345678));BOOL ok=SymCleanup((HANDLE)(uintptr_t)dbghelp_handles[i]);DWORD error=GetLastError();
        if(ok||error!=ERROR_INVALID_HANDLE){fprintf(stderr,"DBGHELP_ORACLE_FAIL handle=%llx result=%d error=%lu\n",(unsigned long long)dbghelp_handles[i],ok,(unsigned long)error);return 2;}
        hash=dbghelp_hash(hash,(uint32_t)ok,error);++checks;
    }
    printf("{\"success\":true,\"cleanup_cases\":%u,\"cleanup_result_LastError_hash\":\"%016llx\",\"SymInitialize_tested_as_native_equivalent\":false}\n",checks,(unsigned long long)hash);return 0;
}
