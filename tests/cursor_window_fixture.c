/* Real canonical USER32 implementation plus the existing window regression. */
#include "../android/host/win64_window.c"
#define main old_window_matrix_main
#include "window_fixture.c"
#undef main
#include "cursor_vectors.h"
static atomic_uint cursor_calls,pointer_calls,warp_calls;
static int reject_pointer;
static uint64_t pointer_hwnd;
static int request_pointer(void *context,int visible){
    (void)context;require(!visible,"NULL guest cursor never invents an Android arrow");atomic_fetch_add(&pointer_calls,1);
    if(pointer_hwnd)require(api("IsWindow",1,pointer_hwnd)==1,"pointer callback can reenter USER32 outside mutex");
    win64_set_error(0x7788);errno=EILSEQ;return !reject_pointer;
}
static uint64_t cursor_checked(const char *name,uint64_t argument,uint32_t expected){
    CPU c,before;memset(&c,0xa7,sizeof(c));c.rcx=argument;c.rsp=UINT64_C(0x12345678);before=c;
    win64_set_error(0x5a17);errno=1927;require(win64_window_call(&c,name),"cursor API handled");uint64_t result=c.rax;
    before.rax=expected;require(!memcmp(&c,&before,sizeof(c)),"cursor complete CPU/EAX/no-pop preservation");
    require(win64_get_error()==0x5a17&&errno==1927,"cursor native errno/LastError preservation");atomic_fetch_add(&cursor_calls,1);return result;
}
static void native_cursor_rows(void){for(unsigned i=0;i<sizeof(cursor_vectors)/sizeof(cursor_vectors[0]);++i)cursor_checked("ShowCursor",UINT64_C(0xa5a5a5a500000000)|cursor_vectors[i].argument,cursor_vectors[i].result);}
static void *cursor_worker(void *unused){
    (void)unused;native_cursor_rows();for(unsigned i=0;i<1024;++i){cursor_checked("ShowCursor",0,UINT32_MAX);cursor_checked("ShowCursor",UINT64_C(0x1234567800000001),0);}return NULL;
}
static void warp_checked(uint64_t x,uint64_t y,uint32_t expected,uint32_t last_error){
    CPU c,before;memset(&c,0xb7,sizeof(c));c.rcx=x;c.rdx=y;c.rsp=UINT64_C(0x12345678);before=c;
    win64_set_error(0x5a17);errno=1927;require(win64_window_call(&c,"SetCursorPos"),"SetCursorPos handled");
    before.rax=expected;require(!memcmp(&c,&before,sizeof(c)),"warp complete CPU/low32 arguments/no-pop preservation");
    require(win64_get_error()==last_error&&errno==1927,"warp native errno/LastError contract");atomic_fetch_add(&warp_calls,1);
}
static void check_virtual_warp(uint64_t hwnd){
    message msg;struct{uint64_t before;int32_t xy[2];uint64_t after;}point={UINT64_C(0x1122334455667788),{0,0},UINT64_C(0x8877665544332211)};
    win64_region_add((uintptr_t)&msg,sizeof(msg),1);win64_region_add((uintptr_t)&point,sizeof(point),1);drain(&msg);
    unsigned callback_before=callback_count,pointer_before=atomic_load(&pointer_calls);
    const uint32_t positions[][2]={{480,270},{13,17},{0,0},{959,539},{480,270}};
    for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);++i){
        uint32_t before=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
        warp_checked(UINT64_C(0xa5a5a5a500000000)|positions[i][0],UINT64_C(0x5a5a5a5a00000000)|positions[i][1],1,0x5a17);
        uint32_t after=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
        require(peek(&msg,hwnd,WM_MOUSEMOVE,WM_MOUSEMOVE,1)&&msg.hwnd==hwnd&&!msg.wp,"warp queues own window mouse move");
        require(msg.lp==point_parameter((int32_t)positions[i][0],(int32_t)positions[i][1])&&msg.x==(int32_t)positions[i][0]&&msg.y==(int32_t)positions[i][1],"warp message coordinates and point snapshot");
        boot_bracket(msg.time,before,after,"warp message is BOOTTIME/GetTickCount domain");
        require(api("GetCursorPos",1,V((uintptr_t)point.xy))&&point.xy[0]==(int32_t)positions[i][0]&&point.xy[1]==(int32_t)positions[i][1],"warp changes real guest cursor readback");
        require(point.before==UINT64_C(0x1122334455667788)&&point.after==UINT64_C(0x8877665544332211),"cursor readback canaries");
        require(!peek(&msg,hwnd,WM_MOUSEMOVE,WM_MOUSEMOVE,1),"one queued mouse move per accepted warp");
    }
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});require(api("GetFocus",0)==hwnd,"warp modifier input focus");drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_SHIFT_LEFT});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_CTRL_LEFT});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=AMOTION_EVENT_BUTTON_PRIMARY|AMOTION_EVENT_BUTTON_SECONDARY,.x=0,.y=0});
    drain(&msg);warp_checked(7,9,1,0x5a17);
    require(peek(&msg,hwnd,WM_MOUSEMOVE,WM_MOUSEMOVE,1)&&msg.wp==15&&msg.lp==point_parameter(7,9),"warp keeps actual mouse buttons and shift/control modifiers");
    require(callback_count==callback_before&&atomic_load(&pointer_calls)==pointer_before,"warp only queues input; no WNDPROC/global pointer/image calls");
    win64_region_remove((uintptr_t)&point);win64_region_remove((uintptr_t)&msg);
}
static uint64_t cursor_setup(int capability){
    pthread_mutex_lock(&host.mutex);host.available=1;host.stopping=0;pthread_mutex_unlock(&host.mutex);
    static uint16_t name[]={'C','u','r','s','o','r','S','t','a','g','e',0};win64_region_add((uintptr_t)name,sizeof(name),0);
    interface.set_pointer_visible=request_pointer;interface.struct_size=capability?sizeof(interface):FP_ANDROID_HOST_BASE_SIZE;win64_window_configure(&interface);
    wc cls={.size=80,.proc=PROC1,.instance=0x140000000,.name=(uintptr_t)name};win64_region_add((uintptr_t)&cls,sizeof(cls),1);uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));win64_region_remove((uintptr_t)&cls);require(atom!=0,"cursor class");
    uint64_t hwnd=make_window(atom);require(hwnd!=0,"cursor real-window model");return hwnd;
}
int main(int argc,char **argv){
    if(argc==2){
        if(!strcmp(argv[1],"--overflow-high")||!strcmp(argv[1],"--overflow-low")){
            pthread_mutex_lock(&mutex);message_queue *q=queue_locked();q->cursor_count=!strcmp(argv[1],"--overflow-high")?INT32_MAX:INT32_MIN;pthread_mutex_unlock(&mutex);
            CPU c={.rcx=!strcmp(argv[1],"--overflow-high")};win64_window_call(&c,"ShowCursor");
        }else if(!strcmp(argv[1],"--missing-backend")){cursor_setup(0);CPU c={0};win64_window_call(&c,"ShowCursor");}
        else if(!strcmp(argv[1],"--backend-failure")){cursor_setup(1);reject_pointer=1;CPU c={0};win64_window_call(&c,"ShowCursor");}
        else if(!strcmp(argv[1],"--null-backend-failure")){cursor_setup(1);reject_pointer=1;CPU c={0};win64_window_call(&c,"SetCursor");}
        else if(!strcmp(argv[1],"--warp-negative")){cursor_setup(1);CPU c={.rcx=UINT64_C(0xa5a5a5a5ffffffff),.rdx=0};win64_window_call(&c,"SetCursorPos");}
        else if(!strcmp(argv[1],"--warp-outside")){cursor_setup(1);CPU c={.rcx=960,.rdx=540};win64_window_call(&c,"SetCursorPos");}
        else return 3;return 99;
    }
    require(argc==1,"cursor fixture arguments");int old=old_window_matrix_main();if(old)return old;
    native_cursor_rows();
    for(unsigned i=0;i<sizeof(cursor_raw_vectors)/sizeof(cursor_raw_vectors[0]);++i)cursor_checked("ShowCursor",cursor_raw_vectors[i].argument,cursor_raw_vectors[i].result);
    warp_checked(UINT64_MAX,UINT64_MAX,0,50);
    interface.struct_size=FP_ANDROID_HOST_BASE_SIZE;interface.set_pointer_visible=request_pointer;win64_window_configure(&interface);
    cursor_checked("ShowCursor",1,1);cursor_checked("ShowCursor",0,0);require(!atomic_load(&pointer_calls),"base ABI count-only requests have no nonexistent pointer target");win64_window_configure(NULL);
    uint64_t hwnd=cursor_setup(1);pointer_hwnd=hwnd;
    cursor_checked("ShowCursor",0,UINT32_MAX);unsigned pointer_before=atomic_load(&pointer_calls);require(pointer_before==1,"own window hides actual pointer");
    pthread_t workers[4];for(unsigned i=0;i<4;++i)require(!pthread_create(&workers[i],NULL,cursor_worker,NULL),"cursor independent thread");for(unsigned i=0;i<4;++i)require(!pthread_join(workers[i],NULL),"cursor independent thread join");
    require(atomic_load(&pointer_calls)==pointer_before,"other threads cannot change the owner's Android pointer");
    cursor_checked("ShowCursor",1,0);require(atomic_load(&pointer_calls)==2,"nonnegative display count still has NULL cursor hidden");
    cursor_checked("SetCursor",0,0);require(atomic_load(&pointer_calls)==3,"SetCursor NULL applies real hidden request");
    check_virtual_warp(hwnd);
    pointer_hwnd=0;require(api("DestroyWindow",1,hwnd)==1,"cursor window destruction");win64_window_configure(NULL);
    cursor_checked("ShowCursor",0,UINT32_MAX);cursor_checked("ShowCursor",1,0);require(atomic_load(&pointer_calls)==3,"no active HWND requires no host pointer request");
    warp_checked(UINT64_MAX,UINT64_MAX,0,50);
    printf("CURSOR_WINDOW_PASS native_counter_rows=%u raw_RAX_rows=%u checked_calls=%u parallel_pairs=4096 parallel_calls=8192 other_thread_native_rows=64 pointer_requests=%u virtual_warp_calls=%u warp_readback=1 warp_queued_input=1 warp_boottime=1 warp_modifiers=1 initial_count=0 low32=1 eax_zeroextend=1 fullCPU=1 errors=1 per_thread=1 NULL_image_hidden=1 real_backend_mock=1 base_ABI=1 actual_Activity_execution=0\n",(unsigned)(sizeof(cursor_vectors)/sizeof(cursor_vectors[0])),(unsigned)(sizeof(cursor_raw_vectors)/sizeof(cursor_raw_vectors[0])),atomic_load(&cursor_calls),atomic_load(&pointer_calls),atomic_load(&warp_calls));return 0;
}
