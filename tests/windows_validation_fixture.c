#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../native/windows/native_validation.h"
#include "../native/windows/bridge.h"
#include "../native/windows/callback_stacks.h"

static unsigned legacy(uint64_t target,uint64_t rsp) {
    MEMORY_BASIC_INFORMATION info;
    assert(VirtualQuery((void *)(uintptr_t)target,&info,sizeof(info)) && info.State==MEM_COMMIT &&
        (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)));
    assert(VirtualQuery((void *)(uintptr_t)rsp,&info,sizeof(info)) && info.State==MEM_COMMIT && !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD)));
    assert((uintptr_t)info.BaseAddress+info.RegionSize-rsp>=40);
    unsigned bytes=0;
    while(bytes<256) {
        if(!VirtualQuery((void *)(uintptr_t)(rsp+40+bytes),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))break;
        uint64_t available=(uintptr_t)info.BaseAddress+info.RegionSize-(rsp+40+bytes);
        unsigned add=available<256-bytes?(unsigned)available:256-bytes;
        bytes+=add;if(!add)break;
    }
    return bytes&~7u;
}
static double benchmark(int optimized,uint64_t target,uint64_t *stack,uint64_t low,uint64_t high) {
    LARGE_INTEGER frequency,began,end;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&began);
    CPU c={0};const unsigned count=100000;
    for(unsigned i=0;i<count;++i) {
        c.rsp=(uintptr_t)stack;c.rcx=1;c.rdx=2;c.r8=3;c.r9=4;
        unsigned bytes;
        if(optimized) {
            assert(fp_native_target_valid(target));
            assert(fp_native_stack_prepare(c.rsp,296,low,high));bytes=256;
        } else bytes=legacy(target,c.rsp);
        fp_native_call(&c,target,bytes);
        assert(c.rax==21 && c.rsp==(uintptr_t)stack+8 && c.rip==0xfeed);
    }
    QueryPerformanceCounter(&end);
    return (double)(end.QuadPart-began.QuadPart)*1e9/frequency.QuadPart/count;
}
static void stack_growth_fixture(void) {
    unsigned char *reservation=VirtualAlloc(NULL,65536,MEM_RESERVE,PAGE_NOACCESS);assert(reservation);
    assert(VirtualAlloc(reservation+49152,16384,MEM_COMMIT,PAGE_READWRITE));
    DWORD old;assert(VirtualProtect(reservation+49152,4096,PAGE_READWRITE|PAGE_GUARD,&old));
    uint64_t high=(uintptr_t)reservation+65536,previous=(uintptr_t)reservation+53248;
    uint64_t rsp=high-1024,owner=0;
    SetLastError(0xabcd);assert(fp_native_stack_reservation(rsp,high,&owner) && GetLastError()==0xabcd);
    assert(owner==(uintptr_t)reservation && fp_native_stack_prepare(rsp,296,owner,high));
    void *foreign=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(foreign);
    uint64_t rejected=UINT64_MAX;
    assert(!fp_native_stack_reservation((uintptr_t)foreign,high,&rejected) && rejected==UINT64_MAX);
    assert(!fp_native_stack_reservation((uintptr_t)reservation+8192,high,&rejected));
    assert(!fp_native_stack_reservation((uintptr_t)reservation+49160,high,&rejected));
    assert(!fp_native_stack_prepare((uintptr_t)reservation+49160,296,owner,high));
    assert(VirtualAlloc(reservation+40960,8192,MEM_COMMIT,PAGE_READWRITE));
    assert(VirtualProtect(reservation+49152,4096,PAGE_READWRITE,&old));
    assert(VirtualProtect(reservation+40960,4096,PAGE_READWRITE|PAGE_GUARD,&old));
    fp_native_validation_invalidate();uint64_t grown_rsp=(uintptr_t)reservation+46080;
    assert(!fp_native_stack_prepare(grown_rsp,296,previous,high));
    assert(fp_native_stack_prepare(grown_rsp,296,owner,high));
    SetLastError(0x3456);
    uint64_t actual_limit=fp_native_stack_commit_limit(rsp,high,owner);DWORD actual_error=GetLastError();
    if(actual_limit!=(uintptr_t)reservation+45056 || actual_error!=0x3456)
        fprintf(stderr,"stack growth limit=%p expected=%p LastError=%lu\n",(void *)(uintptr_t)actual_limit,reservation+45056,actual_error);
    assert(actual_limit==(uintptr_t)reservation+45056 && actual_error==0x3456);
    assert(fp_native_stack_commit_limit(grown_rsp,high,owner)==actual_limit);
    assert(fp_native_stack_commit_limit((uintptr_t)foreign,high,owner)==0);
    assert(!fp_native_stack_commit_limit((uintptr_t)reservation+8192,high,owner));
    assert(!fp_native_stack_commit_limit((uintptr_t)reservation+40968,high,owner));
    assert(!fp_native_stack_commit_limit(UINT64_MAX,high,owner));
    assert(!fp_native_stack_commit_limit(rsp,0,owner));
    assert(!fp_native_stack_prepare((uintptr_t)reservation+8192,296,owner,high));
    assert(!fp_native_stack_prepare((uintptr_t)reservation+40968,296,owner,high));
    assert(VirtualProtect(reservation+45056,4096,PAGE_READONLY,&old));fp_native_validation_invalidate();
    assert(!fp_native_stack_reservation(grown_rsp,high,&rejected));
    assert(!fp_native_stack_prepare(grown_rsp,296,owner,high));
    assert(!fp_native_stack_commit_limit(grown_rsp,high,owner));
    assert(VirtualFree(foreign,0,MEM_RELEASE));assert(VirtualFree(reservation,0,MEM_RELEASE));fp_native_validation_invalidate();
    uintptr_t native_rsp=(uintptr_t)_AddressOfReturnAddress();NT_TIB *tib=(NT_TIB *)NtCurrentTeb();
    assert(fp_native_stack_reservation(native_rsp,(uintptr_t)tib->StackBase,&owner));
    assert(owner<=(uintptr_t)tib->StackLimit && native_rsp>=owner);
    fp_native_stack_stats os_before,os_after;fp_native_stack_statistics(&os_before);
    for(unsigned repeat=0;repeat<10;++repeat) {
        assert(fp_native_stack_reservation(native_rsp,(uintptr_t)tib->StackBase,&owner));
        assert(fp_native_stack_commit_limit(native_rsp,(uintptr_t)tib->StackBase,owner));
    }
    fp_native_stack_statistics(&os_after);
    assert(os_after.reservation_queries-os_before.reservation_queries==20 && os_after.commit_queries>os_before.commit_queries);
    assert(os_after.owned_reservation_hits==os_before.owned_reservation_hits && os_after.owned_commit_hits==os_before.owned_commit_hits);
    unsigned char *full=VirtualAlloc(NULL,16u<<20,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(full);
    assert(VirtualProtect(full,4096,PAGE_NOACCESS,&old));
    fp_native_stack_stats before,after;fp_native_stack_statistics(&before);
    assert(fp_native_stack_commit_limit((uintptr_t)full+(16u<<20)-1024,(uintptr_t)full+(16u<<20),(uintptr_t)full)==(uintptr_t)full+4096);
    fp_native_stack_statistics(&after);assert(after.commit_queries-before.commit_queries==2);
    assert(VirtualProtect(full+8192,4096,PAGE_NOACCESS,&old));
    assert(!fp_native_stack_commit_limit((uintptr_t)full+4224,(uintptr_t)full+(16u<<20),(uintptr_t)full));
    assert(VirtualFree(full,0,MEM_RELEASE));
}

static double stack_checks_benchmark(uint64_t rsp,uint64_t high,uint64_t low,int force_fresh) {
    LARGE_INTEGER frequency,start,end;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    for(unsigned i=0;i<100000;++i) {
        if(force_fresh)fp_native_validation_invalidate();uint64_t owner=0;
        assert(fp_native_stack_reservation(rsp,high,&owner) && owner==low);
        assert(fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
        assert(fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
    }
    QueryPerformanceCounter(&end);return (double)(end.QuadPart-start.QuadPart)*1e9/frequency.QuadPart/100000;
}
static DWORD WINAPI retire_other_owner(void *address) {
    fp_native_memory_mutation_begin((uintptr_t)VirtualFree,(uintptr_t)address,0);
    fp_native_memory_mutation_end();return 0;
}
typedef struct {void *main_fiber;uint64_t rsp,high,low;} fiber_check;
static void WINAPI foreign_fiber(void *parameter) {
    fiber_check *check=parameter;uint64_t owner=0,identity=0;unsigned index=99;
    assert(!fp_callback_stack_owned(check->rsp,check->high,&owner,&identity,&index));
    fp_native_stack_stats before,after;fp_native_stack_statistics(&before);
    assert(fp_native_stack_reservation(check->rsp,check->high,&owner) && owner==check->low);
    assert(fp_native_stack_commit_limit(check->rsp,check->high,owner)==check->low+4096);
    fp_native_stack_statistics(&after);
    assert(after.reservation_queries-before.reservation_queries==2 && after.commit_queries-before.commit_queries==2);
    SwitchToFiber(check->main_fiber);abort();
}
static void owned_stack_fixture(void) {
    unsigned char *block=fp_callback_stack_alloc(0,65536);assert(block);DWORD old;
    assert(VirtualProtect(block,4096,PAGE_NOACCESS,&old));fp_native_validation_invalidate();
    uint64_t low=(uintptr_t)block,high=low+65536,rsp=high-1024,owner=0,identity=0;unsigned index=99;
    assert(fp_callback_stack_owned(rsp,high,&owner,&identity,&index) && owner==low && identity && index==0);
    assert(!fp_callback_stack_owned(low+128,high,&owner,&identity,&index));
    assert(!fp_callback_stack_owned(rsp,high-8,&owner,&identity,&index));
    assert(fp_native_stack_reservation(rsp,high,&owner) && owner==low);
    assert(fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
    fp_native_stack_stats before,after;fp_native_stack_statistics(&before);SetLastError(0x6699);
    for(unsigned i=0;i<10000;++i) {
        assert(fp_native_stack_reservation(rsp,high,&owner) && owner==low);
        assert(fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
    }
    assert(GetLastError()==0x6699);fp_native_stack_statistics(&after);
    assert(after.reservation_queries==before.reservation_queries && after.commit_queries==before.commit_queries);
    assert(after.owned_reservation_hits-before.owned_reservation_hits==10000 && after.owned_commit_hits-before.owned_commit_hits==10000);
    fiber_check fiber={ConvertThreadToFiber(NULL),rsp,high,low};assert(fiber.main_fiber);
    void *other_fiber=CreateFiber(0,foreign_fiber,&fiber);assert(other_fiber);SwitchToFiber(other_fiber);
    DeleteFiber(other_fiber);assert(ConvertFiberToThread());
    assert(fp_callback_stack_owned(rsp,high,&owner,&identity,&index));
    assert(!fp_native_stack_reservation(low+128,high,&owner));
    assert(!fp_native_stack_commit_limit(low+128,high,low));
    fp_native_memory_mutation_begin((uintptr_t)VirtualProtect,rsp,4096);
    fp_native_stack_statistics(&before);
    assert(fp_native_stack_reservation(rsp,high,&owner) && fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
    assert(fp_native_stack_reservation(rsp,high,&owner) && fp_native_stack_commit_limit(rsp,high,owner)==low+4096);
    fp_native_stack_statistics(&after);
    assert(after.reservation_queries-before.reservation_queries==4 && after.commit_queries-before.commit_queries==4);
    /* Release aliases retire authority without trusting the native call's eventual result. */
    const char *release_names[]={"VirtualFreeEx","NtFreeVirtualMemory","ZwFreeVirtualMemory"};
    for(unsigned i=0;i<3;++i) {
        unsigned char *alias_block=fp_callback_stack_alloc(i+1,65536);assert(alias_block);
        uint64_t alias_low=(uintptr_t)alias_block,alias_high=alias_low+65536;
        assert(VirtualProtect(alias_block,4096,PAGE_NOACCESS,&old));
        assert(fp_callback_stack_owned(alias_high-1024,alias_high,&owner,&identity,&index));
        HMODULE module=GetModuleHandleW(i?L"ntdll.dll":L"kernel32.dll");
        uint64_t release=(uintptr_t)GetProcAddress(module,release_names[i]);assert(release && fp_native_memory_mutation(release));
        SetLastError(0x44aa);
        fp_native_stack_release_attempt(release,(uintptr_t)GetCurrentProcess(),i?(uintptr_t)&alias_low:alias_low);
        assert(GetLastError()==0x44aa && !fp_callback_stack_owned(alias_high-1024,alias_high,&owner,&identity,&index));
    }
    fp_native_memory_mutation_end();
    assert(VirtualProtect(block+8192,4096,PAGE_NOACCESS,&old));fp_native_validation_invalidate();
    assert(!fp_native_stack_reservation(low+8192,high,&owner));
    assert(!fp_native_stack_commit_limit(low+4224,high,low));
    /* A hole prevents caching the remaining upper tail as a fully committed owned stack. */
    assert(fp_native_stack_commit_limit(rsp,high,low)==low+12288);
    fp_native_stack_statistics(&before);assert(fp_native_stack_commit_limit(rsp,high,low)==low+12288);
    fp_native_stack_statistics(&after);assert(after.commit_queries>before.commit_queries);
    assert(VirtualProtect(block+8192,4096,PAGE_READWRITE,&old));fp_native_validation_invalidate();
    assert(VirtualProtect(block+8192,4096,PAGE_READWRITE|PAGE_GUARD,&old));fp_native_validation_invalidate();
    assert(!fp_native_stack_reservation(low+8192,high,&owner));
    assert(!fp_native_stack_commit_limit(low+8192,high,low));
    assert(VirtualProtect(block+8192,4096,PAGE_READONLY,&old));fp_native_validation_invalidate();
    assert(!fp_native_stack_reservation(low+8192,high,&owner));
    assert(VirtualProtect(block+8192,4096,PAGE_READWRITE,&old));fp_native_validation_invalidate();
    assert(fp_native_stack_commit_limit(rsp,high,low)==low+4096);
    double fresh=stack_checks_benchmark(rsp,high,low,1),cached=stack_checks_benchmark(rsp,high,low,0);
    printf("{\"owned_stack_cache\":true,\"fresh_ns_per_callback_boundary\":%.2f,\"cached_ns_per_callback_boundary\":%.2f,\"speedup\":%.2f,\"cached_20000_checks_VirtualQuery\":0,\"mutation_inflight_uncached\":true,\"OS_stack_uncached\":true,\"retired_owner_reuse_uncached\":true}\n",fresh,cached,fresh/cached);
    /* Cross-thread release attempts must retire the allocation before it can be reused. */
    HANDLE thread=CreateThread(NULL,0,retire_other_owner,block,0,NULL);assert(thread);
    assert(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0);assert(CloseHandle(thread));
    assert(!fp_callback_stack_owned(rsp,high,&owner,&identity,&index));
    assert(VirtualFree(block,0,MEM_RELEASE));
    assert(VirtualAlloc(block,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE)==block);
    assert(VirtualProtect(block,4096,PAGE_NOACCESS,&old));fp_native_validation_invalidate();
    fp_native_stack_statistics(&before);
    for(unsigned i=0;i<2;++i) {
        assert(fp_native_stack_reservation(rsp,high,&owner) && owner==low);
        assert(fp_native_stack_commit_limit(rsp,high,low)==low+4096);
    }
    fp_native_stack_statistics(&after);
    assert(after.reservation_queries-before.reservation_queries==4 && after.commit_queries-before.commit_queries==4);
    /* Leave the same-address replacement live until process teardown; registry authority stays retired. */
}


typedef struct {
    uint64_t target,rsp;HANDLE entered,protect,changed,release;
} held_mutation;
static DWORD WINAPI hold_memory_mutation(void *parameter) {
    held_mutation *state=parameter;DWORD code_old,stack_old,ignored;
    fp_native_memory_mutation_begin((uintptr_t)VirtualProtect,state->target,1);
    assert(SetEvent(state->entered));assert(WaitForSingleObject(state->protect,30000)==WAIT_OBJECT_0);
    assert(VirtualProtect((void *)(uintptr_t)state->target,1,PAGE_NOACCESS,&code_old));
    assert(VirtualProtect((void *)(uintptr_t)state->rsp,1,PAGE_READONLY,&stack_old));
    assert(SetEvent(state->changed));assert(WaitForSingleObject(state->release,30000)==WAIT_OBJECT_0);
    assert(VirtualProtect((void *)(uintptr_t)state->target,1,code_old,&ignored));
    assert(VirtualProtect((void *)(uintptr_t)state->rsp,1,stack_old,&ignored));
    fp_native_memory_mutation_end();return 0;
}
static void held_mutation_fixture(uint64_t target,uint64_t rsp,uint64_t low,uint64_t high) {
    assert(fp_native_target_valid(target));assert(fp_native_stack_prepare(rsp,296,low,high));
    assert(fp_native_stack_cached(rsp,296,low,high));
    held_mutation state={target,rsp};
    state.entered=CreateEventW(NULL,TRUE,FALSE,NULL);state.protect=CreateEventW(NULL,TRUE,FALSE,NULL);
    state.changed=CreateEventW(NULL,TRUE,FALSE,NULL);state.release=CreateEventW(NULL,TRUE,FALSE,NULL);
    assert(state.entered && state.protect && state.changed && state.release);
    HANDLE thread=CreateThread(NULL,0,hold_memory_mutation,&state,0,NULL);assert(thread);
    assert(WaitForSingleObject(state.entered,30000)==WAIT_OBJECT_0);
    fp_native_stack_stats before,after;fp_native_stack_statistics(&before);SetLastError(0xabcd7654);
    for(unsigned i=0;i<10000;++i) {
        assert(fp_native_target_valid(target));assert(!fp_native_stack_cached(rsp,296,low,high));
        assert(fp_native_stack_prepare(rsp,296,low,high));assert(!fp_native_stack_cached(rsp,296,low,high));
    }
    assert(GetLastError()==0xabcd7654);fp_native_stack_statistics(&after);
    assert(after.hits==before.hits && after.accepted-before.accepted==10000);
    assert(SetEvent(state.protect));assert(WaitForSingleObject(state.changed,30000)==WAIT_OBJECT_0);
    SetLastError(0xbcde8765);
    assert(!fp_native_target_valid(target));assert(!fp_native_stack_prepare(rsp,296,low,high));
    assert(GetLastError()==0xbcde8765);
    assert(SetEvent(state.release));assert(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0);
    assert(fp_native_target_valid(target));assert(fp_native_stack_prepare(rsp,296,low,high));
    assert(fp_native_stack_cached(rsp,296,low,high));
    assert(CloseHandle(thread));assert(CloseHandle(state.entered));assert(CloseHandle(state.protect));
    assert(CloseHandle(state.changed));assert(CloseHandle(state.release));
    puts("{\"held_mutation_concurrency\":true,\"reader_iterations\":10000,\"inflight_stack_cache_bypassed\":true,\"held_protection_change_rejected\":true,\"post_mutation_cache_restored\":true,\"last_error_preserved\":true}");
}

static const char hook_names[][24]={"first alias","second alias","replacement","live value"};
static _Thread_local unsigned hook_case_count;
static void hook_check(const fp_native_hook *hooks,unsigned count,uint64_t target) {
    const char *expected=NULL;for(unsigned i=0;i<count;++i)if(hooks[i].target==target){expected=hooks[i].name;break;}
    SetLastError(0xe5ef1234);unsigned csr=_mm_getcsr();
    assert(fp_native_hook_name(hooks,count,target)==expected && GetLastError()==0xe5ef1234 && _mm_getcsr()==csr);++hook_case_count;
}
static uint64_t hook_random(uint64_t *s){*s^=*s<<13;*s^=*s>>7;return *s^=*s<<17;}
static void hook_fill(fp_native_hook *h,unsigned count,uint64_t base) {
    for(unsigned i=0;i<count;++i)h[i]=(fp_native_hook){base+(i/3)*32,hook_names[i%4]};
}
static DWORD WINAPI hook_thread(void *arg) {
    fp_native_hook h[557];uint64_t state=(uintptr_t)arg+1;unsigned csr=_mm_getcsr();_mm_setcsr(0x5fc3);
    for(unsigned turn=0;turn<4096;++turn){unsigned count=(unsigned)(hook_random(&state)%558);hook_fill(h,count,((uintptr_t)arg+1)*0x100000);uint64_t target=count?h[hook_random(&state)%count].target+(turn&1):0;hook_check(h,count,target);}
    assert(hook_case_count==4096);_mm_setcsr(csr);return 0;
}
static void hook_name_cases(void) {
    unsigned csr=_mm_getcsr();_mm_setcsr(0x5fc3);fp_native_hook h[557];hook_fill(h,557,UINT64_C(0x7ffb00000000));
    for(unsigned i=0;i<557;++i){hook_check(h,557,h[i].target);hook_check(h,557,h[i].target+1);}
    hook_check(h,557,0);hook_check(h,557,UINT64_MAX);
    /* The actual host self-test replaces names without changing target/count. */
    for(unsigned i=0;i<4;++i){h[0]=(fp_native_hook){0x12345678,hook_names[i]};hook_check(h,1,h[0].target);}
    h[0].name=NULL;hook_check(h,1,h[0].target);
    for(unsigned i=0;i<8;++i)h[i]=(fp_native_hook){i*32,hook_names[i%4]};
    hook_check(h,8,128);h[3].target=128;hook_check(h,8,128);h[3].name=hook_names[3];hook_check(h,8,128);
    hook_check(h,8,129);h[5].target=129;hook_check(h,8,129);
    hook_check(h,8,224);hook_check(NULL,0,224);hook_check(h,1,224);hook_check(h,8,224);
    h[0]=(fp_native_hook){0,hook_names[0]};h[1]=(fp_native_hook){UINT64_MAX,hook_names[1]};
    hook_check(h,2,0);hook_check(h,2,UINT64_MAX);hook_check(h,2,1);
    for(unsigned i=0;i<557;++i)h[i]=(fp_native_hook){UINT64_C(0x7ffb00000000)+i*32,hook_names[i%4]};
    for(unsigned i=0;i<128;++i)hook_check(h,557,h[(i&1)*32].target); /* Same hash slot, different lower_bound. */
    uint64_t state=UINT64_C(0x4576cba987123def);
    for(unsigned turn=0;turn<2048;++turn){unsigned count=(unsigned)(hook_random(&state)%558);hook_fill(h,count,hook_random(&state)&UINT64_C(0xffff00000000));for(unsigned q=0;q<16;++q){uint64_t target=count?h[hook_random(&state)%count].target:hook_random(&state);hook_check(h,count,target+(q&1));}}
    unsigned char *area=VirtualAlloc(NULL,3*4096,MEM_RESERVE,PAGE_NOACCESS);assert(area);
    assert(VirtualAlloc(area+4096,4096,MEM_COMMIT,PAGE_READWRITE));
    for(unsigned endpoint=0;endpoint<2;++endpoint)for(unsigned count=1;count<=256;++count){
        fp_native_hook *guarded=(fp_native_hook *)(area+4096+(endpoint?4096-count*sizeof(*guarded):0));hook_fill(guarded,count,0x100000);
        hook_check(guarded,count,guarded[0].target);hook_check(guarded,count,guarded[count-1].target);
        hook_check(guarded,count,guarded[0].target-1);hook_check(guarded,count,UINT64_MAX);
        hook_check((fp_native_hook *)area,0,guarded[count-1].target);hook_check(NULL,0,guarded[0].target);
    }
    fp_native_hook *old=(fp_native_hook *)(area+4096);hook_fill(old,100,0x200000);hook_check(old,100,old[99].target);
    fp_native_hook fresh[100];hook_fill(fresh,100,0x200000);fresh[99].name=hook_names[3];DWORD protect;
    assert(VirtualProtect(area+4096,4096,PAGE_NOACCESS,&protect));hook_check(fresh,100,fresh[99].target);
    assert(VirtualFree(area,0,MEM_RELEASE));
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,hook_thread,(void *)(uintptr_t)i,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i){DWORD code;assert(GetExitCodeThread(threads[i],&code)&&!code);assert(CloseHandle(threads[i]));}
    assert(hook_case_count==37103);_mm_setcsr(csr);
}

unsigned fp_test_mutation_cases(void);
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);fp_native_validation_init();
    fp_native_hook hooks[]={{9,"last"},{3,"first alias"},{3,"second alias"},{1,"first"}};
    fp_native_hooks_sort(hooks,4);
    assert(!strcmp(fp_native_hook_name(hooks,4,3),"first alias"));
    assert(!strcmp(fp_native_hook_name(hooks,4,9),"last"));
    assert(!fp_native_hook_name(hooks,4,0) && !fp_native_hook_name(hooks,4,10));
    hook_name_cases();
    unsigned mutation_cases=fp_test_mutation_cases();assert(mutation_cases==21005);
    HMODULE module=LoadLibraryW(argv[1]);assert(module);
    uint64_t target=(uintptr_t)GetProcAddress(module,"validation_sum");assert(target);
    SetLastError(0x1234);assert(fp_native_target_valid(target) && GetLastError()==0x1234);
    assert(!fp_native_target_valid((uintptr_t)&hooks));
    DWORD old;assert(VirtualProtect((void *)(uintptr_t)target,1,PAGE_NOACCESS,&old));
    fp_native_validation_invalidate();assert(!fp_native_target_valid(target));
    assert(VirtualProtect((void *)(uintptr_t)target,1,old,&old));fp_native_validation_invalidate();
    assert(fp_native_target_valid(target));
    assert(fp_native_memory_mutation((uintptr_t)VirtualProtectEx));fp_native_validation_invalidate();
    assert(VirtualProtectEx(GetCurrentProcess(),(void *)(uintptr_t)target,1,PAGE_NOACCESS,&old));
    fp_native_validation_invalidate();assert(!fp_native_target_valid(target));
    assert(VirtualProtectEx(GetCurrentProcess(),(void *)(uintptr_t)target,1,old,&old));
    fp_native_validation_invalidate();assert(fp_native_target_valid(target));
    typedef LONG (NTAPI *protect_fn)(HANDLE,PVOID *,PSIZE_T,ULONG,PULONG);
    protect_fn nt=(protect_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtProtectVirtualMemory");
    assert(nt && fp_native_memory_mutation((uintptr_t)nt));
    void *protected_base=(void *)(uintptr_t)target;SIZE_T protected_size=1;ULONG previous;
    fp_native_validation_invalidate();
    assert(nt(GetCurrentProcess(),&protected_base,&protected_size,PAGE_NOACCESS,&previous)>=0);
    fp_native_validation_invalidate();assert(!fp_native_target_valid(target));
    fp_native_validation_invalidate();
    assert(nt(GetCurrentProcess(),&protected_base,&protected_size,previous,&previous)>=0);
    fp_native_validation_invalidate();assert(fp_native_target_valid(target));
    assert(FreeLibrary(module));assert(GetModuleHandleW(argv[1]));
    void *private_code=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE);assert(private_code);
    assert(fp_native_target_valid((uintptr_t)private_code));
    assert(VirtualProtect(private_code,4096,PAGE_NOACCESS,&old));
    assert(!fp_native_target_valid((uintptr_t)private_code));assert(VirtualFree(private_code,0,MEM_RELEASE));
    void *region=VirtualAlloc(NULL,32768,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(region);
    assert(VirtualProtect(region,4096,PAGE_NOACCESS,&old));
    uint64_t low=(uintptr_t)region+4096,high=(uintptr_t)region+32768;
    uint64_t *stack=(uint64_t *)((char *)region+16384);stack[0]=0xfeed;stack[5]=5;stack[6]=6;
    assert(fp_native_stack_prepare((uintptr_t)stack,296,low,high));
    assert(fp_native_stack_cached((uintptr_t)stack,8,low,high));
    assert(!fp_native_stack_cached(low-8,16,low,high));
    assert(!fp_native_stack_prepare(high-32,40,low,high));
    assert(!fp_native_stack_prepare(UINT64_MAX-4,40,low,high));
    assert(VirtualProtect(stack,4096,PAGE_READONLY,&old));fp_native_validation_invalidate();
    assert(!fp_native_stack_cached((uintptr_t)stack,8,low,high));
    assert(!fp_native_stack_prepare((uintptr_t)stack,296,low,high));
    assert(VirtualProtect(stack,4096,PAGE_READWRITE,&old));fp_native_validation_invalidate();
    assert(fp_native_memory_mutation((uintptr_t)VirtualProtect));
    assert(fp_native_memory_mutation((uintptr_t)VirtualProtectEx));
    assert(fp_native_memory_mutation((uintptr_t)VirtualAllocEx));
    FARPROC nt_protect=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtProtectVirtualMemory");
    assert(nt_protect && fp_native_memory_mutation((uintptr_t)nt_protect));
    FARPROC zw_protect=GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"ZwProtectVirtualMemory");
    assert(zw_protect && fp_native_memory_mutation((uintptr_t)zw_protect));
    fp_native_stack_stats stats;fp_native_stack_statistics(&stats);
    assert(stats.calls && stats.hits==0 && stats.accepted && stats.bounds>=2 && stats.protection);
    SetLastError(0x9876);assert(!fp_native_stack_prepare((uintptr_t)stack,296,0,0) && GetLastError()==0x9876);
    fp_native_stack_statistics(&stats);assert(stats.no_owner==1);
    assert(!fp_native_memory_mutation(target));
    stack_growth_fixture();
    owned_stack_fixture();
    held_mutation_fixture(target,(uintptr_t)stack,low,high);
    double before=0,after=0;
    for(unsigned i=0;i<3;++i){before+=benchmark(0,target,stack,low,high);after+=benchmark(1,target,stack,low,high);}
    before/=3;after/=3;
    printf("{\"pass\":true,\"baseline_ns_per_native_call\":%.2f,\"optimized_ns_per_native_call\":%.2f,\"speedup\":%.2f,\"stable_alias_lookup\":true,\"hook_lookup_cases\":%u,\"hook_thread_cases\":16384,\"hook_guard_endpoints\":true,\"hook_live_names_tables_counts\":true,\"mutation_lookup_cases\":%u,\"mutation_thread_cases\":16384,\"mutation_original_table_restored\":true,\"pinned_module\":true,\"private_exec_uncached\":true,\"guard_and_bounds_preserved\":true,\"permission_invalidation\":true,\"mutation_Ex_Nt_recognized\":true,\"stack_reservation_growth\":true,\"foreign_heap_guard_uncommitted_rejected\":true,\"committed_TIB_limit_preserved\":true,\"full_16MiB_stack_commit_queries\":2,\"iterations_per_variant\":300000,\"gameplay_proven\":false}\n",before,after,before/after,hook_case_count,mutation_cases);
    assert(VirtualFree(region,0,MEM_RELEASE));return 0;
}
