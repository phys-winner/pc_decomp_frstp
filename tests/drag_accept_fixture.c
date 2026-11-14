/* Real guest-window registration; injected host/window fixture is explicit. */
#define main window_regression_main
#include "window_fixture.c"
#undef main
#include "win64_shell.h"
#include <errno.h>
#include <strings.h>
#include "drag_route.inc"
#include "drag_native.h"
const char *win64_string_a(uint64_t address){for(unsigned n=0;n<32768;++n){if(address>UINT64_MAX-n)abort();if(!*(const char*)win64_pointer(address+n,1,0))return(const char*)(uintptr_t)address;}abort();}
static _Atomic unsigned drag_checks,drag_calls;
static void drag_check(int ok,const char*why){if(!ok){fprintf(stderr,"DRAG_ACCEPT_FIXTURE_FAIL %s\n",why);_Exit(1);}++drag_checks;}
static void request(uint64_t hwnd,uint64_t value,uint32_t initial,uint32_t expected,int routed){
    uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t saved_stack[8];memcpy(saved_stack,stack,sizeof(stack));
    CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=hwnd;c.rdx=value;c.rsp=(uintptr_t)stack;b=c;
    win64_set_error(initial);errno=2718;int handled=routed?drag_import(&c,"SHELL32.DLL","DragAcceptFiles"):win64_shell_call(&c,"DragAcceptFiles");
    drag_check(handled,"handled SHELL API");if(routed)b.rsp+=8;
    drag_check(!memcmp(&c,&b,sizeof(c))&&!memcmp(stack,saved_stack,sizeof(stack)),"void fullCPU/RAX/onepop/stack");
    drag_check(win64_get_error()==expected&&errno==2718,"native LastError/errno");++drag_calls;
}
typedef struct{uint64_t hwnd;uint32_t value;} work;
static void *toggle_worker(void*argument){work*w=argument;for(unsigned i=0;i<512;++i)request(w->hwnd,w->value,0x5a17,0x5a17,i&1);return NULL;}
int main(void){
    uint16_t name[]={'D','r','a','g','F','i','x','t','u','r','e',0};win64_region_add((uintptr_t)name,sizeof(name),0);
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};win64_region_add((uintptr_t)&cls,sizeof(cls),1);
    uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));drag_check(atom>=0xc000,"class registration");uint64_t hwnd=make_message(atom,0);drag_check(hwnd!=0,"real message-only guest window");
    /* Native STATIC creates WS_EX_WINDOWEDGE; seed that unrelated style bit
     * explicitly so the replay checks complete before/after style values. */
    api("SetWindowLongPtrW",3,hwnd,V(-20),V(drag_native_rows[0].before));
    for(unsigned i=0;i<sizeof(drag_native_rows)/sizeof(*drag_native_rows);++i){const drag_native_row*r=&drag_native_rows[i];
        uint64_t target=r->valid?hwnd:r->hwnd;if(r->valid)drag_check(api("GetWindowLongPtrW",2,hwnd,V(-20))==r->before,"native style before");
        request(target,r->value,r->initial,r->error,i&1);if(r->valid)drag_check(api("GetWindowLongPtrW",2,hwnd,V(-20))==r->after,"native WS_EX_ACCEPTFILES roundtrip");}
    pthread_t threads[4];work task={hwnd,1};for(unsigned phase=0;phase<2;++phase){task.value=phase?0:1;for(unsigned i=0;i<4;++i)drag_check(!pthread_create(&threads[i],NULL,toggle_worker,&task),"threadcreate");for(unsigned i=0;i<4;++i)drag_check(!pthread_join(threads[i],NULL),"threadjoin");drag_check(!!(api("GetWindowLongPtrW",2,hwnd,V(-20))&0x10)==!!task.value,"parallel final registration");}
    CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(17);errno=2718;
    drag_check(!drag_import(&c,"USER32.dll","DragAcceptFiles")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==17&&errno==2718,"not exposed through USER32");
    drag_check(!drag_import(&c,"kernel32.dll","DragAcceptFiles")&&!memcmp(&c,&b,sizeof(c)),"wrongDLL");drag_check(!win64_shell_call(&c,"DragAcceptFilesA")&&!memcmp(&c,&b,sizeof(c)),"unknownAPI unchanged");
    request(hwnd,1,17,17,1);drag_check(api("GetWindowLongPtrW",2,hwnd,V(-20))&0x10,"registeredbefore destroy");drag_check(api("DestroyWindow",1,hwnd),"destroy registeredwindow");request(hwnd,1,17,1400,1);
    uint64_t next=make_message(atom,0);drag_check(next&&next!=hwnd&&!api("GetWindowLongPtrW",2,next,V(-20)),"newwindow has no leaked registration");drag_check(api("DestroyWindow",1,next),"destroy newwindow");drag_check(api("UnregisterClassW",2,atom,V(0x140000000)),"unregister ownclass");win64_region_remove((uintptr_t)&cls);win64_region_remove((uintptr_t)name);
    drag_check(!window_regression_main(),"whole original window regression");
    printf("{\"success\":true,\"checks\":%u,\"calls\":%u,\"native_cases\":20,\"parallel_calls\":4096,\"void_CPU_RAX_single_pop\":true,\"per_window_registration\":true,\"destroy_lifetime\":true,\"SHELL32_only\":true,\"Android_file_drop_delivery\":false,\"game_boot_proven\":false}\n",drag_checks,drag_calls);return 0;
}
