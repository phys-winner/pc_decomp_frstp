#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {DWORD type;const char *name;DWORD tid,flags;} name_info;
_Static_assert(sizeof(name_info)==24,"Win64 thread name record");
_Static_assert(offsetof(name_info,name)==8&&offsetof(name_info,tid)==16,"record fields");
static _Thread_local const ULONG_PTR *expected;
static _Thread_local DWORD expected_count;
static _Thread_local unsigned callbacks,handlers,failures;
static LONG observe(EXCEPTION_POINTERS *p){
    ++callbacks;EXCEPTION_RECORD *r=p->ExceptionRecord;
    if(r->ExceptionCode!=0x406d1388||r->ExceptionFlags!=0x80||r->NumberParameters!=expected_count||!p->ContextRecord){if(!failures)fprintf(stderr,"record code=%lx flags=%lx params=%lu expected=%lu\n",r->ExceptionCode,r->ExceptionFlags,r->NumberParameters,expected_count);++failures;}
    for(DWORD i=0;i<expected_count;++i)if(r->ExceptionInformation[i]!=expected[i]){if(!failures)fprintf(stderr,"arg%lu=%llx expected=%llx\n",i,(unsigned long long)r->ExceptionInformation[i],(unsigned long long)expected[i]);++failures;}
    return r->ExceptionCode==0x406d1388?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_EXECUTE_HANDLER;
}
static DWORD WINAPI run_cases(void *unused){(void)unused;
    static const char *names[]={"Worker", "JobSystemWorker000", "12345678901\xf0\x9f\x8c\xa8", ""};
    for(unsigned i=0;i<256;++i){ULONG_PTR args[6];for(unsigned j=0;j<6;++j)args[j]=UINT64_C(0xa5a5000000000000)+j;
        name_info info;memset(&info,0xa5,sizeof(info));info.type=0x1000;info.name=names[i%4];info.tid=GetCurrentThreadId();info.flags=0;memcpy(args,&info,sizeof(info));expected=args;expected_count=(i&1)?6:3;
        SetLastError(1927);
        __try{RaiseException(0x406d1388,0,expected_count,args);}
        __except(observe(GetExceptionInformation())){++handlers;}
        if(GetLastError()!=1927){if(!failures)fprintf(stderr,"LastError=%lu\n",GetLastError());++failures;}
    }
    if(failures||handlers||callbacks!=256)fprintf(stderr,"native worker failures=%u handlers=%u callbacks=%u\n",failures,handlers,callbacks);
    return failures||handlers||callbacks!=256?1:0;
}
int main(void){HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,run_cases,NULL,0,NULL);if(!threads[i])return 2;}
    if(WaitForMultipleObjects(4,threads,TRUE,10000)!=WAIT_OBJECT_0)return 3;
    for(unsigned i=0;i<4;++i){DWORD status=1;if(!GetExitCodeThread(threads[i],&status)||status||!CloseHandle(threads[i]))return 4;}
    printf("{\"success\":true,\"notifications\":1024,\"threads\":4,\"record_size\":24,\"NumberParameters3_and6\":true,\"full_ULONG_PTR_array_checked\":true,\"input_exception_flags\":0,\"native_record_software_origin_flags\":128,\"filter_CONTINUE_EXECUTION\":true,\"no_handler_unwind\":true,\"LastError_preserved\":true,\"native_OS_label_equivalence_claimed\":false}\n");return 0;
}
