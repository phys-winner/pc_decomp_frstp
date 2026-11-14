#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <objbase.h>
#include <dinput.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../native/windows/input.h"

static CPU cpu(uint64_t *stack) {
    CPU c={0};memset(stack,0,64);stack[0]=UINT64_C(0xfeed1234);c.rsp=(uintptr_t)stack;return c;
}
static void returned(const CPU *c,uint64_t *stack,HRESULT result) {
    assert(c->rax==(uint32_t)result && c->rip==UINT64_C(0xfeed1234) && c->rsp==(uintptr_t)stack+8);
}
static void release(void *object) {IUnknown_Release((IUnknown *)object);}
static HRESULT initialize(void *object,HINSTANCE instance,DWORD version) {
    typedef HRESULT (WINAPI *call)(void *,HINSTANCE,DWORD);
    return ((call)(*(void ***)object)[7])(object,instance,version);
}
static void *co_object(const GUID *iid) {
    void *object=NULL;assert(SUCCEEDED(CoCreateInstance(&CLSID_DirectInput8,NULL,CLSCTX_INPROC_SERVER,iid,&object)));
    assert(object);return object;
}

int main(void) {
    assert(SUCCEEDED(CoInitializeEx(NULL,COINIT_MULTITHREADED)));
    HINSTANCE host=GetModuleHandleW(NULL);
    void *manual=VirtualAlloc(NULL,65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    assert(manual);memcpy(manual,host,4096);
    fp_windows_input_init((uintptr_t)manual);
    const GUID *iids[]={&IID_IDirectInput8A,&IID_IDirectInput8W};
    for(unsigned i=0;i<2;++i) {
        void *old=co_object(iids[i]);
        HRESULT original=initialize(old,(HINSTANCE)manual,DIRECTINPUT_VERSION);
        release(old);
        assert(FAILED(original));
        uint64_t stack[8];CPU c=cpu(stack);
        struct {uint64_t before;void *value;uint64_t after;} out={0x12345678,NULL,0x87654321};
        c.rcx=(uintptr_t)&CLSID_DirectInput8;c.rdx=0;c.r8=CLSCTX_INPROC_SERVER;c.r9=(uintptr_t)iids[i];
        stack[5]=(uintptr_t)&out.value;
        assert(fp_windows_input_call(&c,(uintptr_t)CoCreateInstance,"CoCreateInstance"));
        returned(&c,stack,S_OK);assert(out.value && out.before==0x12345678 && out.after==0x87654321);
        void *object=out.value;
        c=cpu(stack);c.rcx=(uintptr_t)object;c.rdx=(uintptr_t)manual;c.r8=DIRECTINPUT_VERSION;
        uint64_t target=(uintptr_t)(*(void ***)object)[7];
        assert(fp_windows_input_call(&c,target,NULL));returned(&c,stack,S_OK);
        release(object);
        void *native=NULL;HRESULT expected=DirectInput8Create(host,DIRECTINPUT_VERSION,iids[i],&native,NULL);
        assert(SUCCEEDED(expected)&&native);release(native);
        out.value=NULL;c=cpu(stack);c.rcx=(uintptr_t)manual;c.rdx=DIRECTINPUT_VERSION;
        c.r8=(uintptr_t)iids[i];c.r9=(uintptr_t)&out.value;
        assert(fp_windows_input_call(&c,(uintptr_t)DirectInput8Create,"DirectInput8Create"));
        returned(&c,stack,expected);assert(out.value && out.before==0x12345678 && out.after==0x87654321);
        release(out.value);
        SetLastError(0x1234);
        expected=DirectInput8Create(host,DIRECTINPUT_VERSION,iids[i],NULL,NULL);
        DWORD native_error=GetLastError();assert(FAILED(expected));
        c=cpu(stack);c.rcx=(uintptr_t)manual;c.rdx=DIRECTINPUT_VERSION;c.r8=(uintptr_t)iids[i];c.r9=0;
        SetLastError(0x1234);
        assert(fp_windows_input_call(&c,(uintptr_t)DirectInput8Create,"DirectInput8Create"));
        assert(GetLastError()==native_error);returned(&c,stack,expected);
        GUID unknown={0};native=(void *)(uintptr_t)1;
        expected=DirectInput8Create(host,DIRECTINPUT_VERSION,&unknown,&native,NULL);
        assert(FAILED(expected));
        out.value=(void *)(uintptr_t)1;c=cpu(stack);c.rcx=(uintptr_t)manual;c.rdx=DIRECTINPUT_VERSION;
        c.r8=(uintptr_t)&unknown;c.r9=(uintptr_t)&out.value;
        assert(fp_windows_input_call(&c,(uintptr_t)DirectInput8Create,"DirectInput8Create"));
        returned(&c,stack,expected);assert((out.value==NULL)==(native==NULL));
        void *reference=co_object(iids[i]);expected=initialize(reference,host,0);release(reference);
        object=co_object(iids[i]);c=cpu(stack);c.rcx=(uintptr_t)object;c.rdx=(uintptr_t)manual;c.r8=0;
        assert(fp_windows_input_call(&c,target,NULL));returned(&c,stack,expected);release(object);
        c=cpu(stack);c.rcx=(uintptr_t)&CLSID_DirectInput8;c.r9=(uintptr_t)&IID_IUnknown;
        CPU saved=c;
        assert(!fp_windows_input_call(&c,(uintptr_t)CoCreateInstance,"CoCreateInstance"));
        assert(!memcmp(&saved,&c,sizeof(c)));
        c=cpu(stack);c.rcx=(uintptr_t)&unknown;c.r9=(uintptr_t)iids[i];saved=c;
        assert(!fp_windows_input_call(&c,(uintptr_t)CoCreateInstance,"CoCreateInstance"));
        assert(!memcmp(&saved,&c,sizeof(c)));
        c=cpu(stack);c.rdx=(uintptr_t)host;saved=c;
        assert(!fp_windows_input_call(&c,target,NULL));assert(!memcmp(&saved,&c,sizeof(c)));
        printf("DIRECTINPUT_FIXTURE iid=%c native_manual_hresult=0x%08lx translated_hresult=0x00000000 factory_errors_preserved=1 scope_preserved=1\n",i?'W':'A',(unsigned long)original);
    }
    assert(VirtualFree(manual,0,MEM_RELEASE));CoUninitialize();
    puts("WINDOWS_INPUT_PASS real_COM=1 manual_image_NX=1 original_game_executed=0");
    return 0;
}
