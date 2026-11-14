/* The existing real-window fixture remains unchanged. These new APIs create
 * no cursor handles, dispatch no callbacks and consume no Android input. */
#define main window_regression_main
#include "window_fixture.c"
#undef main
#include <errno.h>
#include "cursor_native.h"
#include "cursor_route.inc"
static _Atomic unsigned cursor_checks,cursor_calls;
static void cursor_check(int value,const char *why){if(!value){fprintf(stderr,"CURSOR_FIXTURE_FAIL %s\n",why);_Exit(1);}++cursor_checks;}
static uint64_t cursor_call(const char *name,uint64_t a,uint64_t b,int route,uint64_t expected,uint32_t expected_error){
 uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t old_stack[8];memcpy(old_stack,stack,sizeof(stack));CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=a;c.rdx=b;c.rsp=(uintptr_t)stack;before=c;
 win64_set_error(0x5a17);errno=2718;int handled=route?cursor_import(&c,"USER32.DLL",name):win64_window_call(&c,name);
 cursor_check(handled&&c.rax==expected,"result/zeroextension");cursor_check(win64_get_error()==expected_error&&errno==2718,"LastError/native errno");c.rax=before.rax;if(route)before.rsp+=8;cursor_check(!memcmp(&c,&before,sizeof(c))&&!memcmp(stack,old_stack,sizeof(stack)),"fullCPU/RSP/stack");++cursor_calls;return expected;
}
static void cursor_profile(unsigned routed){
 cursor_call("GetDoubleClickTime",UINT64_MAX,UINT64_MAX,routed,500,0x5a17);
 cursor_call("LoadCursorW",0,0x7f00,routed,0,50);
 cursor_call("SetCursor",0,UINT64_MAX,routed,0,0x5a17);
 cursor_call("DestroyIcon",0,UINT64_MAX,routed,0,CURSOR_NATIVE_NULL_ERROR);
 cursor_call("DestroyCursor",0,UINT64_MAX,routed,0,CURSOR_NATIVE_NULL_ERROR);
}
static void *cursor_worker(void *unused){(void)unused;for(unsigned i=0;i<512;++i)cursor_profile(i&1);return NULL;}
int main(void){
 for(unsigned route=0;route<2;++route){cursor_profile(route);cursor_call("SetCursor",1,0,route,0,50);cursor_call("DestroyIcon",UINT64_MAX,0,route,0,50);cursor_call("DestroyCursor",0x7f00,0,route,0,50);cursor_call("LoadCursorW",UINT64_MAX,1,route,0,50);}
 /* Mirror only the proven SDL ownership shape: allocation remains non-NULL
  * when its optional HCURSOR is unavailable; cleanup still frees the object. */
 uint64_t object=win64_alloc(16,1);cursor_check(object!=0,"genuine allocated SDL-shaped cursor object");((uint64_t*)(uintptr_t)object)[1]=cursor_call("LoadCursorW",0,0x7f00,1,0,50);cursor_check(object&&!((uint64_t*)(uintptr_t)object)[1],"NULL HCURSOR does not discard SDL object");cursor_call("SetCursor",((uint64_t*)(uintptr_t)object)[1],0,1,0,0x5a17);cursor_call("DestroyIcon",((uint64_t*)(uintptr_t)object)[1],0,1,0,CURSOR_NATIVE_NULL_ERROR);win64_free(object);
 CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(0x5a17);errno=2718;cursor_check(!cursor_import(&c,"kernel32.dll","GetDoubleClickTime")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==0x5a17&&errno==2718,"USER32-only gate");cursor_check(!win64_window_call(&c,"LoadCursorA")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==0x5a17,"unknown API unchanged");
 pthread_t threads[4];for(unsigned i=0;i<4;++i)cursor_check(!pthread_create(&threads[i],NULL,cursor_worker,NULL),"worker create");for(unsigned i=0;i<4;++i)cursor_check(!pthread_join(threads[i],NULL),"worker join");
 cursor_check(!window_regression_main(),"existing whole window/input/lifecycle regression");
 printf("{\"success\":true,\"checks\":%u,\"calls\":%u,\"parallel_calls\":10240,\"full_CPU_RSP_errno_LastError\":true,\"virtual_doubleclick500\":true,\"cursor_unavailable50\":true,\"SetCursor_NULL_valid\":true,\"NULL_destroy_native1402\":true,\"existing_window_regression\":true,\"game_boot_proven\":false}\n",cursor_checks,cursor_calls);return 0;
}
