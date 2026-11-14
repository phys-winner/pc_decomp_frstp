/* Real production USER32 queue/pump with injected platform events only. */
#define main window_regression_main
#include "window_fixture.c"
#undef main
#include <strings.h>
typedef struct {uint32_t size,flags;uint64_t hwnd;uint32_t hover,pad;} track_request;
static unsigned track_calls,track_cases,query_calls;
static struct {unsigned action;float x,y;uint64_t generation;} last_motion;
static int track_import(CPU *c,const char *dll,const char *name) {
    const int user=!strcasecmp(dll,"user32.dll");
#include "track_route.inc"
    return 0;
}
static void track_call(track_request *request,int route,uint64_t result,uint32_t expected_error) {
    uint64_t stack[8];memset(stack,0x7b,sizeof(stack));uint64_t old_stack[8];memcpy(old_stack,stack,sizeof(stack));
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)request;c.rsp=(uintptr_t)stack;before=c;
    win64_set_error(0x5a17);errno=2718;
    require(route?track_import(&c,"USER32.DLL","TrackMouseEvent"):win64_window_call(&c,"TrackMouseEvent"),"TrackMouseEvent route recognized");
    require(c.rax==result&&win64_get_error()==expected_error&&errno==2718,"TrackMouseEvent result/errors");
    c.rax=before.rax;if(route)before.rsp+=8;
    require(!memcmp(&c,&before,sizeof(c))&&!memcmp(stack,old_stack,sizeof(stack)),"TrackMouseEvent fullCPU/singlepop/stack unchanged");++track_calls;
}
static track_request request(uint32_t flags,uint64_t hwnd) {
    track_request value;memset(&value,0xa5,sizeof(value));value.size=24;value.flags=flags;value.hwnd=hwnd;return value;
}
static void query_at(uint64_t hwnd,int active,unsigned line) {
    unsigned sequence=++query_calls;
    track_request q=request(0x40000000,UINT64_MAX);win64_region_add((uintptr_t)&q,sizeof(q),1);
    track_call(&q,track_calls&1,1,0x5a17);
    int matches=q.size==24&&q.flags==(active?2u:0)&&q.hwnd==(active?hwnd:0)&&!q.hover&&!q.pad;
    if(!matches)fprintf(stderr,"TRACKMOUSE_QUERY_DETAIL sequence=%u line=%u tid=%ld track_calls=%u track_cases=%u checks=%u expected_active=%d expected_hwnd=0x%llx actual_size=%u actual_flags=0x%x actual_hwnd=0x%llx actual_hover=0x%x actual_pad=0x%x last_motion_action=%u last_motion_x=%.9g last_motion_y=%.9g last_motion_generation=%llu host_generation=%llu host_surface=%dx%d error=%u errno=%d\n",
        sequence,line,syscall(SYS_gettid),track_calls,track_cases,atomic_load(&checks),active,(unsigned long long)(active?hwnd:0),
        q.size,q.flags,(unsigned long long)q.hwnd,q.hover,q.pad,last_motion.action,(double)last_motion.x,(double)last_motion.y,
        (unsigned long long)last_motion.generation,(unsigned long long)host.generation,host.window.width,host.window.height,win64_get_error(),errno);
    require(matches,"query current-thread state/layout/padding");
    win64_region_remove((uintptr_t)&q);++track_cases;
}
#define query(hwnd,active) query_at((hwnd),(active),__LINE__)
static void register_leave(uint64_t hwnd,uint32_t flags) {
    track_request q=request(flags,hwnd),old=q;win64_region_add((uintptr_t)&q,sizeof(q),0);
    track_call(&q,track_calls&1,1,0x5a17);require(!memcmp(&q,&old,sizeof(q)),"LEAVE/CANCEL read-only request/unused hover/padding preserved");
    win64_region_remove((uintptr_t)&q);
}
static void motion(unsigned action,float x,float y,uint64_t generation) {
    last_motion.action=action;last_motion.x=x;last_motion.y=y;last_motion.generation=generation?generation:host.generation;
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=action,.source=AINPUT_SOURCE_MOUSE,
          .window_generation=generation,.width=2400,.height=1080,.x=x,.y=y});
}
static void leave_message(uint64_t hwnd,unsigned expected) {
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);
    unsigned count=0;while(peek(&msg,hwnd,0x2a3,0x2a3,1)){
        require(msg.hwnd==hwnd&&!msg.wp&&!msg.lp,"WM_MOUSELEAVE hwnd/zero payload");++count;
    }
    require(count==expected,"exactly one posted WM_MOUSELEAVE per registration");
    win64_region_remove((uintptr_t)&msg);++track_cases;
}
static void *foreign(void *opaque) {
    uint64_t hwnd=(uintptr_t)opaque;
    for(unsigned i=0;i<64;++i)query(0,0);
    track_request q=request(2,hwnd),old=q;win64_region_add((uintptr_t)&q,sizeof(q),0);
    track_call(&q,1,0,50);require(!memcmp(&q,&old,sizeof(q)),"foreign-window failure preserves request");
    win64_region_remove((uintptr_t)&q);return NULL;
}
int main(void) {
    require(!window_regression_main(),"existing whole USER32/input/lifecycle regression");
    static const uint16_t name[]={'O','w','n','T','r','a','c','k','F','i','x','t','u','r','e',0};
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};
    win64_region_add((uintptr_t)name,sizeof(name),0);win64_region_add((uintptr_t)&cls,sizeof(cls),0);
    uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));require(atom!=0,"register focused fixture class");
    win64_window_configure(&interface);host.available=1;event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    uint64_t hwnd=make_window(atom);require(hwnd!=0,"create focused real-platform-backed HWND");
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);drain(&msg);
    motion(AMOTION_EVENT_ACTION_HOVER_MOVE,1200,540,0);drain(&msg);query(0,0);
    register_leave(hwnd,2);query(hwnd,1);register_leave(hwnd,2);leave_message(hwnd,0);
    pthread_t thread;require(!pthread_create(&thread,NULL,foreign,(void*)(uintptr_t)hwnd)&&!pthread_join(thread,NULL),"foreign query/cross-thread rejection");query(hwnd,1);
    /* The existing coordinate contract rounds floor(x*scale+.5): -1*.4 -> 0. */
    motion(AMOTION_EVENT_ACTION_HOVER_MOVE,-1,540,0);
    int32_t point[2]={INT32_MIN,INT32_MIN};win64_region_add((uintptr_t)point,sizeof(point),1);
    require(api("GetCursorPos",1,V((uintptr_t)point))==1&&point[0]==0&&point[1]==270,"negative physical fraction rounds to inbounds virtual zero");
    win64_region_remove((uintptr_t)point);query(hwnd,1);leave_message(hwnd,0);
    for(unsigned i=0;i<128;++i){
        motion(AMOTION_EVENT_ACTION_HOVER_MOVE,1200,540,0);drain(&msg);register_leave(hwnd,2);query(hwnd,1);
        /* -3*.4 -> -1 after rounding, outside the integer guest client range. */
        motion(AMOTION_EVENT_ACTION_HOVER_MOVE,i&1?2400:-3,540,0);query(0,0);leave_message(hwnd,1);leave_message(hwnd,0);drain(&msg);
    }
    register_leave(hwnd,2);query(0,0);register_leave(hwnd,0x80000002);leave_message(hwnd,1);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    register_leave(hwnd,0x80000002);query(0,0);motion(AMOTION_EVENT_ACTION_HOVER_EXIT,1200,540,0);leave_message(hwnd,0);
    register_leave(hwnd,2);leave_message(hwnd,1);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=0});query(hwnd,1);leave_message(hwnd,0);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});drain(&msg);
    motion(AMOTION_EVENT_ACTION_HOVER_EXIT,1200,540,host.generation-1);query(hwnd,1);
    motion(AMOTION_EVENT_ACTION_HOVER_EXIT,1200,540,0);query(0,0);leave_message(hwnd,1);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    require(api("ShowWindow",2,hwnd,V(0))==1,"hide registered window");query(0,0);leave_message(hwnd,1);
    api("ShowWindow",2,hwnd,V(5));drain(&msg);register_leave(hwnd,2);
    api("ShowWindow",2,hwnd,V(2));query(0,0);leave_message(hwnd,1);api("ShowWindow",2,hwnd,V(9));drain(&msg);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    host.available=0;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});query(0,0);leave_message(hwnd,1);
    host.available=1;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});drain(&msg);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    motion(AMOTION_EVENT_ACTION_CANCEL,1200,540,0);query(0,0);leave_message(hwnd,1);
    for(unsigned i=0;i<6;++i){
        const uint32_t flags[]={1,0x10,3,0,0x80000000,0xc0000000};
        track_request q=request(flags[i],hwnd),old=q;win64_region_add((uintptr_t)&q,sizeof(q),0);
        track_call(&q,i&1,0,i<3?50:87);require(!memcmp(&q,&old,sizeof(q)),"unsupported profile leaves request unchanged");win64_region_remove((uintptr_t)&q);
    }
    track_call(NULL,0,0,87);
    uint32_t bad=23;win64_region_add((uintptr_t)&bad,4,0);track_call((void*)&bad,1,0,87);win64_region_remove((uintptr_t)&bad);
    track_request invalid=request(2,UINT64_MAX);win64_region_add((uintptr_t)&invalid,sizeof(invalid),0);track_call(&invalid,0,0,1400);win64_region_remove((uintptr_t)&invalid);
    motion(AMOTION_EVENT_ACTION_HOVER_ENTER,1200,540,0);drain(&msg);register_leave(hwnd,2);
    require(api("DestroyWindow",1,hwnd)==1,"destroy registered window");query(0,0);leave_message(0,0);
    require(api("UnregisterClassW",2,atom,V(0x140000000))==1,"unregister focused fixture class");win64_window_configure(NULL);
    printf("TRACKMOUSE_FIXTURE_PASS checks=%u calls=%u cases=%u rearm128=1 thread_queries64=1 negative_fraction_rounds_inside=1 original_SDL_flags2=1 query_cancel=1 outside_immediate=1 hover_exit=1 stale_generation=1 focus_not_leave=1 hide_minimize_surface_loss=1 lifecycle=1 whole_CPU_single_pop=1 injected_platform_only=1 game_boot_proven=0\n",checks,track_calls,track_cases);
    return 0;
}
