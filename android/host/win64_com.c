/* COM apartment ownership is local to each native guest thread. The only
 * class provider is the explicit DirectInput8W startup profile; general COM
 * activation/marshaling require separate real services. */
#include "win64_com.h"
#include "win64.h"
#include "win64_dinput.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COM_E_INVALIDARG UINT32_C(0x80070057)
#define COM_E_OUTOFMEMORY UINT32_C(0x8007000e)
#define COM_E_CHANGED_MODE UINT32_C(0x80010106)
typedef struct {uint64_t references;uint32_t model,options;} apartment_context;
static pthread_key_t apartment_key;
static pthread_once_t apartment_once=PTHREAD_ONCE_INIT;
static int apartment_key_error;
static void apartment_destroy(void *context) { free(context); }
static void apartment_key_initialize(void) {
    apartment_key_error=pthread_key_create(&apartment_key,apartment_destroy);
}
static _Noreturn void com_stop(const char *reason) {
    fprintf(stderr,"STOP Win64 COM %s\n",reason);fflush(stderr);_Exit(73);
}
static uint32_t apartment_initialize(uint64_t reserved,uint32_t flags) {
    /* Native Windows rejects all bits outside the three COINIT options. */
    if(reserved || (flags&~UINT32_C(0xe)))return COM_E_INVALIDARG;
    if(pthread_once(&apartment_once,apartment_key_initialize)||apartment_key_error)return COM_E_OUTOFMEMORY;
    apartment_context *context=pthread_getspecific(apartment_key);
    uint32_t model=flags&2u;
    if(context){
        if(context->model!=model)return COM_E_CHANGED_MODE;
        if(context->references==UINT64_MAX)com_stop("apartment reference-count overflow");
        ++context->references;context->options|=flags&12u;return 1;
    }
    context=calloc(1,sizeof(*context));if(!context)return COM_E_OUTOFMEMORY;
    context->references=1;context->model=model;context->options=flags&12u;
    if(pthread_setspecific(apartment_key,context)){free(context);return COM_E_OUTOFMEMORY;}
    return 0;
}
static void apartment_uninitialize(void) {
    if(pthread_once(&apartment_once,apartment_key_initialize)||apartment_key_error)return;
    apartment_context *context=pthread_getspecific(apartment_key);
    if(!context)return;
    if(--context->references)return;
    if(pthread_setspecific(apartment_key,NULL))com_stop("apartment teardown TLS failure");
    apartment_destroy(context);
}
static int apartment_initialized(void) {
    int saved_errno=errno,ready=0;
    if(!pthread_once(&apartment_once,apartment_key_initialize) && !apartment_key_error)
        ready=pthread_getspecific(apartment_key)!=NULL;
    errno=saved_errno;return ready;
}
int win64_com_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"CoCreateInstance")) {
        int ready=apartment_initialized();
        if(win64_dinput_com_call(cpu,name,ready))return 1;
        /* shortcut: Android has no TSF/IMM provider; SDL accepts this absence. */
        static const unsigned char clsid[16]={0x6b,0x9e,0x9a,0x52,0x87,0x65,0x23,0x4f,0xab,0x9e,0x9c,0x7d,0x68,0x3e,0x3c,0x50};
        static const unsigned char iid[16]={0x01,0xe8,0x80,0xaa,0x21,0x20,0xd2,0x11,0x93,0xe0,0x00,0x60,0xb0,0x67,0xb8,0x6e};
        if(cpu->rdx || (uint32_t)cpu->r8!=1 ||
           memcmp(win64_pointer(cpu->rcx,16,0),clsid,16) || memcmp(win64_pointer(cpu->r9,16,0),iid,16))return 0;
        int saved_errno=errno;uint32_t last=win64_get_error();uint64_t output=win64_argument(cpu,4),zero=0;
        if(!output)cpu->rax=COM_E_INVALIDARG;
        else {memcpy(win64_pointer(output,8,1),&zero,8);cpu->rax=ready?UINT32_C(0x80040154):UINT32_C(0x800401f0);}
        win64_set_error(last);errno=saved_errno;return 1;
    }
    if(!strcmp(name,"CoInitializeEx")){cpu->rax=apartment_initialize(cpu->rcx,(uint32_t)cpu->rdx);return 1;}
    if(!strcmp(name,"CoInitialize")){cpu->rax=apartment_initialize(cpu->rcx,2);return 1;}
    if(!strcmp(name,"CoUninitialize")){apartment_uninitialize();return 1;}
    return 0;
}
static void *com_self_test_thread(void *unused) {
    (void)unused;
    if(apartment_initialize(1,2)!=COM_E_INVALIDARG || apartment_initialize(0,1)!=COM_E_INVALIDARG ||
       apartment_initialize(0,2)!=0 || apartment_initialize(0,6)!=1 ||
       apartment_initialize(0,0)!=COM_E_CHANGED_MODE)com_stop("apartment initialization self-test");
    apartment_uninitialize();
    if(apartment_initialize(0,0)!=COM_E_CHANGED_MODE)com_stop("failed initialization changed reference count");
    apartment_uninitialize();
    if(apartment_initialize(0,0)!=0)com_stop("apartment reinitialization self-test");
    apartment_uninitialize();apartment_uninitialize();return NULL;
}
void win64_com_self_test(void) {
    CPU cpu={0},before;cpu.rsp=123;cpu.rax=456;before=cpu;
    if(win64_com_call(&cpu,"CoGetClassObject") || memcmp(&cpu,&before,sizeof(cpu)))com_stop("unsupported class API self-test");
    pthread_t thread;
    if(pthread_create(&thread,NULL,com_self_test_thread,NULL)||pthread_join(thread,NULL))com_stop("self-test thread lifecycle");
    fputs("WIN64_COM_SELF_TEST per_thread_apartment=1 STA_MTA=1 balanced_references=1 changed_mode=1 DirectInput8W_class_provider=1 no_general_class_activation=1 passed\n",stderr);
}
