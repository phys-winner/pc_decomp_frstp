/* Reuse the whole host/input/lifecycle fixture; add property probes to its
 * real static guest WNDPROC dispatch without replacing production code. */
#define main window_regression_main
#define dispatch property_base_dispatch
#include "window_fixture.c"
#undef dispatch
#undef main
#include <errno.h>
#include "property_native.h"
#include "property_route.inc"
static const uint16_t property_keys[][15]={
 {'S','D','L','_','W','i','n','d','o','w','D','a','t','a',0},
 {'s','d','l','_','w','i','n','d','o','w','d','a','t','a',0},
 {'S','D','L','_','W','I','N','D','O','W','D','A','T','A',0},
 {'S','D','L','_','W','i','n','d','o','w','D','a','t',0,0},
 {'S','D','L','_','W','i','n','d','o','w','D','a','t','a','x'}};
static _Atomic unsigned property_checks,property_calls;
static int property_hook;static uint64_t property_hwnd,property_live;
static unsigned properties_nccreate,properties_destroy,properties_ncdestroy;
static void property_check(int value,const char *why){if(!value){fprintf(stderr,"PROPERTY_FIXTURE_FAIL %s\n",why);_Exit(1);}++property_checks;}
static uint64_t property_call(const char *name,uint64_t a,uint64_t b,uint64_t d,int route,uint64_t expected,uint32_t initial,uint32_t expected_error){
 uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t old_stack[8];memcpy(old_stack,stack,sizeof(stack));CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=a;c.rdx=b;c.r8=d;c.rsp=(uintptr_t)stack;before=c;
 win64_set_error(initial);errno=2718;int handled=route?property_import(&c,"USER32.DLL",name):win64_window_call(&c,name);
 property_check(handled&&c.rax==expected,"result/full64-bit value");property_check(win64_get_error()==expected_error&&errno==2718,"LastError/host errno");c.rax=before.rax;if(route)before.rsp+=8;property_check(!memcmp(&c,&before,sizeof(c))&&!memcmp(stack,old_stack,sizeof(stack)),"fullCPU/RSP/stack");++property_calls;return expected;
}
void dispatch(CPU *cpu,uint64_t target){
 if(property_hook && target==PROC1){
  uint64_t key=(uintptr_t)property_keys[0],hwnd=cpu->rcx;
  if(cpu->rdx==0x81){++properties_nccreate;property_hwnd=hwnd;property_check(!nested(cpu,"GetPropW",hwnd,key,0),"initial NCCREATE missing property");property_check(nested(cpu,"SetPropW",hwnd,key,property_live)==1,"NCCREATE sets independent property");}
  else if(cpu->rdx==1 || cpu->rdx==2 || cpu->rdx==0x82){property_check(nested(cpu,"GetPropW",hwnd,key,0)==property_live,"property alive through CREATE/DESTROY/NCDESTROY");if(cpu->rdx==2)++properties_destroy;if(cpu->rdx==0x82)++properties_ncdestroy;}
 }
 property_base_dispatch(cpu,target);
}
static int property_value(uint64_t value){return !value||(value>=UINT64_C(0xf123456789abcde0)&&value<=UINT64_C(0xf123456789abcde3));}
static void *property_worker(void *opaque){unsigned id=(unsigned)(uintptr_t)opaque;uint64_t key=(uintptr_t)property_keys[id%3],value=UINT64_C(0xf123456789abcde0)+id;
 for(unsigned i=0;i<512;++i){property_call("SetPropW",property_hwnd,key,value,i&1,1,0x5a17,0x5a17);
  CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=property_hwnd;c.rdx=key;b=c;win64_set_error(0x5a17);errno=2718;property_check(win64_window_call(&c,"GetPropW")&&property_value(c.rax),"concurrent value never torn");c.rax=b.rax;property_check(!memcmp(&c,&b,sizeof(c))&&errno==2718&&win64_get_error()==0x5a17,"concurrent get CPU/error");++property_calls;
  c=b;property_check(win64_window_call(&c,"RemovePropW")&&property_value(c.rax),"concurrent atomic remove");c.rax=b.rax;property_check(!memcmp(&c,&b,sizeof(c))&&errno==2718&&win64_get_error()==0x5a17,"concurrent remove CPU/error");++property_calls;}
 return NULL;
}
static uint64_t cleanup_atom,cleanup_hwnd;
static void *property_owner_exit(void *unused){(void)unused;cleanup_hwnd=make_message(cleanup_atom,0);property_check(cleanup_hwnd!=0,"owner-exit window created");property_call("SetPropW",cleanup_hwnd,(uintptr_t)property_keys[0],UINT64_C(0xfedcba9876543210),0,1,0x5a17,0x5a17);return NULL;}
static void drag_check(uint64_t hwnd,uint64_t accept,uint32_t expected_error){CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=hwnd;c.rdx=accept;b=c;win64_set_error(0x5a17);errno=2718;win64_window_drag_accept_files(&c);property_check(!memcmp(&c,&b,sizeof(c))&&win64_get_error()==expected_error&&errno==2718,"void DragAcceptFiles CPU/RSP/error");++property_calls;}
int main(void){
 property_check(!window_regression_main(),"whole existing USER32/input lifecycle regression");
 win64_region_add((uintptr_t)property_keys,sizeof(property_keys),0);
 uint16_t name[]={'P','r','o','p','e','r','t','y','F','i','x','t','u','r','e',0};win64_region_add((uintptr_t)name,sizeof(name),0);
 wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};win64_region_add((uintptr_t)&cls,sizeof(cls),1);cleanup_atom=api("RegisterClassExW",1,V((uintptr_t)&cls));property_check(cleanup_atom>=0xc000,"property class");
 uint64_t hwnd=make_message(cleanup_atom,0);property_check(hwnd!=0,"native-free registered HWND");
 for(unsigned i=0;i<sizeof(property_native)/sizeof(property_native[0]);++i){const property_native_row *r=&property_native[i];uint64_t key=r->key<0?0:(uintptr_t)property_keys[r->key];property_call(r->name,r->valid?hwnd:UINT64_MAX,key,r->value,i&1,r->result,r->initial,r->error);}
 for(unsigned i=3;i<5;++i)for(unsigned op=0;op<3;++op){const char *apis[]={"GetPropW","SetPropW","RemovePropW"};property_call(apis[op],hwnd,(uintptr_t)property_keys[i],0x123,op&1,0,0x5a17,50);}
 uint64_t isolated=make_message(cleanup_atom,0);property_check(isolated&&isolated!=hwnd,"independent property HWND");property_call("GetPropW",isolated,(uintptr_t)property_keys[0],0,0,0,0x5a17,0x5a17);property_call("SetPropW",isolated,(uintptr_t)property_keys[0],UINT64_MAX,0,1,0x5a17,0x5a17);property_call("GetPropW",hwnd,(uintptr_t)property_keys[0],0,0,0x12345678,0x5a17,0x5a17);property_check(api("DestroyWindow",1,isolated)==1,"isolated property lifecycle");
 property_call("SetPropW",hwnd,1,0x123,0,0,0x5a17,50);
 property_call("GetPropW",UINT64_MAX,UINT64_MAX,0,0,0,0x5a17,1400);
 property_call("GetMenu",hwnd,UINT64_MAX,0,1,0,0x5a17,0x5a17);property_call("GetMenu",0,0,0,0,0,0x5a17,1400);
 property_call("SetPropW",hwnd,(uintptr_t)property_keys[0],0,1,1,0x5a17,0x5a17);property_call("GetPropW",hwnd,(uintptr_t)property_keys[0],0,1,0,0x5a17,0x5a17);
 property_check(api("GetWindowLongPtrW",2,hwnd,V(-21))==0xaabbccdd,"property never aliases USERDATA");
 api("SetWindowLongPtrW",3,hwnd,V(-20),V(0xa0));
 const uint64_t bools[]={1,2,UINT64_MAX,UINT64_C(0x80000000),UINT64_C(0x100000000),0};
 for(unsigned i=0;i<sizeof(bools)/sizeof(bools[0]);++i){drag_check(hwnd,bools[i],0x5a17);property_check(api("GetWindowLongPtrW",2,hwnd,V(-20))==(0xa0|((uint32_t)bools[i]?0x10:0)),"DragAcceptFiles only real exstyle bit10");}
 drag_check(0,1,1400);drag_check(UINT64_MAX,0,1400);
 CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(0x5a17);errno=2718;property_check(!win64_window_call(&c,"DragAcceptFiles")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==0x5a17&&errno==2718,"Shell delegate unavailable through generic USER32 name");
 property_check(!property_import(&c,"SHELL32.dll","SetPropW")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==0x5a17,"property USER32-only gate");
 uint32_t keymap[512];memcpy(keymap,property_default_keymap,sizeof(keymap));
 for(unsigned i=0;i<sizeof(property_eligible_scans)/sizeof(property_eligible_scans[0]);++i){unsigned scan=property_eligible_scans[i],index=property_scan_to_SDL[scan];uint64_t vk=property_call("MapVirtualKeyW",scan,1,0,i&1,0,0x5a17,50);if(vk)keymap[index]=(uint32_t)property_call("MapVirtualKeyW",vk,2,0,0,0,0x5a17,50);}
 property_check(!memcmp(keymap,property_default_keymap,sizeof(keymap))&&keymap[4]=='a'&&keymap[44]==' ',"original SDL default512 keymap remains intact");
 for(unsigned type=0;type<8;++type)property_call("MapVirtualKeyW",UINT64_MAX,type,0,type&1,0,0x5a17,50);
 property_call("MapVirtualKeyW",0x20,0,0,1,0,0x5a17,50);
 property_call("GetClipboardSequenceNumber",UINT64_MAX,UINT64_MAX,UINT64_MAX,0,0,0x5a17,50);property_call("GetClipboardSequenceNumber",UINT64_MAX,UINT64_MAX,UINT64_MAX,1,0,0x5a17,50);
 property_call("GetMessageExtraInfo",UINT64_MAX,UINT64_MAX,UINT64_MAX,0,0,0x5a17,0x5a17);property_call("GetMessageExtraInfo",UINT64_MAX,UINT64_MAX,UINT64_MAX,1,0,0x5a17,0x5a17);
 property_call("SetWindowPos",hwnd,UINT64_MAX,UINT64_MAX,1,0,0x5a17,50);property_call("SetWindowPos",0,UINT64_MAX,UINT64_MAX,0,0,0x5a17,1400);
 property_call("ClipCursor",UINT64_MAX,UINT64_MAX,UINT64_MAX,1,0,0x5a17,50);property_call("ClipCursor",0,UINT64_MAX,UINT64_MAX,0,0,0x5a17,50);
 property_call("RegisterDeviceNotificationW",hwnd,UINT64_MAX,0,1,0,0x5a17,50);
 property_call("RegisterDeviceNotificationW",UINT64_MAX,UINT64_MAX,UINT64_MAX,0,0,0x5a17,50);
 property_call("MapVirtualKeyExA",0x20,4,0,1,0x39,0x5a17,0x5a17);
 property_call("MapVirtualKeyExA",0x20,4,UINT64_MAX,0,0,0x5a17,50);
 c=b;property_check(!win64_window_call(&c,"RegisterDeviceNotificationA")&&!memcmp(&c,&b,sizeof(c)),"unrequested notification encoding unhandled");
 property_hwnd=hwnd;pthread_t threads[4];for(unsigned i=0;i<4;++i)property_check(!pthread_create(&threads[i],NULL,property_worker,(void*)(uintptr_t)i),"property worker create");for(unsigned i=0;i<4;++i)property_check(!pthread_join(threads[i],NULL),"property worker join");
 property_check(api("DestroyWindow",1,hwnd)==1,"close native-free property window");property_call("GetPropW",hwnd,(uintptr_t)property_keys[0],0,0,0,0x5a17,1400);
 property_live=win64_alloc(8,1);*(uint64_t*)(uintptr_t)property_live=UINT64_C(0x123456789abcdef0);property_hook=1;
 reject_nc=1;property_check(!make_message(cleanup_atom,0)&&properties_nccreate==1&&properties_ncdestroy==1,"NCCREATE rejection property visible through final callback");reject_nc=0;property_call("GetPropW",property_hwnd,(uintptr_t)property_keys[0],0,0,0,0x5a17,1400);
 reject_create=1;property_check(!make_message(cleanup_atom,0)&&properties_destroy==1&&properties_ncdestroy==2,"CREATE rejection property cleanup");reject_create=0;
 hwnd=make_message(cleanup_atom,0);property_check(hwnd&&property_hwnd==hwnd,"callback property window");property_check(api("DestroyWindow",1,hwnd)==1&&properties_destroy==2&&properties_ncdestroy==3,"property visible through normal WM_NCDESTROY");property_call("GetPropW",hwnd,(uintptr_t)property_keys[0],0,1,0,0x5a17,1400);property_hook=0;
 property_check(*(uint64_t*)win64_pointer(property_live,8,0)==UINT64_C(0x123456789abcdef0),"opaque property never owns/frees guest data");win64_free(property_live);
 property_check(!pthread_create(&threads[0],NULL,property_owner_exit,NULL)&&!pthread_join(threads[0],NULL),"owner queue cleanup");property_call("GetPropW",cleanup_hwnd,(uintptr_t)property_keys[0],0,1,0,0x5a17,1400);property_check(!api("IsWindow",1,cleanup_hwnd),"thread queue cleanup invalidates property HWND");
 win64_window_configure(&interface);hwnd=make_window(cleanup_atom);property_check(hwnd&&host.window.references==2,"real-host window for unavailable positioning");message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);drain(&msg);
 int32_t rect[4];win64_region_add((uintptr_t)rect,sizeof(rect),1);property_check(api("GetClientRect",2,hwnd,V((uintptr_t)rect))&&rect[0]==0&&rect[1]==0&&rect[2]==960&&rect[3]==540,"initial virtual client geometry");
 for(unsigned i=0;i<2;++i){property_check(!api("SetWindowPos",7,hwnd,V(-2),V(100),V(200),V(320),V(240),V(i?0x114:0x110))&&win64_get_error()==50,"actual SDL SetWindowPos flags fail honestly");property_check(api("GetClientRect",2,hwnd,V((uintptr_t)rect))&&rect[0]==0&&rect[1]==0&&rect[2]==960&&rect[3]==540&&!peek(&msg,0,0,0,1),"no invented geometry or WINDOWPOS event");}
 property_call("ClipCursor",(uintptr_t)rect,UINT64_MAX,UINT64_MAX,1,0,0x5a17,50);property_check(rect[0]==0&&rect[1]==0&&rect[2]==960&&rect[3]==540&&!peek(&msg,0,0,0,1),"unavailable confinement does not touch RECT/queue");
 property_check(api("DestroyWindow",1,hwnd)==1&&host.window.references==1,"real-host positioning fixture release");win64_region_remove((uintptr_t)rect);win64_region_remove((uintptr_t)&msg);win64_window_configure(NULL);
 property_check(api("UnregisterClassW",2,cleanup_atom,V(0x140000000))==1,"all class refs released");
 printf("{\"success\":true,\"checks\":%u,\"calls\":%u,\"native_property_cases\":%zu,\"parallel_calls\":6144,\"full_CPU_RSP_errno_LastError\":true,\"reentrant_lifetime\":true,\"owner_exit_cleanup\":true,\"MapVirtualKeyW_unavailable50\":true,\"clipboard_unavailable50\":true,\"message_extra_info_default0\":true,\"SDL_default512_unchanged\":true,\"DragAcceptFiles_exstyle10\":true,\"USER32_only_properties\":true,\"game_boot_proven\":false}\n",property_checks,property_calls,sizeof(property_native)/sizeof(property_native[0]));
 return 0;
}
