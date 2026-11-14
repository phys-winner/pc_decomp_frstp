/* Native API oracle data, real production handler, and full ARM USER32 regression. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "cpu64.h"
#include <errno.h>
#include <stdio.h>
static __declspec(thread) uint32_t error;
static void win64_set_error(uint32_t value){error=value;}
static uint32_t win64_get_error(void){return error;}
static uint64_t failure(uint32_t value,uint64_t result){win64_set_error(value);return result;}
#include "map_extract.inc"
#else
#define main window_regression_main
#include "window_fixture.c"
#undef main
#endif
#include "map_native.h"
#include <inttypes.h>
typedef struct {unsigned calls;} map_state;
static void map_check(int condition,const char *why) {
    if(!condition){fprintf(stderr,"MAPVK_FIXTURE_FAIL %s error=%u errno=%d\n",why,win64_get_error(),errno);exit(1);}
}
static int map_import(CPU *c,const char *dll,const char *name) {
    const int user=!strcmp(dll,"USER32.DLL");
#include "map_route.inc"
    return 0;
}
static void map_call(map_state *state,const char *name,uint64_t code,uint64_t type,uint64_t layout,int route,uint64_t result,uint32_t expected_error) {
    uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t old_stack[8];memcpy(old_stack,stack,sizeof(stack));
    CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=code;c.rdx=type;c.r8=layout;c.rsp=(uintptr_t)stack;b=c;
    win64_set_error(0x5a17);errno=2718;
    map_check(route?map_import(&c,"USER32.DLL",name):win64_window_call(&c,name),"recognized API");
    if(c.rax!=result)fprintf(stderr,"MAPVK_RESULT code=%" PRIx64 " type=%" PRIx64 " layout=%" PRIx64 " actual=%" PRIx64 " expected=%" PRIx64 "\n",code,type,layout,c.rax,result);
    map_check(c.rax==result,"full64 result");map_check(win64_get_error()==expected_error && errno==2718,"LastError/errno");
    c.rax=b.rax;if(route)b.rsp+=8;map_check(!memcmp(&c,&b,sizeof(c))&&!memcmp(stack,old_stack,sizeof(stack)),"fullCPU/single return/stack");++state->calls;
}
static void map_worker_body(map_state *state) {
    for(unsigned i=0;i<4096;++i){unsigned code=(i*73)&255;map_call(state,"MapVirtualKeyExA",code,4,0,i&1,map_native[code],0x5a17);}
}
#if defined(_WIN32)
static DWORD WINAPI map_worker(LPVOID value){map_worker_body(value);return 0;}
#else
static void *map_worker(void *value){map_worker_body(value);return NULL;}
#endif
int main(void) {
#if !defined(_WIN32)
    map_check(!window_regression_main(),"whole USER32/input/lifecycle regression");
#endif
    map_state state={0};
    for(unsigned i=0;i<65536;++i)map_call(&state,"MapVirtualKeyExA",i,4,0,i&1,i<256?map_native[i]:0,0x5a17);
    uint32_t random=UINT32_C(0x847359a1);
    for(unsigned i=0;i<8192;++i){random=random*UINT32_C(1664525)+UINT32_C(1013904223);map_call(&state,"MapVirtualKeyExA",random|UINT32_C(0x10000),4,0,i&1,0,0x5a17);}
    for(unsigned i=0;i<256;++i){map_call(&state,"MapVirtualKeyExA",UINT64_C(0xa587194500000000)|i,UINT64_C(0xb351824600000004),0,i&1,map_native[i],0x5a17);}
    const uint64_t layouts[]={1,UINT64_C(0x04090409),UINT64_MAX,UINT64_C(0x100000000)};
    for(unsigned i=0;i<16;++i)if(i!=4)map_call(&state,"MapVirtualKeyExA",0x41,i,0,i&1,0,50);
    for(unsigned i=0;i<sizeof(layouts)/sizeof(layouts[0]);++i)map_call(&state,"MapVirtualKeyExA",0x41,4,layouts[i],i&1,0,50);
    for(unsigned i=0;i<8;++i)map_call(&state,"MapVirtualKeyW",UINT64_MAX,i,0,i&1,0,50);
    CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(0x5a17);errno=2718;
    map_check(!map_import(&c,"SHELL32.DLL","MapVirtualKeyExA")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==0x5a17&&errno==2718,"USER32 namespace only");
    map_state workers[4]={{0}};
#if defined(_WIN32)
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,map_worker,&workers[i],0,NULL);map_check(threads[i]!=NULL,"thread create");}
    for(unsigned i=0;i<4;++i){map_check(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0,"thread join");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)map_check(!pthread_create(&threads[i],NULL,map_worker,&workers[i]),"thread create");
    for(unsigned i=0;i<4;++i)map_check(!pthread_join(threads[i],NULL),"thread join");
#endif
    unsigned parallel=0;for(unsigned i=0;i<4;++i)parallel+=workers[i].calls;
    printf("{\"success\":true,\"calls\":%u,\"parallel_calls\":%u,\"native_table_entries\":256,\"fullCPU\":true,\"LastError_errno\":true,\"single_return_pop\":true,\"MapVirtualKeyW_unavailable50\":true,\"virtual_US_type4_NULL_layout_only\":true,\"gameplay_proven\":false}\n",state.calls+parallel,parallel);
    return 0;
}
