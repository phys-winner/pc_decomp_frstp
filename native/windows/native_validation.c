/* Only pinned read-only image code and current owned committed stacks are cached. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include "native_validation.h"
#include "callback_stacks.h"

static volatile LONG64 epoch=1;
static volatile LONG mutations_inflight;
static uint64_t mutation_targets[128];
static unsigned mutation_count;
static struct {uint64_t target;unsigned argument;} release_targets[12];
static unsigned release_count;
static _Thread_local struct {uint64_t target,epoch;} code_cache[256];
static _Thread_local struct {uint64_t low,high,owner_low,owner_high,epoch;} stack_cache;
static _Thread_local struct {uint64_t low,high,identity,limit,epoch;} owned_stack_cache[FP_CALLBACK_STACK_LIMIT];
static _Thread_local fp_native_stack_stats stack_stats;
static _Thread_local uint64_t stack_failures;
static volatile LONG failure_examples;

static uint64_t generation(void) {return (uint64_t)ReadAcquire64(&epoch);}
static int cache_allowed(void) {return !ReadAcquire(&mutations_inflight);}
void fp_native_validation_invalidate(void) {InterlockedIncrement64(&epoch);}
void fp_native_memory_mutation_begin(uint64_t target,uint64_t rcx,uint64_t rdx) {
    InterlockedIncrement(&mutations_inflight);fp_native_validation_invalidate();
    fp_native_stack_release_attempt(target,rcx,rdx);
}
void fp_native_memory_mutation_end(void) {
    fp_native_validation_invalidate();InterlockedDecrement(&mutations_inflight);
}

void fp_native_hooks_sort(fp_native_hook *hooks,unsigned count) {
    for(unsigned i=1;i<count;++i) {
        fp_native_hook value=hooks[i];unsigned j=i;
        while(j && hooks[j-1].target>value.target){hooks[j]=hooks[j-1];--j;}
        hooks[j]=value;
    }
}
static _Thread_local unsigned hook_name_hints[64];
const char *fp_native_hook_name(const fp_native_hook *hooks,unsigned count,uint64_t target) {
    unsigned slot=(unsigned)((target>>4)^(target>>16))&63;
    unsigned low=hook_name_hints[slot];
    if(!(low<=count && (!low || hooks[low-1].target<target) &&
         (low==count || hooks[low].target>=target))) {
        low=0;unsigned high=count;
        while(low<high){unsigned mid=low+(high-low)/2;if(hooks[mid].target<target)low=mid+1;else high=mid;}
        hook_name_hints[slot]=low;
    }
    return low<count && hooks[low].target==target?hooks[low].name:NULL;
}

void fp_native_validation_init(void) {
    static const wchar_t *modules[]={L"kernel32.dll",L"kernelbase.dll",L"ntdll.dll"};
    static const char *names[]={
        "VirtualAlloc","VirtualAllocEx","VirtualAllocExNuma","VirtualAllocFromApp","VirtualAlloc2","VirtualAlloc2FromApp",
        "VirtualProtect","VirtualProtectEx","VirtualProtectFromApp","VirtualFree","VirtualFreeEx",
        "MapViewOfFile","MapViewOfFileEx","MapViewOfFileExNuma","MapViewOfFileFromApp","MapViewOfFile3",
        "MapViewOfFile3FromApp","MapViewOfFileNuma2","UnmapViewOfFile","UnmapViewOfFileEx",
        "NtAllocateVirtualMemory","NtAllocateVirtualMemoryEx","NtProtectVirtualMemory","NtFreeVirtualMemory",
        "NtMapViewOfSection","NtMapViewOfSectionEx","NtUnmapViewOfSection","NtUnmapViewOfSectionEx",
        "ZwAllocateVirtualMemory","ZwAllocateVirtualMemoryEx","ZwProtectVirtualMemory","ZwFreeVirtualMemory",
        "ZwMapViewOfSection","ZwMapViewOfSectionEx","ZwUnmapViewOfSection","ZwUnmapViewOfSectionEx"};
    DWORD saved=GetLastError();mutation_count=0;release_count=0;
    for(unsigned m=0;m<sizeof(modules)/sizeof(*modules);++m) {
        HMODULE module=GetModuleHandleW(modules[m]);
        if(!module)continue;
        for(unsigned n=0;n<sizeof(names)/sizeof(*names);++n) {
            uint64_t target=(uintptr_t)GetProcAddress(module,names[n]);
            if(!target)continue;
            unsigned i;for(i=0;i<mutation_count;++i)if(mutation_targets[i]==target)break;
            if(i==mutation_count && mutation_count<128)mutation_targets[mutation_count++]=target;
        }
    }
    static const struct {const char *name;unsigned argument;} releases[]={
        {"VirtualFree",1},{"VirtualFreeEx",2},{"NtFreeVirtualMemory",3},{"ZwFreeVirtualMemory",3}};
    for(unsigned m=0;m<sizeof(modules)/sizeof(*modules);++m) {
        HMODULE module=GetModuleHandleW(modules[m]);if(!module)continue;
        for(unsigned n=0;n<sizeof(releases)/sizeof(*releases);++n) {
            uint64_t target=(uintptr_t)GetProcAddress(module,releases[n].name);if(!target)continue;
            unsigned i;for(i=0;i<release_count;++i)if(release_targets[i].target==target)break;
            if(i==release_count && release_count<sizeof(release_targets)/sizeof(*release_targets)) {
                release_targets[release_count].target=target;
                release_targets[release_count++].argument=releases[n].argument;
            }
        }
    }
    for(unsigned i=1;i<mutation_count;++i) {
        uint64_t value=mutation_targets[i];unsigned j=i;
        while(j && mutation_targets[j-1]>value){mutation_targets[j]=mutation_targets[j-1];--j;}
        mutation_targets[j]=value;
    }
    fp_native_validation_invalidate();SetLastError(saved);
}

void fp_native_stack_release_attempt(uint64_t target,uint64_t rcx,uint64_t rdx) {
    for(unsigned i=0;i<release_count;++i)if(release_targets[i].target==target) {
        DWORD saved=GetLastError();uint64_t address=release_targets[i].argument==1?rcx:rdx;int all=0;
        if(release_targets[i].argument==3) {
            SIZE_T got=0;
            all=!ReadProcessMemory(GetCurrentProcess(),(void *)(uintptr_t)rdx,&address,sizeof(address),&got) || got!=sizeof(address);
        }
        /* Even failed release attempts retire authority: address reuse must never inherit it. */
        fp_callback_stack_retire(address,all);SetLastError(saved);return;
    }
}

static _Thread_local unsigned mutation_hints[64];
int fp_native_memory_mutation(uint64_t target) {
    unsigned slot=(unsigned)((target>>4)^(target>>16))&63;
    unsigned low=mutation_hints[slot];
    if(!(low<=mutation_count && (!low || mutation_targets[low-1]<target) &&
         (low==mutation_count || mutation_targets[low]>=target))) {
        low=0;unsigned high=mutation_count;
        while(low<high){unsigned mid=low+(high-low)/2;if(mutation_targets[mid]<target)low=mid+1;else high=mid;}
        mutation_hints[slot]=low;
    }
    return low<mutation_count && mutation_targets[low]==target;
}

int fp_native_target_valid(uint64_t target) {
    uint64_t current=generation();
    unsigned slot=(unsigned)((target>>4)^(target>>16))&255;
    if(cache_allowed() && code_cache[slot].target==target && code_cache[slot].epoch==current && generation()==current)return 1;
    DWORD saved=GetLastError();MEMORY_BASIC_INFORMATION info;
    int valid=VirtualQuery((void *)(uintptr_t)target,&info,sizeof(info)) && info.State==MEM_COMMIT &&
        !(info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) &&
        (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
    if(valid && cache_allowed() && info.Type==MEM_IMAGE && (info.Protect==PAGE_EXECUTE_READ || info.Protect==PAGE_EXECUTE)) {
        HMODULE pinned;
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)(uintptr_t)target,&pinned)) {
            code_cache[slot].target=target;code_cache[slot].epoch=current;
        }
    }
    SetLastError(saved);return valid;
}

int fp_native_stack_cached(uint64_t address,size_t bytes,uint64_t owner_low,uint64_t owner_high) {
    return cache_allowed() && stack_cache.epoch==generation() && stack_cache.owner_low==owner_low &&
        stack_cache.owner_high==owner_high && address>=stack_cache.low && address<=stack_cache.high &&
        bytes<=stack_cache.high-address;
}
int fp_native_stack_reservation(uint64_t rsp,uint64_t stack_base,uint64_t *reservation_low) {
    if(!reservation_low || !stack_base || rsp>=stack_base || stack_base-rsp<8)return 0;
    uint64_t low,identity;unsigned index;
    if(cache_allowed() && fp_callback_stack_owned(rsp,stack_base,&low,&identity,&index) &&
       owned_stack_cache[index].identity==identity && owned_stack_cache[index].epoch==generation() &&
       owned_stack_cache[index].low==low && owned_stack_cache[index].high==stack_base &&
       rsp>=owned_stack_cache[index].limit) {
        *reservation_low=low;++stack_stats.owned_reservation_hits;return 1;
    }
    DWORD saved=GetLastError();MEMORY_BASIC_INFORMATION at={0},top={0};
    ++stack_stats.reservation_queries;int valid=VirtualQuery((void *)(uintptr_t)rsp,&at,sizeof(at))!=0;
    if(valid){++stack_stats.reservation_queries;valid=VirtualQuery((void *)(uintptr_t)(stack_base-1),&top,sizeof(top))!=0;}
    valid=valid &&
        at.State==MEM_COMMIT && top.State==MEM_COMMIT && at.Type==MEM_PRIVATE && top.Type==MEM_PRIVATE &&
        at.Protect==PAGE_READWRITE && top.Protect==PAGE_READWRITE && at.AllocationBase &&
        at.AllocationBase==top.AllocationBase && (uintptr_t)at.AllocationBase<=rsp &&
        at.RegionSize>=8 && rsp-(uintptr_t)at.BaseAddress<=at.RegionSize-8;
    if(valid)*reservation_low=(uintptr_t)at.AllocationBase;
    SetLastError(saved);return valid;
}
uint64_t fp_native_stack_commit_limit(uint64_t rsp,uint64_t stack_base,uint64_t reservation_low) {
    if(!reservation_low || rsp<reservation_low || rsp>=stack_base || stack_base-rsp<8)return 0;
    uint64_t owned_low=0,identity=0,current=generation();unsigned index=0;
    int owned=fp_callback_stack_owned(rsp,stack_base,&owned_low,&identity,&index) && owned_low==reservation_low;
    if(cache_allowed() && owned && owned_stack_cache[index].identity==identity && owned_stack_cache[index].epoch==current &&
       owned_stack_cache[index].low==reservation_low && owned_stack_cache[index].high==stack_base &&
       rsp>=owned_stack_cache[index].limit && generation()==current) {
        ++stack_stats.owned_commit_hits;return owned_stack_cache[index].limit;
    }
    DWORD saved=GetLastError();uint64_t cursor=reservation_low,tail=0,result=0;
    int fixed_guard=0,exact_end=0;
    /* Scan forward by RegionSize: querying low-1 repeatedly rounds to each page. */
    while(cursor<stack_base) {
        MEMORY_BASIC_INFORMATION info;++stack_stats.commit_queries;
        if(!VirtualQuery((void *)(uintptr_t)cursor,&info,sizeof(info)) || info.Type!=MEM_PRIVATE ||
           (uintptr_t)info.AllocationBase!=reservation_low ||
           (info.State!=MEM_RESERVE && info.State!=MEM_COMMIT))break;
        uint64_t region=(uintptr_t)info.BaseAddress;
        if(region>cursor || info.RegionSize>UINT64_MAX-region)break;
        uint64_t next=region+info.RegionSize;if(next<=cursor)break;
        if(cursor==reservation_low)fixed_guard=info.State==MEM_COMMIT && info.Protect==PAGE_NOACCESS &&
            region==reservation_low && info.RegionSize==4096;
        if(info.State==MEM_COMMIT && info.Protect==PAGE_READWRITE){if(!tail)tail=cursor;}
        else tail=0;
        cursor=next<stack_base?next:stack_base;
        if(cursor==stack_base && tail && tail<=rsp){result=tail;exact_end=next==stack_base;}
    }
    if(cache_allowed() && owned && fixed_guard && exact_end && reservation_low<=UINT64_MAX-4096 && result==reservation_low+4096) {
        owned_stack_cache[index].low=reservation_low;owned_stack_cache[index].high=stack_base;
        owned_stack_cache[index].identity=identity;owned_stack_cache[index].limit=result;owned_stack_cache[index].epoch=current;
    }
    SetLastError(saved);return result;
}
void fp_native_stack_statistics(fp_native_stack_stats *stats) {*stats=stack_stats;}
static void stack_failure(const char *reason,uint64_t address,size_t bytes,uint64_t owner_low,uint64_t owner_high) {
    uint64_t failures=++stack_failures;
    if(failures!=65536 && failures!=1048576) {
        if(failures!=1 || InterlockedIncrement(&failure_examples)>8)return;
    }
    DWORD saved=GetLastError();MEMORY_BASIC_INFORMATION info={0};
    int queried=VirtualQuery((void *)(uintptr_t)address,&info,sizeof(info))!=0;
    fprintf(stderr,"NATIVE_STACK_FALLBACK tid=%lu reason=%s rsp=0x%llx bytes=%zu owner_low=0x%llx owner_high=0x%llx region=0x%llx region_bytes=%zu state=%08lx protect=%08lx type=%08lx queried=%d failures=%llu calls=%llu cache_hits=%llu accepted_queries=%llu no_owner=%llu bounds=%llu query=%llu protection=%llu span=%llu\n",
        GetCurrentThreadId(),reason,address,bytes,owner_low,owner_high,(uintptr_t)info.BaseAddress,(size_t)info.RegionSize,
        info.State,info.Protect,info.Type,queried,failures,stack_stats.calls,stack_stats.hits,stack_stats.accepted,
        stack_stats.no_owner,stack_stats.bounds,stack_stats.query,stack_stats.protection,stack_stats.span);
    SetLastError(saved);
}

int fp_native_stack_prepare(uint64_t address,size_t bytes,uint64_t owner_low,uint64_t owner_high) {
    ++stack_stats.calls;
    if(!owner_low || owner_high<=owner_low) {
        ++stack_stats.no_owner;stack_failure("no_owner",address,bytes,owner_low,owner_high);return 0;
    }
    if(address<owner_low || address>owner_high || bytes>owner_high-address) {
        ++stack_stats.bounds;stack_failure("bounds",address,bytes,owner_low,owner_high);return 0;
    }
    if(fp_native_stack_cached(address,bytes,owner_low,owner_high)){++stack_stats.hits;return 1;}
    uint64_t current=generation();
    DWORD saved=GetLastError();MEMORY_BASIC_INFORMATION info;
    int queried=VirtualQuery((void *)(uintptr_t)address,&info,sizeof(info))!=0;
    int valid=queried && info.State==MEM_COMMIT && info.Type==MEM_PRIVATE && info.Protect==PAGE_READWRITE;
    if(valid) {
        uint64_t low=(uintptr_t)info.BaseAddress,high=low+info.RegionSize;
        if(high<=low)valid=0;
        else {
            if(low<owner_low)low=owner_low;if(high>owner_high)high=owner_high;
            if(cache_allowed()) {
                stack_cache.low=low;stack_cache.high=high;stack_cache.owner_low=owner_low;
                stack_cache.owner_high=owner_high;stack_cache.epoch=current;
            }
            valid=address>=low && address<=high && bytes<=high-address;
        }
    }
    if(valid)++stack_stats.accepted;
    else if(!queried){++stack_stats.query;stack_failure("query",address,bytes,owner_low,owner_high);}
    else if(info.State!=MEM_COMMIT || info.Type!=MEM_PRIVATE || info.Protect!=PAGE_READWRITE) {
        ++stack_stats.protection;stack_failure("protection",address,bytes,owner_low,owner_high);
    } else {++stack_stats.span;stack_failure("span",address,bytes,owner_low,owner_high);}
    SetLastError(saved);return valid;
}
