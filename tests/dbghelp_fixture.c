/* Retain actual core LastError, pointer validation and the exact import router.
 * Unrelated providers return 'not handled' within this isolated router test. */
#define win64_dispatch_import unused_full_core_dispatch
#include "../android/host/win64.c"
#undef win64_dispatch_import
#include "win64_dbghelp.h"
#include "dbghelp_cases.h"
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes) {
    uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;
}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes) {
    if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);
}
static int unrelated_import(CPU *cpu,const char *dll,const char *name){(void)cpu;(void)dll;(void)name;return 0;}
static int unrelated_api(CPU *cpu,const char *name){(void)cpu;(void)name;return 0;}
static int unrelated_notification(CPU *cpu){(void)cpu;return 0;}
#include "core_router.inc"

typedef struct {uint64_t hash;unsigned cleanup_cases,route_cases,policy_cases;} worker_result;
static const char search_path[]="C:\\Symbols";
static uint32_t invoke(int routed,const char *dll,const char *api,uint64_t handle,uint64_t search,uint32_t error) {
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=handle;cpu.rdx=search;cpu.r8=1;cpu.rsp=UINT64_C(0x1234567812345678);before=cpu;
    win64_set_error(UINT32_C(0x12345678));int handled=routed?win64_dispatch_import(&cpu,dll,api):win64_dbghelp_call(&cpu,api);
    if(handled!=1||cpu.rax||win64_get_error()!=error)abort();cpu.rax=before.rax;if(routed)before.rsp+=8;
    if(memcmp(&cpu,&before,sizeof(cpu)))abort();return 0;
}
static void *worker(void *opaque) {
    worker_result *out=opaque;out->hash=UINT64_C(1469598103934665603);
    for(unsigned n=0;n<64;++n)for(unsigned i=0;i<sizeof(dbghelp_handles)/sizeof(*dbghelp_handles);++i){
        invoke(0,NULL,"SymCleanup",dbghelp_handles[i],0,6);out->hash=dbghelp_hash(out->hash,0,6);++out->cleanup_cases;
        invoke(1,"DBGHELP.DLL","SymCleanup",dbghelp_handles[i],0,6);++out->route_cases;
    }
    for(unsigned i=0;i<sizeof(dbghelp_handles)/sizeof(*dbghelp_handles);++i){
        invoke(0,NULL,"SymInitialize",dbghelp_handles[i],0,50);
        invoke(1,"dbghelp.dll","SymInitialize",dbghelp_handles[i],(uintptr_t)search_path,50);out->policy_cases+=2;
        /* An unavailable initialization never creates a cleanup session. */
        invoke(1,"dbghelp.dll","SymCleanup",dbghelp_handles[i],0,6);++out->route_cases;
    }
    return NULL;
}
int main(int argc,char **argv) {
    win64_dbghelp_self_test();win64_region_add((uintptr_t)search_path,sizeof(search_path),0);
    if(argc==2&&!strcmp(argv[1],"--bad-search")){CPU cpu={0};cpu.rcx=UINT64_MAX;cpu.rdx=1;win64_dbghelp_call(&cpu,"SymInitialize");return 99;}
    if(argc!=1)return 3;
    pthread_t threads[4];worker_result workers[4]={0};unsigned cleanup=0,routes=0,policy=0;
    for(unsigned i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,&workers[i]))return 4;
    for(unsigned i=0;i<4;++i){if(pthread_join(threads[i],NULL)||workers[i].hash!=workers[0].hash)return 5;cleanup+=workers[i].cleanup_cases;routes+=workers[i].route_cases;policy+=workers[i].policy_cases;}
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;win64_set_error(UINT32_C(0x12345678));
    if(win64_dispatch_import(&cpu,"kernel32.dll","SymCleanup")||win64_dispatch_import(&cpu,"ucrtbase.dll","SymInitialize")||win64_dispatch_import(&cpu,"dbghelp.dll","SymFromAddr")||memcmp(&cpu,&before,sizeof(cpu))||win64_get_error()!=UINT32_C(0x12345678))return 6;
    printf("{\"success\":true,\"threads\":4,\"cleanup_cases\":%u,\"per_thread_cleanup_hash\":\"%016llx\",\"router_calls\":%u,\"unsupported_initialization_policy_cases\":%u,\"DLL_gate\":true,\"one_router_RSP_pop\":true,\"CPU_and_LastError\":true,\"symbol_session_created\":false}\n",cleanup,(unsigned long long)workers[0].hash,routes,policy);return 0;
}
