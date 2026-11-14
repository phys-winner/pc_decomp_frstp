/* DirectInput requires a loader-registered HINSTANCE at its native boundary. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

static const GUID input_class = {0x25e609e4,0xb259,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00}};
static const GUID input_a = {0xbf798030,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
static const GUID input_w = {0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
typedef struct input_target {uint64_t address;struct input_target *next;} input_target;
static input_target *initialize_targets;
static SRWLOCK target_lock = SRWLOCK_INIT;
static uint64_t guest_base;

static int accessible(uint64_t address, size_t bytes, int write) {
    uint64_t end=address+bytes;
    if(!address || end<address)return 0;
    while(address<end) {
        MEMORY_BASIC_INFORMATION info;
        if(!VirtualQuery((void *)(uintptr_t)address,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return 0;
        DWORD allowed=write?(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY):
            (PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY);
        if(!(info.Protect&allowed))return 0;
        uint64_t next=(uintptr_t)info.BaseAddress+info.RegionSize;
        if(next<=address)return 0;
        address=next<end?next:end;
    }
    return 1;
}

static int input_iid(uint64_t address) {
    if(!accessible(address,sizeof(GUID),0))return 0;
    const void *iid=(const void *)(uintptr_t)address;
    return !memcmp(iid,&input_a,sizeof(GUID)) || !memcmp(iid,&input_w,sizeof(GUID));
}

static int learned(uint64_t address) {
    int found=0;
    AcquireSRWLockShared(&target_lock);
    for(input_target *entry=initialize_targets;entry;entry=entry->next)
        if(entry->address==address){found=1;break;}
    ReleaseSRWLockShared(&target_lock);
    return found;
}

static void remember(void *object) {
    if(!accessible((uintptr_t)object,8,0))goto invalid;
    uint64_t *table=*(uint64_t **)object;
    if(!accessible((uintptr_t)table,8*8,0))goto invalid;
    uint64_t address=table[7];
    if(learned(address))return;
    MEMORY_BASIC_INFORMATION info;
    HMODULE module;
    if(!VirtualQuery((void *)(uintptr_t)address,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        !(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            (LPCWSTR)(uintptr_t)address,&module))goto invalid;
    input_target *entry=malloc(sizeof(*entry));
    if(!entry)goto invalid;
    entry->address=address;
    AcquireSRWLockExclusive(&target_lock);
    entry->next=initialize_targets;initialize_targets=entry;
    ReleaseSRWLockExclusive(&target_lock);
    return;
invalid:
    fputs("STOP invalid native DirectInput Initialize identity\n",stderr);
    ExitProcess(73);
}

void fp_windows_input_init(uint64_t guest_image_base) {guest_base=guest_image_base;}

int fp_windows_input_call(CPU *c,uint64_t target,const char *name) {
    if(!guest_base)return 0;
    DWORD previous_error=GetLastError();
    int cocreate=name && !strcmp(name,"CoCreateInstance");
    int factory=name && !strcmp(name,"DirectInput8Create");
    int initialize=c->rdx==guest_base && learned(target);
    if(cocreate && (!accessible(c->rcx,sizeof(GUID),0) ||
        memcmp((void *)(uintptr_t)c->rcx,&input_class,sizeof(GUID)) || !input_iid(c->r9))) {
        SetLastError(previous_error);return 0;
    }
    if(!cocreate && !factory && !initialize){SetLastError(previous_error);return 0;}
    if(!accessible(c->rsp,8,0) || ((cocreate || factory) &&
        (c->rsp>UINT64_MAX-40 || !accessible(c->rsp+40,8,0)))) {
        fputs("STOP invalid DirectInput native argument stack\n",stderr);ExitProcess(73);
    }
    uint64_t ret=rd64(c->rsp),fifth=(cocreate || factory)?rd64(c->rsp+40):0;
    HRESULT result;
    void **output=cocreate?(void **)(uintptr_t)fifth:factory?(void **)(uintptr_t)c->r9:NULL;
    if(output && !accessible((uintptr_t)output,8,1)) {
        fputs("STOP invalid DirectInput native output span\n",stderr);ExitProcess(73);
    }
    int observe=0,translated=0;
    SetLastError(previous_error);
    if(cocreate) {
        typedef HRESULT (WINAPI *create_fn)(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID *);
        result=((create_fn)(uintptr_t)target)((REFCLSID)(uintptr_t)c->rcx,(LPUNKNOWN)(uintptr_t)c->rdx,
            (DWORD)c->r8,(REFIID)(uintptr_t)c->r9,output);
        observe=1;
    } else if(factory) {
        typedef HRESULT (WINAPI *factory_fn)(HINSTANCE,DWORD,REFIID,LPVOID *,LPUNKNOWN);
        HINSTANCE instance=(HINSTANCE)(uintptr_t)c->rcx;
        if(c->rcx==guest_base){instance=GetModuleHandleW(NULL);translated=1;}
        observe=input_iid(c->r8);
        SetLastError(previous_error);
        result=((factory_fn)(uintptr_t)target)(instance,(DWORD)c->rdx,(REFIID)(uintptr_t)c->r8,
            output,(LPUNKNOWN)(uintptr_t)fifth);
    } else {
        typedef HRESULT (WINAPI *initialize_fn)(void *,HINSTANCE,DWORD);
        HINSTANCE instance=GetModuleHandleW(NULL);
        SetLastError(previous_error);
        result=((initialize_fn)(uintptr_t)target)((void *)(uintptr_t)c->rcx,instance,(DWORD)c->r8);
        translated=1;
    }
    DWORD result_error=GetLastError();
    if(SUCCEEDED(result) && observe && output && accessible((uintptr_t)output,8,0) && *output)remember(*output);
    if(translated)fprintf(stderr,"DIRECTINPUT_MODULE_IDENTITY boundary=%s hresult=0x%08lx guest_instance_translated=1\n",
        factory?"DirectInput8Create":"Initialize",(unsigned long)result);
    c->rax=(uint32_t)result;c->rip=ret;c->rsp+=8;
    SetLastError(result_error);
    return 1;
}
