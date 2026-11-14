/* Extracted production XInput clause, common router, error accessors and self-test. */
#include "win64.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <xmmintrin.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <stdatomic.h>
#include <pthread.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

static _Thread_local uint64_t teb[32];
static _Thread_local uint32_t last_error;
static _Thread_local int crt_errno;
#ifdef _WIN32
static volatile LONG64 calls;
#else
static _Atomic uint64_t calls;
#endif
static unsigned char *guard,*readonly;
static size_t page;
static void check(int yes,const char *why){if(!yes){fprintf(stderr,"XINPUT_FIXTURE_FAIL %s\n",why);exit(1);}}
static void invalid(const char *why){check(0,why);}
uint64_t frostpunk_teb_read(uint64_t at,unsigned n){uint64_t v=0;check(at+n<=sizeof(teb),"TEB read");memcpy(&v,(unsigned char*)teb+at,n);return v;}
void frostpunk_teb_write(uint64_t at,uint64_t v,unsigned n){check(at+n<=sizeof(teb),"TEB write");memcpy((unsigned char*)teb+at,&v,n);}
void win64_stdio_set_errno(int value){crt_errno=value;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;return 0;}
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;return 0;}
static int unrelated_notification(CPU*c){(void)c;return 0;}
static void metadata_lock(void){invalid("unexpected metadata lock");}
static void metadata_unlock(void){invalid("unexpected metadata unlock");}
void win64_region_mutation_begin(void){invalid("unexpected memory mutation");}
void win64_region_mutation_end(void){invalid("unexpected memory mutation end");}
void *win64_pointer(uint64_t p,size_t n,int w){(void)p;(void)n;(void)w;invalid("disconnected XInput touched guest pointer");return NULL;}
#include "xinput_production.inc"
#include "xinput_native_rows.h"

static void environment(uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(value[0]),"=r"(value[1]));
#else
    value[0]=_mm_getcsr();value[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static void one(size_t row,unsigned upper,unsigned pattern,const char*dll){
    unsigned char bytes[32],before[32];memset(bytes,0xa5,sizeof(bytes));memset(bytes+8,0,16);memcpy(before,bytes,sizeof(bytes));
    uint64_t stack[8]={0};void *pointers[]={bytes+8,NULL,(void*)1,(void*)UINTPTR_MAX,guard+page,readonly,guard+page-2};
    const char *name=native_rows[row].api?"XInputSetState":"XInputGetState";
    CPU c;memset(&c,(int)(0xa5u^pattern*37u),sizeof(c));c.rcx=((uint64_t)upper<<32)|native_rows[row].index;
    c.rdx=(uintptr_t)pointers[native_rows[row].kind];c.rsp=(uintptr_t)&stack[3];CPU expected=c;
    expected.rax=native_rows[row].result;expected.rsp+=8;
    uint64_t saved[2],native[2],after[2];environment(saved);memcpy(native,saved,sizeof(native));
#if defined(__aarch64__)
    native[0]=(native[0]&~((UINT64_C(3)<<22)|(UINT64_C(31)<<8)|(UINT64_C(1)<<15)))|((uint64_t)(pattern&3u)<<22);
    native[1]=0x15;
#else
    native[0]=0x1f95u|((pattern&3u)<<13);
#endif
    set_environment(native);win64_set_error(0x51cafe);crt_errno=31337;errno=2718;
    int handled=win64_dispatch_import(&c,dll,name);int host_errno=errno;
    uint32_t error=win64_get_error();environment(after);set_environment(saved);
    check(handled==1&&!memcmp(&c,&expected,sizeof(c)),"full CPU and single stack pop");
    check(error==native_rows[row].error&&host_errno==native_rows[row].native_errno&&crt_errno==31337,"native/guest errno and TEB LastError");
    check(!memcmp(native,after,sizeof(native)),"native FP environment");
    check(!memcmp(bytes,before,sizeof(bytes)),"input/output payload and canaries untouched");
    check(!readonly[0]&&!guard[page-2]&&!guard[page-1],"read-only and guard-prefix memory untouched");
#ifdef _WIN32
    InterlockedIncrement64(&calls);
#else
    atomic_fetch_add_explicit(&calls,1,memory_order_relaxed);
#endif
}
static unsigned negative_namespaces(void){
    const char *dlls[]={"xinput1_4.dll","xinput1_3.dll","xinput9_1_0.dll.extra","C:\\Windows\\System32\\xinput9_1_0.dll","ucrtbase.dll","KERNEL32.dll"};
    const char *names[]={"XInputSetState","XInputGetState","xinputsetstate","XInputSetStateA","XInputSetState ","XInputGetStateEx"};
    unsigned count=0;
    for(unsigned i=0;i<sizeof(dlls)/sizeof(*dlls);++i)for(unsigned j=0;j<2;++j){
        CPU c;memset(&c,0xa5,sizeof(c));CPU before=c;win64_set_error(0x51cafe);crt_errno=31337;errno=2718;
        check(!win64_dispatch_import(&c,dlls[i],names[j])&&!memcmp(&c,&before,sizeof(c))&&
              win64_get_error()==0x51cafe&&errno==2718&&crt_errno==31337,"unrelated DLL decline unchanged");++count;
    }
    for(unsigned j=2;j<sizeof(names)/sizeof(*names);++j){
        CPU c;memset(&c,0x5a,sizeof(c));CPU before=c;win64_set_error(0x51cafe);crt_errno=31337;errno=2718;
        check(!win64_dispatch_import(&c,"XINPUT9_1_0.dll",names[j])&&!memcmp(&c,&before,sizeof(c))&&
              win64_get_error()==0x51cafe&&errno==2718&&crt_errno==31337,"unknown exact name decline unchanged");++count;
    }
    return count;
}
#ifdef _WIN32
static DWORD WINAPI worker(void *argument){
#else
static void *worker(void *argument){
#endif
    unsigned id=(unsigned)(uintptr_t)argument;
    for(unsigned i=0;i<256;++i)one((i*7u+id)%NATIVE_ROW_COUNT,id*0x11111111u,i,"XiNpUt9_1_0.DlL");
    return 0;
}
int main(void){
#ifdef _WIN32
    SYSTEM_INFO info;GetSystemInfo(&info);page=info.dwPageSize;
    guard=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    readonly=VirtualAlloc(NULL,page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);DWORD old;
    check(guard&&readonly&&VirtualProtect(guard+page,page,PAGE_NOACCESS,&old)&&
          VirtualProtect(readonly,page,PAGE_READONLY,&old),"guard mappings");
#else
    page=(size_t)sysconf(_SC_PAGESIZE);guard=mmap(NULL,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    readonly=mmap(NULL,page,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    check(guard!=MAP_FAILED&&readonly!=MAP_FAILED&&!mprotect(guard+page,page,PROT_NONE),"guard mappings");
#endif
    const char *dlls[]={"xinput9_1_0.dll","XINPUT9_1_0.dll","XiNpUt9_1_0.DlL"};
    const unsigned upper[]={0,0x12345678u,UINT32_MAX};
    for(size_t i=0;i<NATIVE_ROW_COUNT;++i)for(unsigned d=0;d<3;++d)for(unsigned u=0;u<3;++u)
        for(unsigned pattern=0;pattern<4;++pattern)one(i,upper[u],pattern,dlls[d]);
    unsigned negative=negative_namespaces();xinput_disconnected_self_test();
#ifdef _WIN32
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)(i+1),0,NULL);check(threads[i]!=NULL,"create worker");}
    check(WaitForMultipleObjects(4,threads,TRUE,INFINITE)==WAIT_OBJECT_0,"join workers");
    for(unsigned i=0;i<4;++i)CloseHandle(threads[i]);
    VirtualFree(readonly,0,MEM_RELEASE);VirtualFree(guard,0,MEM_RELEASE);
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)check(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)(i+1)),"create worker");
    for(unsigned i=0;i<4;++i)check(!pthread_join(threads[i],NULL),"join workers");
    munmap(readonly,page);munmap(guard,page*2);
#endif
#ifdef _WIN32
    uint64_t count=(uint64_t)InterlockedCompareExchange64(&calls,0,0);
#else
    uint64_t count=atomic_load_explicit(&calls,memory_order_relaxed);
#endif
    printf("{\"success\":true,\"native_oracle_rows\":%u,\"full_CPU_calls\":%" PRIu64 ",\"parallel_calls\":1024,\"negative_namespaces\":%u,\"existing_selftest_cases\":48,\"pointer_reads\":0,\"buffers_untouched\":true,\"native_FP_errno_TEB_TLS\":true,\"single_RSP_pop\":true}\n",
           NATIVE_ROW_COUNT,count,negative);return 0;
}
