/* Thread exit must defer freeing the stack on which its FLS destructor runs. */
#define wmain fp_unused_host_wmain
#include "../native/windows/host.c"
#undef wmain
#include <assert.h>
#include <limits.h>
static _Thread_local uint64_t fixture_tls;
static _Thread_local int inject_foreign_limit;
static unsigned nested_foreign_cases;static uint64_t nested_commit_queries;
extern uint64_t fp_churn_call(void *target,uint64_t argument,unsigned *errors);
static void callback_body(CPU *c) {
    assert(c->rbx==0x11 && c->rbp==0x22 && c->rsi==0x33 && c->rdi==0x44);
    assert(c->r12==0x55 && c->r13==0x66 && c->r14==0x77 && c->r15==0x88);
    for(unsigned i=6;i<16;++i)assert(c->xmm[i].u64[0]==UINT64_MAX && c->xmm[i].u64[1]==UINT64_MAX);
    if(inject_foreign_limit && callback_depth==1) {
        uint64_t base=__readgsqword(8),foreign=callbacks[0].native_limit;
        assert(foreign<(uintptr_t)callbacks[0].stack);
        fp_native_stack_stats before,after;fp_native_stack_statistics(&before);
        for(unsigned repeat=0;repeat<2;++repeat) {
            native_stack_bounds(base,foreign);
            unsigned errors=0;uint64_t result=fp_churn_call(game.base,0x24,&errors);
            assert(!errors && result==(UINT64_C(0x24)^UINT64_C(0xe5efcafe)));
        }
        fp_native_stack_statistics(&after);
        assert(__readgsqword(8)==base && __readgsqword(0x10)==(uintptr_t)callbacks[0].stack+4096);
        assert(after.commit_queries-before.commit_queries<=2);
        assert(after.owned_reservation_hits-before.owned_reservation_hits>=1);
        assert(after.owned_commit_hits-before.owned_commit_hits>=1);
        nested_commit_queries+=after.commit_queries-before.commit_queries;++nested_foreign_cases;
    }
    if(c->rcx&1){c->rcx=0x37;dispatch(c,(uintptr_t)ExitThread);abort();}
    returned(c,c->rcx^UINT64_C(0xe5efcafe));
}
const recomp_entry_t recomp_dispatch_table[]={{UINT64_C(0x140000000),callback_body}};
const uint32_t recomp_dispatch_count=1;
const hle_import_t recomp_imports[HLE_COUNT]={{0}};
frostpunk_supplemental_fn frostpunk_supplemental_lookup(uint64_t target){(void)target;return NULL;}
static DWORD WINAPI worker(void *argument) {
    uint64_t value=(uintptr_t)argument;fixture_tls=value^UINT64_C(0x123456789abcdef0);
    uint64_t native_tls=__readgsqword(0x58),base=__readgsqword(8),limit=__readgsqword(0x10);
    void *stack_address=_AddressOfReturnAddress();unsigned errors=0;
    uint64_t result=fp_churn_call(game.base,value,&errors);
    assert(!(value&1) && !errors && result==(value^UINT64_C(0xe5efcafe)));
    assert(native_tls==__readgsqword(0x58) && base==__readgsqword(8));
    /* Windows can legitimately commit an additional native stack guard page. */
    assert(__readgsqword(0x10)<=limit && __readgsqword(0x10)<base);
    assert(_AddressOfReturnAddress()==stack_address && fixture_tls==(value^UINT64_C(0x123456789abcdef0)));
    assert(!callback_depth && !call_depth);return 0;
}
static void reap_probe_native_call(void) {
    uint64_t words[40]={UINT64_C(0xe5efcafe)};
    uint64_t saved_base=guest_stack_base,saved_limit=guest_stack_limit,saved_owner=guest_stack_owner_low;
    guest_stack_limit=guest_stack_owner_low=(uintptr_t)words;guest_stack_base=(uintptr_t)(words+40);
    CPU c={0};c.rsp=(uintptr_t)words;SetLastError(0x77665544);
    dispatch(&c,(uintptr_t)GetCurrentThreadId);
    assert(c.rax==GetCurrentThreadId() && c.rip==words[0] && c.rsp==(uintptr_t)(words+1));
    assert(GetLastError()==0x77665544);
    guest_stack_base=saved_base;guest_stack_limit=saved_limit;guest_stack_owner_low=saved_owner;
}
static DWORD WINAPI reap_probe_worker(void *unused) {
    (void)unused;assert(native_reap_tick==0);
    for(unsigned i=0;i<13;++i)reap_probe_native_call();
    assert(native_reap_tick==13 && fp_callback_stack_alloc(0,65536));return 0;
}
static void periodic_reap_probe(void) {
    unsigned saved_tick=native_reap_tick;native_reap_tick=0;
    fp_callback_stack_stats before,after;fp_callback_stacks_statistics(&before);assert(!before.pending);
    HANDLE thread=CreateThread(NULL,0,reap_probe_worker,NULL,0,NULL);assert(thread);
    assert(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0);
    DWORD code;assert(GetExitCodeThread(thread,&code) && !code);assert(CloseHandle(thread));
    assert(native_reap_tick==0);
    fp_callback_stacks_statistics(&after);
    assert(after.pending==1 && after.active==before.active+1 && after.freed==before.freed);
    for(unsigned i=0;i<4095;++i)reap_probe_native_call();
    fp_callback_stacks_statistics(&after);
    assert(native_reap_tick==4095 && after.pending==1 && after.freed==before.freed);
    reap_probe_native_call();fp_callback_stacks_statistics(&after);
    assert(native_reap_tick==4096 && !after.pending && after.active==before.active && after.freed==before.freed+1);
    native_reap_tick=UINT_MAX;reap_probe_native_call();assert(native_reap_tick==0);
    native_reap_tick=saved_tick;
}
int wmain(void) {
    setvbuf(stderr,NULL,_IONBF,0);fp_native_validation_init();assert(fp_callback_stacks_init());
    void *handler=AddVectoredExceptionHandler(1,exceptions);assert(handler);
    game.base=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(game.base);
    fp_section section={0,4096,IMAGE_SCN_MEM_EXECUTE};game.size=4096;game.sections=&section;game.section_count=1;
    g_image_delta=(int64_t)(uintptr_t)game.base-INT64_C(0x140000000);self_test_mode=0;
    assert(worker((void *)2)==0);inject_foreign_limit=1;assert(worker((void *)4)==0);inject_foreign_limit=0;
    periodic_reap_probe();
    fp_callback_stack_stats before;fp_callback_stacks_statistics(&before);
    assert(before.active==2 && before.pending==0 && nested_foreign_cases==1);
    assert(!fp_callback_stack_alloc(2,SIZE_MAX));DWORD allocation_error=GetLastError();assert(allocation_error);
    const unsigned total_threads=512,batch_size=16;
    for(unsigned first=0;first<total_threads;first+=batch_size) {
        HANDLE threads[16];
        for(unsigned i=0;i<batch_size;++i){threads[i]=CreateThread(NULL,0,worker,(void *)(uintptr_t)(first+i+2),0,NULL);assert(threads[i]);}
        fp_callback_stacks_reap();
        assert(WaitForMultipleObjects(batch_size,threads,TRUE,30000)==WAIT_OBJECT_0);
        for(unsigned i=0;i<batch_size;++i){DWORD code;assert(GetExitCodeThread(threads[i],&code));assert(code==((first+i+2)&1?0x37:0));assert(CloseHandle(threads[i]));}
        SetLastError(0x9988);fp_callback_stacks_reap();assert(GetLastError()==0x9988);
        fp_callback_stack_stats stats;fp_callback_stacks_statistics(&stats);
        assert(stats.active==before.active && stats.pending==0 && stats.freed-before.freed==first+batch_size);
        assert(worker((void *)2)==0);
    }
    fp_callback_stack_stats after;fp_callback_stacks_statistics(&after);
    assert(after.allocated-before.allocated==total_threads && after.freed-before.freed==total_threads);
    assert(after.destructors_on_stack>=total_threads/2);
    printf("{\"success\":true,\"threads\":%u,\"guest_callback_ExitThread\":%u,\"allocated\":%llu,\"freed\":%llu,\"active_main_thread_blocks\":%llu,\"pending\":%llu,\"FLS_destructors_on_callback_stack\":%llu,\"native_GPR_XMM_nonvolatile\":true,\"main_stack_and_native_TLS_preserved\":true,\"nested_foreign_TEB_limit_cases\":%u,\"nested_16MiB_stack_commit_queries\":%llu,\"forced_allocation_error\":%lu,\"native_reap_per_thread_4096\":true,\"native_reap_unsigned_wrap\":true,\"original_bytes_executed\":false}\n",
        total_threads,total_threads/2,after.allocated,after.freed,after.active,after.pending,after.destructors_on_stack,nested_foreign_cases,nested_commit_queries,allocation_error);
    assert(RemoveVectoredExceptionHandler(handler));VirtualFree(game.base,0,MEM_RELEASE);return 0;
}
