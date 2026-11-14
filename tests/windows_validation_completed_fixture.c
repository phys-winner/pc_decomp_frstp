#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <wchar.h>
#include "../native/windows/native_validation.h"
#include "../native/windows/callback_stacks.h"

static HANDLE mutation_start,mutation_done,writer_release;
static volatile LONG hook_armed;
static unsigned hook_calls;
static void finish_between_reads(void) {
    if(InterlockedCompareExchange(&hook_armed,0,1)==1) {
        DWORD saved=GetLastError();++hook_calls;
        assert(SetEvent(mutation_start));assert(WaitForSingleObject(mutation_done,30000)==WAIT_OBJECT_0);
        SetLastError(saved);
    }
}
LONG64 fp_test_read_acquire64(LONG64 const volatile *source) {
    LONG64 value=ReadAcquire64(source);finish_between_reads();return value;
}
typedef struct {void *address;DWORD previous;} mutation_state;
static DWORD WINAPI complete_mutation(void *parameter) {
    mutation_state *state=parameter;
    assert(WaitForSingleObject(mutation_start,30000)==WAIT_OBJECT_0);
    fp_native_memory_mutation_begin((uintptr_t)VirtualProtect,(uintptr_t)state->address,1);
    assert(VirtualProtect(state->address,1,PAGE_NOACCESS,&state->previous));
    fp_native_memory_mutation_end();assert(SetEvent(mutation_done));
    /* DLL thread-detach callbacks require its code page restored before this thread exits. */
    assert(WaitForSingleObject(writer_release,30000)==WAIT_OBJECT_0);return 0;
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==3);int code=!wcscmp(argv[2],L"code");assert(code || !wcscmp(argv[2],L"stack"));
    fp_native_validation_init();uint64_t target=0,low=0,high=0,rsp=0,expected;
    mutation_state state={0};
    if(code) {
        HMODULE module=LoadLibraryW(argv[1]);assert(module);
        target=(uintptr_t)GetProcAddress(module,"validation_sum");assert(target);
        assert(fp_native_target_valid(target));state.address=(void *)(uintptr_t)target;expected=0;
    } else {
        unsigned char *block=fp_callback_stack_alloc(0,65536);assert(block);DWORD old;
        assert(VirtualProtect(block,4096,PAGE_NOACCESS,&old));fp_native_validation_invalidate();
        low=(uintptr_t)block;high=low+65536;rsp=high-1024;
        assert(fp_native_stack_commit_limit(rsp,high,low)==low+4096);
        state.address=block+8192;expected=low+12288;
    }
    mutation_start=CreateEventW(NULL,TRUE,FALSE,NULL);mutation_done=CreateEventW(NULL,TRUE,FALSE,NULL);
    writer_release=CreateEventW(NULL,TRUE,FALSE,NULL);assert(mutation_start && mutation_done && writer_release);
    HANDLE writer=CreateThread(NULL,0,complete_mutation,&state,0,NULL);assert(writer);
    InterlockedExchange(&hook_armed,1);SetLastError(0x76ab54cd);
    uint64_t actual=code?(uint64_t)fp_native_target_valid(target):fp_native_stack_commit_limit(rsp,high,low);
    DWORD result_error=GetLastError();assert(hook_calls==1);
    DWORD ignored;assert(VirtualProtect(state.address,1,state.previous,&ignored));fp_native_validation_invalidate();
    assert(SetEvent(writer_release));assert(WaitForSingleObject(writer,30000)==WAIT_OBJECT_0);
    assert(CloseHandle(writer));assert(CloseHandle(mutation_start));assert(CloseHandle(mutation_done));assert(CloseHandle(writer_release));
    int pass=actual==expected && result_error==0x76ab54cd;
    printf("{\"completed_mutation\":true,\"case\":\"%s\",\"hook_calls\":%u,\"actual\":%llu,\"expected\":%llu,\"last_error_preserved\":%s,\"pass\":%s}\n",
           code?"code":"stack",hook_calls,actual,expected,result_error==0x76ab54cd?"true":"false",pass?"true":"false");
    return pass?0:1;
}
