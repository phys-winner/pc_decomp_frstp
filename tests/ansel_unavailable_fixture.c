/* Exact production capability route plus unchanged static-C caller bodies. */
#include "cpu64.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define strcasecmp _stricmp
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#include <strings.h>
#define LOCAL _Thread_local
#endif
#define EXPORT "?isAnselAvailable@ansel@@YA_NXZ"
#define SENTINEL UINT64_C(0xe5e30028)
static LOCAL uint32_t guest_error;
static LOCAL unsigned api_calls,logs,cookies,eh_depth;
static LOCAL es3_ehframe_t *eh_top;
static uint64_t cookie=UINT64_C(0x8ce3712546adbf09),log_context;
static uint8_t global_flag;
static char unavailable[]="Ansel is not available.",disabled[]="Ansel is not configured or not available.";
static uint64_t iat=SENTINEL;
int64_t g_image_delta;
static void check(int yes,const char *why){if(!yes){fprintf(stderr,"ANSEL_FIXTURE_FAIL %s\n",why);exit(1);}}
static uint64_t fake_gva(uint64_t va) {
    if(va==UINT64_C(0x141CC7030))return(uintptr_t)&iat;
    if(va==UINT64_C(0x142985AAC))return(uintptr_t)&global_flag;
    if(va==UINT64_C(0x14289C978))return(uintptr_t)&cookie;
    if(va==UINT64_C(0x14296FCA0))return(uintptr_t)&log_context;
    if(va==UINT64_C(0x141FB1610))return(uintptr_t)unavailable;
    if(va==UINT64_C(0x141FB1480))return(uintptr_t)disabled;
    fprintf(stderr,"ANSEL_FIXTURE_UNEXPECTED_GVA %" PRIx64 "\n",va);exit(1);
}
#undef GVA
#define GVA(va) fake_gva(va)
static int capability_import(CPU *c,const char *dll,const char *name) {
#include "ansel_route.inc"
    return 0;
}
void es3_eh_push(es3_ehframe_t *f,uint64_t func,jmp_buf *jb){f->prev=eh_top;f->func=func;f->jb=jb;eh_top=f;++eh_depth;}
void es3_eh_pop(es3_ehframe_t *f){check(eh_top==f&&eh_depth,"EH balanced LIFO");eh_top=f->prev;--eh_depth;}
void frostpunk_unsupported(uint64_t va,const char *text){fprintf(stderr,"ANSEL_FIXTURE_TODO %" PRIx64 " %s\n",va,text);exit(1);}
void dispatch(CPU *c,uint64_t target) {
    if(target==SENTINEL){check(capability_import(c,"AnselSDK64.dll",EXPORT),"static IAT route");++api_calls;return;}
    if(target==UINT64_C(0x140E26740)){check(c->rcx==(uintptr_t)&log_context&&(c->r9==(uintptr_t)unavailable||c->r9==(uintptr_t)disabled),"original unavailable log");++logs;c->rsp+=8;return;}
    if(target==UINT64_C(0x141AE0960)){check(c->rcx==cookie,"original cookie preserved");++cookies;c->rsp+=8;return;}
    fprintf(stderr,"ANSEL_FIXTURE_UNEXPECTED_CALL %" PRIx64 "\n",target);exit(1);
}
void dispatch_jmp(CPU *c,uint64_t target){dispatch(c,target);}
#include "ansel_callers.inc"
static void fp_state(uint64_t state[2]) {
#if defined(_WIN32)
    state[0]=_mm_getcsr();state[1]=0;
#else
    __asm__ volatile("mrs %0,fpcr":"=r"(state[0]));__asm__ volatile("mrs %0,fpsr":"=r"(state[1]));
#endif
}
typedef struct {unsigned calls;} worker_state;
static void provider_case(worker_state *state,const char *dll,const char *name,int expected) {
    uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t oldstack[8];memcpy(oldstack,stack,sizeof(stack));
    CPU c,b;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;b=c;
    uint64_t before[2],after[2];fp_state(before);guest_error=0x5a17;errno=2718;
    check(capability_import(&c,dll,name)==expected,"exact DLL/export gate");fp_state(after);
    check(guest_error==0x5a17&&errno==2718&&!memcmp(before,after,sizeof(before)),"LastError/errno/nativeFP preserved");
    if(expected){check(c.rax==0,"typed false fullRAX");b.rax=0;b.rsp+=8;}
    check(!memcmp(&c,&b,sizeof(c))&&!memcmp(stack,oldstack,sizeof(stack)),"fullCPU/single pop/stack");++state->calls;
}
static void worker_body(worker_state *state) {
    for(unsigned i=0;i<4096;++i)provider_case(state,i&1?"ANSELSDK64.DLL":"anselsdk64.dll",EXPORT,1);
}
#if defined(_WIN32)
static DWORD WINAPI worker(LPVOID value){worker_body(value);return 0;}
#else
static void *worker(void *value){worker_body(value);return NULL;}
#endif
static void original_false_path(unsigned mode) {
    _Alignas(16) uint64_t stack[512];memset(stack,0x7b,sizeof(stack));
    unsigned char object[128],before[128];memset(object,0x7b,sizeof(object));object[0x50]=(unsigned char)(mode==2);memcpy(before,object,sizeof(object));
    CPU c,b;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)&stack[401];c.rcx=(uintptr_t)object;c.r8=(uintptr_t)object;b=c;
    unsigned oldlogs=logs,oldapi=api_calls,oldcookies=cookies;errno=2718;guest_error=0x5a17;
    if(mode==0){L_0001415D84F0(&c);before[0x50]=1;}else L_0001415D8280(&c);
    check(c.rsp==b.rsp+8&&c.rbx==b.rbx&&c.rbp==b.rbp&&c.rdi==b.rdi,"original caller stack/nonvolatile");
    check(!memcmp(object,before,sizeof(object))&&logs==oldlogs+1&&cookies==oldcookies+1&&api_calls==oldapi+(mode!=1),"unavailable skips SDK setup; expected state only");
    check(!eh_top&&!eh_depth&&errno==2718&&guest_error==0x5a17,"caller EH/errors");if(mode)check(c.rax==0,"original second caller returns unavailable false");
}
int main(void) {
    worker_state state={0};
    const char *dlls[]={"AnselSDK64.dll","anselsdk64.dll","ANSELSDK64.DLL","AnselSDK64.dll.extra","AnselSDK32.dll","USER32.dll"};
    const char *names[]={EXPORT,"?isAnselAvailable@ansel@@YA_NXZx","?isAnselAvailable@ansel@@YA_NXZ ","?isAnselAvailable@ansel@@YA_NXZ\n","?ISAnselAvailable@ansel@@YA_NXZ","?setConfiguration@ansel@@YA?AW4SetConfigurationStatus@1@AEBUConfiguration@1@@Z","?updateCamera@ansel@@YAXAEAUCamera@1@@Z","?rotationMatrixVectorsToQuaternion@ansel@@YAXAEBUVec3@nv@@00AEAUQuat@3@@Z"};
    for(unsigned d=0;d<sizeof(dlls)/sizeof(dlls[0]);++d)for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n)provider_case(&state,dlls[d],names[n],d<3&&n==0);
    for(unsigned i=0;i<128;++i){original_false_path(0);original_false_path(1);original_false_path(2);}
    worker_state states[4]={{0}};
#if defined(_WIN32)
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,&states[i],0,NULL);check(threads[i]!=NULL,"thread create");}
    for(unsigned i=0;i<4;++i){check(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0,"thread join");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)check(!pthread_create(&threads[i],NULL,worker,&states[i]),"thread create");
    for(unsigned i=0;i<4;++i)check(!pthread_join(threads[i],NULL),"thread join");
#endif
    unsigned parallel=0;for(unsigned i=0;i<4;++i)parallel+=states[i].calls;
    printf("{\"success\":true,\"provider_cases\":%u,\"parallel_calls\":%u,\"original_static_C_false_paths\":384,\"fullCPU_errno_LastError_nativeFP\":true,\"SDK_setup_skipped\":true,\"original_bytes_executed\":false,\"Ansel_supported\":false,\"gameplay_proven\":false}\n",state.calls,parallel);
    return 0;
}
