/* An exiting thread can still be using its callback stack during FLS cleanup. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include "callback_stacks.h"
#include "native_validation.h"

typedef struct stack_owner {
    struct stack_owner *next,*all_next;HANDLE thread;
    void *blocks[FP_CALLBACK_STACK_LIMIT];size_t bytes[FP_CALLBACK_STACK_LIMIT];
    uint64_t identities[FP_CALLBACK_STACK_LIMIT];volatile LONG retired[FP_CALLBACK_STACK_LIMIT];
} stack_owner;
static INIT_ONCE initialized=INIT_ONCE_STATIC_INIT;
static DWORD cleanup_slot=FLS_OUT_OF_INDEXES;
static SRWLOCK lock=SRWLOCK_INIT;
static stack_owner *pending,*owners;
static _Thread_local stack_owner *current;
static volatile LONG64 allocated,freed,pending_count,destructors_on_stack;
static volatile LONG64 next_identity;

int fp_callback_stack_owned(uint64_t rsp,uint64_t stack_base,uint64_t *low,uint64_t *identity,unsigned *index) {
    if(!current || !low || !identity || !index || rsp>=stack_base || stack_base-rsp<8)return 0;
    DWORD saved=GetLastError();int live=FlsGetValue(cleanup_slot)==current;SetLastError(saved);
    if(!live)return 0;
    for(unsigned i=0;i<FP_CALLBACK_STACK_LIMIT;++i) {
        uint64_t start=(uintptr_t)current->blocks[i];size_t bytes=current->bytes[i];
        if(start && !InterlockedCompareExchange(&current->retired[i],0,0) && bytes<=UINT64_MAX-start &&
           start+bytes==stack_base && bytes>4096 && rsp>=start+4096) {
            *low=start;*identity=current->identities[i];*index=i;return 1;
        }
    }
    return 0;
}
void fp_callback_stack_retire(uint64_t address,int all) {
    AcquireSRWLockExclusive(&lock);
    for(stack_owner *owner=owners;owner;owner=owner->all_next)
        for(unsigned i=0;i<FP_CALLBACK_STACK_LIMIT;++i) {
            uint64_t start=(uintptr_t)owner->blocks[i];
            if(start && (all || (address>=start && address-start<owner->bytes[i])))InterlockedExchange(&owner->retired[i],1);
        }
    ReleaseSRWLockExclusive(&lock);
}

static VOID CALLBACK thread_cleanup(PVOID value) {
    stack_owner *owner=value;if(!owner)return;
    uintptr_t here=(uintptr_t)&owner;
    for(unsigned i=0;i<FP_CALLBACK_STACK_LIMIT;++i) {
        uintptr_t start=(uintptr_t)owner->blocks[i];
        if(start && here>=start && here-start<owner->bytes[i]){InterlockedIncrement64(&destructors_on_stack);break;}
    }
    AcquireSRWLockExclusive(&lock);
    for(unsigned i=0;i<FP_CALLBACK_STACK_LIMIT;++i)InterlockedExchange(&owner->retired[i],1);
    owner->next=pending;pending=owner;
    InterlockedIncrement64(&pending_count);ReleaseSRWLockExclusive(&lock);
    current=NULL;
}
static BOOL CALLBACK initialize(PINIT_ONCE once,PVOID parameter,PVOID *context) {
    (void)once;(void)parameter;(void)context;
    cleanup_slot=FlsAlloc(thread_cleanup);return cleanup_slot!=FLS_OUT_OF_INDEXES;
}
int fp_callback_stacks_init(void) {
    return InitOnceExecuteOnce(&initialized,initialize,NULL,NULL)!=0;
}
void fp_callback_stacks_statistics(fp_callback_stack_stats *stats) {
    stats->freed=(uint64_t)InterlockedCompareExchange64(&freed,0,0);
    stats->allocated=(uint64_t)InterlockedCompareExchange64(&allocated,0,0);
    stats->active=stats->allocated-stats->freed;
    stats->pending=(uint64_t)InterlockedCompareExchange64(&pending_count,0,0);
    stats->destructors_on_stack=(uint64_t)InterlockedCompareExchange64(&destructors_on_stack,0,0);
}
void fp_callback_stacks_reap(void) {
    DWORD saved=GetLastError();
    if(!InterlockedCompareExchange64(&pending_count,0,0)){SetLastError(saved);return;}
    AcquireSRWLockExclusive(&lock);
    stack_owner **link=&pending;
    while(*link) {
        stack_owner *owner=*link;
        if(WaitForSingleObject(owner->thread,0)!=WAIT_OBJECT_0){link=&owner->next;continue;}
        /* The real thread is signaled: no return address can still use these blocks. */
        int released=1;
        for(unsigned i=0;i<FP_CALLBACK_STACK_LIMIT;++i)if(owner->blocks[i]) {
            if(VirtualFree(owner->blocks[i],0,MEM_RELEASE)){
                owner->blocks[i]=NULL;InterlockedIncrement64(&freed);fp_native_validation_invalidate();
            }
            else {fprintf(stderr,"CALLBACK_STACK_REAP_FAILED error=%lu\n",GetLastError());released=0;}
        }
        if(!released){link=&owner->next;continue;}
        *link=owner->next;
        stack_owner **all=&owners;while(*all && *all!=owner)all=&(*all)->all_next;
        if(*all)*all=owner->all_next;
        CloseHandle(owner->thread);HeapFree(GetProcessHeap(),0,owner);
        InterlockedDecrement64(&pending_count);
    }
    ReleaseSRWLockExclusive(&lock);SetLastError(saved);
}
static void allocation_failure(DWORD error,size_t bytes) {
    MEMORYSTATUSEX memory={0};memory.dwLength=sizeof(memory);int memory_ok=GlobalMemoryStatusEx(&memory)!=0;
    PROCESS_MEMORY_COUNTERS_EX process={0};process.cb=sizeof(process);
    int process_ok=K32GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS *)&process,sizeof(process))!=0;
    fp_callback_stack_stats stats;fp_callback_stacks_statistics(&stats);
    fprintf(stderr,"CALLBACK_STACK_ALLOCATION_FAILED error=%lu requested=%zu active_blocks=%llu allocated=%llu freed=%llu pending_threads=%llu process_private=%zu process_memory_valid=%d commit_limit=%llu commit_available=%llu physical_available=%llu global_memory_valid=%d\n",
        error,bytes,stats.active,stats.allocated,stats.freed,stats.pending,(size_t)process.PrivateUsage,process_ok,
        memory.ullTotalPageFile,memory.ullAvailPageFile,memory.ullAvailPhys,memory_ok);
    SetLastError(error);
}
void *fp_callback_stack_alloc(unsigned index,size_t bytes) {
    if(index>=FP_CALLBACK_STACK_LIMIT || !bytes){SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
    if(!fp_callback_stacks_init())return NULL;
    fp_callback_stacks_reap();
    if(!current) {
        stack_owner *owner=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*owner));
        if(!owner){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}
        if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&owner->thread,SYNCHRONIZE,FALSE,0) ||
           !FlsSetValue(cleanup_slot,owner)) {
            DWORD error=GetLastError();if(owner->thread)CloseHandle(owner->thread);HeapFree(GetProcessHeap(),0,owner);SetLastError(error);return NULL;
        }
        AcquireSRWLockExclusive(&lock);owner->all_next=owners;owners=owner;ReleaseSRWLockExclusive(&lock);
        current=owner;
    }
    if(current->blocks[index]) {
        if(current->bytes[index]!=bytes){SetLastError(ERROR_INVALID_PARAMETER);return NULL;}
        return current->blocks[index];
    }
    void *block=VirtualAlloc(NULL,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!block){DWORD error=GetLastError();allocation_failure(error,bytes);return NULL;}
    fp_native_validation_invalidate();
    AcquireSRWLockExclusive(&lock);current->blocks[index]=block;current->bytes[index]=bytes;
    current->identities[index]=(uint64_t)InterlockedIncrement64(&next_identity);
    ReleaseSRWLockExclusive(&lock);
    InterlockedIncrement64(&allocated);return block;
}
