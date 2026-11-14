/* Production snapshot provider and exact extracted route; unrelated providers are mocked. */
#ifdef _WIN32
#include "win64_environment.h"
#include "win64.h"
#include <windows.h>
#include <stdio.h>
#include <errno.h>
#define strcasecmp _stricmp
#define TLS __declspec(thread)
typedef struct {uint64_t at;size_t n;int write,active;} test_region;
static test_region test_regions[2048];static SRWLOCK regions_lock=SRWLOCK_INIT;
static TLS uint32_t guest_error;static TLS int crt_errno;
static void invalid(const char *s){fprintf(stderr,"STOP fixture checked pointer %s\n",s);_Exit(73);}
void win64_set_error(uint32_t e){guest_error=e;}uint32_t win64_get_error(void){return guest_error;}
void win64_stdio_set_errno(int e){crt_errno=e;}
void win64_region_add(uint64_t at,size_t n,int write){AcquireSRWLockExclusive(&regions_lock);for(unsigned i=0;i<2048;++i)if(!test_regions[i].active){test_regions[i]=(test_region){at,n,write,1};ReleaseSRWLockExclusive(&regions_lock);return;}abort();}
void win64_region_remove(uint64_t at){AcquireSRWLockExclusive(&regions_lock);for(unsigned i=0;i<2048;++i)if(test_regions[i].at==at)test_regions[i].active=0;ReleaseSRWLockExclusive(&regions_lock);}
void *win64_pointer(uint64_t at,size_t n,int write){int ok=0;AcquireSRWLockShared(&regions_lock);for(unsigned i=0;i<2048;++i){test_region *r=&test_regions[i];if(r->active&&at>=r->at&&at-r->at<=r->n&&n<=r->n-(at-r->at)&&(!write||r->write)){ok=1;break;}}ReleaseSRWLockShared(&regions_lock);if(!ok)invalid("range/permission");return(void*)(uintptr_t)at;}
static void metadata_lock(void){}static void metadata_unlock(void){}
static int is_crt(const char*dll){(void)dll;return 0;} /* Unrelated host fixture route. */
#else
#define win64_dispatch_import unused_full_core_dispatch
#include "../android/host/win64.c"
#undef win64_dispatch_import
#define TLS _Thread_local
static TLS unsigned char test_teb[256];
uint64_t frostpunk_teb_read(uint64_t off,unsigned n){uint64_t v=0;if(off>sizeof(test_teb)||n>sizeof(test_teb)-off||n>8)abort();memcpy(&v,test_teb+off,n);return v;}
void frostpunk_teb_write(uint64_t off,uint64_t v,unsigned n){if(off>sizeof(test_teb)||n>sizeof(test_teb)-off||n>8)abort();memcpy(test_teb+off,&v,n);}
#endif
#include "win64_stdio.h"
int win64_environment_windows_call(CPU*,const char*);
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;return 0;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;return 0;}
static int unrelated_notification(CPU*c){(void)c;return 0;}
#include "environment_router.inc"
#include "native_rows.h"
static TLS unsigned checks;
static void check(int ok,const char *reason){++checks;if(!ok){fprintf(stderr,"ENV_WIN32_STAGING_FAIL %s\n",reason);_Exit(1);}}
static uint32_t query(int route,const char*dll,const char*name,void*out,uint32_t size,uint32_t initial,uint32_t error){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)name;c.rdx=(uintptr_t)out;c.r8=UINT64_C(0xfeca123400000000)|size;c.rsp=UINT64_C(0x1234567890abcdef);before=c;
    win64_stdio_set_errno(2718);errno=31337;win64_set_error(initial);
    int h=route?win64_dispatch_import(&c,dll,"GetEnvironmentVariableA"):win64_environment_windows_call(&c,"GetEnvironmentVariableA");
    check(h,"handled");check(errno==31337&&crt_errno==2718,"native and guest errno preserved");check(win64_get_error()==error,"LastError");uint32_t result=(uint32_t)c.rax;check(c.rax==result,"DWORD zeroextend");c.rax=before.rax;if(route)before.rsp+=8;check(!memcmp(&c,&before,sizeof(c)),"fullCPU and sole pop");return result;
}
static unsigned run_rows(void){unsigned count=0;for(unsigned i=0;i<sizeof(native_rows)/sizeof(*native_rows);++i){const native_row*r=&native_rows[i];unsigned char bytes[48];memset(bytes,0xa5,sizeof(bytes));win64_region_add((uintptr_t)bytes,sizeof(bytes),1);
    check(query(i%3!=0,i&1?"KERNEL32.DLL":"kernelbase.dll",r->name,bytes+8,r->capacity,r->error_before,r->error_after)==r->result,"native result");check(!memcmp(bytes,r->bytes,sizeof(bytes)),"native fullbuffer and guards");win64_region_remove((uintptr_t)bytes);++count;}return count;}
static void extra(void){
    const char *n=native_rows[0].name;unsigned char exact[6];win64_region_add((uintptr_t)exact,6,1);memset(exact,0xa5,6);
    check(query(0,NULL,n,NULL,0,17,17)==6&&query(1,"kernel32.dll",n,(void*)1,1,19,19)==6,"query/insufficient never dereferences output");
    check(query(1,"kernelbase.dll",n,exact,UINT32_MAX,23,23)==5&&!memcmp(exact,"dummy",6),"validate only actualwrite notcapacity");
    check(query(0,NULL,NULL,(void*)1,UINT32_MAX,13,203)==0,"NULLname measured missing");
    CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(27);check(!win64_dispatch_import(&c,"user32.dll","GetEnvironmentVariableA")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==27,"DLLgate");check(!win64_environment_windows_call(&c,"GetEnvironmentVariableW")&&!memcmp(&c,&b,sizeof(c)),"Aonly");
    c.rcx=(uintptr_t)n;check(win64_environment_call(&c,"getenv"),"CRTgetter retained");uint64_t stable=c.rax;check(stable&&!strcmp((const char*)(uintptr_t)stable,"dummy"),"CRTvalue");
    check(!win64_environment_configure(NULL,0),"published snapshotimmutable");c.rcx=(uintptr_t)n;check(win64_environment_call(&c,"getenv")&&c.rax==stable,"CRTpointer stable");
    static uint16_t wide[]={ 'S','D','L','_','A','U','D','I','O','D','R','I','V','E','R',0};win64_region_add((uintptr_t)wide,sizeof(wide),0);c.rcx=(uintptr_t)wide;check(win64_environment_call(&c,"_wgetenv"),"wideCRTretained");static const uint16_t value[]={'d','u','m','m','y',0};check(!memcmp((void*)(uintptr_t)c.rax,value,sizeof(value)),"wideCRTvalue");win64_region_remove((uintptr_t)wide);win64_region_remove((uintptr_t)exact);
}
int main(int argc,char**argv){
    win64_environment_self_test();check(win64_environment_configure(configured_values,sizeof(configured_values)/sizeof(*configured_values)),"configureexplicit");
    for(unsigned i=0;i<sizeof(native_rows)/sizeof(*native_rows);++i)win64_region_add((uintptr_t)native_rows[i].name,strlen(native_rows[i].name)+1,0);
    if(argc>1){const char *name=native_rows[0].name;unsigned char bytes[8];CPU c={0};c.rcx=(uintptr_t)name;c.rdx=(uintptr_t)bytes;c.r8=6;
        if(!strcmp(argv[1],"--short"))win64_region_add(c.rdx,5,1);else if(!strcmp(argv[1],"--readonly"))win64_region_add(c.rdx,6,0);else if(!strcmp(argv[1],"--null-output"))c.rdx=0;else if(!strcmp(argv[1],"--bad-name"))c.rcx=1;else return 2;
        win64_environment_windows_call(&c,"GetEnvironmentVariableA");return 99;}
    unsigned rows=run_rows();extra();printf("{\"success\":true,\"native_cases\":%u,\"checks\":%u,\"full_CPU\":true,\"single_pop\":true,\"original_CRT_getters\":true,\"staging_only\":false,\"game_boot_proven\":false}\n",rows,checks);return 0;
}
