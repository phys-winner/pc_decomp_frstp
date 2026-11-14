/* Injected platform fixture only: these objects are not production HWNDs or
 * Android surfaces. Production links its actual ANativeWindow implementation. */
#define _GNU_SOURCE
#include "win64_window.h"
#include "win64.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "window_scans.h"
#include "window_title_returns.h"
#define PROC1 UINT64_C(0x140001000)
#define PROC2 UINT64_C(0x140002000)
typedef struct {uint32_t size,style;uint64_t proc;int32_t class_extra,window_extra;uint64_t instance,icon,cursor,brush,menu,name,small_icon;} wc;
typedef struct {uint64_t hwnd;uint32_t message,pad;uint64_t wp,lp;uint32_t time;int32_t x,y;uint32_t reserved;} message;
typedef struct {uint64_t parameter,instance,menu,parent;int32_t height,width,y,x;uint32_t style,pad;uint64_t title,class_name;uint32_t ex_style,pad2;} creation;
typedef struct {uint64_t address;size_t bytes;int writable;} region;
static region regions[2048];static unsigned region_count;static pthread_mutex_t registry=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t error;
static _Atomic unsigned checks;static unsigned callback_count;static uint32_t callback_messages[256];static int reject_nc,reject_create,creating_message;static int32_t message_dimension;
static uint64_t create_title,title_return;static int title_creation_probe,title_creation_length,title_override,title_callback_error,title_last_null;
static int title_destroy_expected=-1;static unsigned title_set_calls;static uint64_t worker_title_hwnd,worker_atom;
struct ANativeWindow {int width,height,references;};
static struct {pthread_mutex_t mutex;pthread_cond_t changed;struct ANativeWindow window;int available,stopping;uint64_t generation,wake_sequence,consumed_wake_sequence;unsigned count;FrostpunkAndroidEvent events[256];} host={.mutex=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER,.window={2400,1080,1},.generation=1};
static void require(int value,const char *reason){if(!value){fprintf(stderr,"WINDOW_FIXTURE_FAIL %s error=%u\n",reason,error);exit(1);}++checks;}
/* This isolated window harness has no Activity lifecycle; clocks are tested separately. */
uint64_t win64_game_clock_ns(clockid_t clock){struct timespec now;require(!clock_gettime(clock,&now),"fixture clock");return (uint64_t)now.tv_sec*1000000000+(uint64_t)now.tv_nsec;}
void win64_set_error(uint32_t value){error=value;}uint32_t win64_get_error(void){return error;}
void win64_region_add(uint64_t address,size_t bytes,int writable){pthread_mutex_lock(&registry);if(region_count==2048)abort();regions[region_count++]=(region){address,bytes,writable};pthread_mutex_unlock(&registry);}
void win64_region_remove(uint64_t address){pthread_mutex_lock(&registry);for(unsigned i=0;i<region_count;++i)if(regions[i].address==address){regions[i]=regions[--region_count];pthread_mutex_unlock(&registry);return;}abort();}
void *win64_pointer(uint64_t address,size_t bytes,int writable){pthread_mutex_lock(&registry);for(unsigned i=0;i<region_count;++i)if(address>=regions[i].address&&address-regions[i].address<=regions[i].bytes&&bytes<=regions[i].bytes-(address-regions[i].address)&&(!writable||regions[i].writable)){pthread_mutex_unlock(&registry);return(void*)(uintptr_t)address;}fprintf(stderr,"WINDOW_FIXTURE_BADPTR address=%llx bytes=%zu write=%d\n",(unsigned long long)address,bytes,writable);abort();}
uint64_t win64_alloc(size_t bytes,int zero){void *p=zero?calloc(1,bytes):malloc(bytes);if(!p)abort();win64_region_add((uintptr_t)p,bytes,1);return(uintptr_t)p;}
void win64_free(uint64_t address){win64_region_remove(address);free((void*)(uintptr_t)address);}
uint64_t win64_argument(CPU *cpu,unsigned index){if(index<4){uint64_t r[]={cpu->rcx,cpu->rdx,cpu->r8,cpu->r9};return r[index];}uint64_t value;memcpy(&value,win64_pointer(cpu->rsp+40+(index-4)*8,8,0),8);return value;}
static uint64_t api(const char *name,unsigned count,...) {
    _Alignas(16) uint64_t stack[512]={0};uint64_t args[12]={0};va_list ap;va_start(ap,count);for(unsigned i=0;i<count;++i)args[i]=va_arg(ap,uint64_t);va_end(ap);
    win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU cpu={0};cpu.rsp=(uintptr_t)&stack[401];cpu.rcx=args[0];cpu.rdx=args[1];cpu.r8=args[2];cpu.r9=args[3];cpu.rbx=0xabcdef;cpu.cf=1;
    for(unsigned i=4;i<count;++i)stack[406+i-4]=args[i];uint64_t before=cpu.rsp;
    require(win64_window_call(&cpu,name),"known API handler");require(cpu.rsp==before&&cpu.rbx==0xabcdef&&cpu.cf==1,"caller CPU/RSP preserved");
    win64_region_remove((uintptr_t)stack);return cpu.rax;
}
#define V(x) ((uint64_t)(x))
static uint64_t nested(CPU *cpu,const char *name,uint64_t a,uint64_t b,uint64_t c) {
    CPU call=*cpu;call.rcx=a;call.rdx=b;call.r8=c;require(win64_window_call(&call,name),"reentrant handler");require(call.rsp==cpu->rsp,"reentrant RSP");return call.rax;
}
static uint64_t nested4(CPU *cpu,const char *name,uint64_t a,uint64_t b,uint64_t c,uint64_t d){CPU call=*cpu;call.rcx=a;call.rdx=b;call.r8=c;call.r9=d;require(win64_window_call(&call,name)&&call.rsp==cpu->rsp,"reentrant title API/RSP");return call.rax;}
void dispatch(CPU *cpu,uint64_t target) {
    require(target==PROC1||target==PROC2,"static callback target");require((cpu->rsp&15)==8,"Win64 callback stack alignment");
    require(*(uint64_t*)win64_pointer(cpu->rsp,8,0)==UINT64_C(0xe5ed0000),"callback return sentinel");
    uint64_t *shadow=win64_pointer(cpu->rsp+8,32,1);shadow[0]=0x11223344;shadow[3]=0x55667788;
    uint64_t hwnd=cpu->rcx;unsigned msg=(unsigned)cpu->rdx;
    if(callback_count==256)abort();callback_messages[callback_count++]=msg;
    if(msg==0x81 || msg==1) {
        creation *create=win64_pointer(cpu->r9,80,0);require(create->parameter==0xaabbccdd && create->width==(creating_message?message_dimension:960) && create->height==(creating_message?message_dimension:540) && create->instance==0x140000000,"CREATESTRUCT80 arguments");
        if(creating_message)require(create->parent && create->parent!=UINT64_MAX-2 && !create->style && !create->ex_style,"queue-backed message-only CREATESTRUCT parent/raw style");
        if(msg==0x81){require(nested(cpu,"SetWindowLongPtrW",hwnd,V(-21),create->parameter)==0,"NCCREATE reentrant userdata");
            if(title_creation_probe){require(create->title==create_title&&!nested4(cpu,"DefWindowProcW",hwnd,V(14),0,0),"initial title empty before DefProc NCCREATE");require(nested4(cpu,"DefWindowProcW",hwnd,V(msg),cpu->r8,cpu->r9)==1,"DefProc NCCREATE accepted");require(nested4(cpu,"DefWindowProcW",hwnd,V(14),0,0)==(uint64_t)title_creation_length,"initial title copied by DefProc NCCREATE");}
            cpu->rax=reject_nc?0:1;}
        else cpu->rax=reject_create?UINT64_MAX:0;
    } else if(msg==12){++title_set_calls;title_last_null=cpu->r9==0;if(title_callback_error){win64_set_error(1234);errno=2345;cpu->rax=0;}else cpu->rax=title_override?title_return:nested4(cpu,"DefWindowProcW",hwnd,V(msg),cpu->r8,cpu->r9);}
    else if(msg==2 || msg==0x82) {if(msg==2)require(nested(cpu,"GetWindowLongPtrW",hwnd,V(-21),0)==0xaabbccdd,"userdata alive during destroy");if(title_destroy_expected>=0)require(nested4(cpu,"DefWindowProcW",hwnd,V(14),0,0)==(uint64_t)title_destroy_expected,"owned title alive through DESTROY/NCDESTROY callbacks");cpu->rax=0;}
    else cpu->rax=target==PROC2?UINT64_C(0xf123456789abcdef):cpu->r8^cpu->r9;
    cpu->rsp+=8;
}
void dispatch_jmp(CPU *cpu,uint64_t target){dispatch(cpu,target);}
static ANativeWindow *acquire(void *context,uint64_t *generation){(void)context;pthread_mutex_lock(&host.mutex);if(generation)*generation=host.generation;ANativeWindow *w=host.available?&host.window:NULL;if(w)++host.window.references;pthread_mutex_unlock(&host.mutex);return w;}
static void release(ANativeWindow *window){pthread_mutex_lock(&host.mutex);require(window==&host.window&&host.window.references>1,"balanced surface release");--host.window.references;pthread_mutex_unlock(&host.mutex);}
int32_t ANativeWindow_getWidth(ANativeWindow *w){return w->width;}int32_t ANativeWindow_getHeight(ANativeWindow *w){return w->height;}
static int stopped(void *context){(void)context;pthread_mutex_lock(&host.mutex);int value=host.stopping;pthread_mutex_unlock(&host.mutex);return value;}
static void wake(void *context){(void)context;pthread_mutex_lock(&host.mutex);++host.wake_sequence;pthread_cond_broadcast(&host.changed);pthread_mutex_unlock(&host.mutex);}
static int poll(void *context,FrostpunkAndroidEvent *event,int timeout){(void)context;pthread_mutex_lock(&host.mutex);
    if(timeout<0){while(!host.count&&!host.stopping&&host.wake_sequence==host.consumed_wake_sequence)pthread_cond_wait(&host.changed,&host.mutex);}
    else if(timeout&&!host.count&&!host.stopping&&host.wake_sequence==host.consumed_wake_sequence){struct timespec end;clock_gettime(CLOCK_REALTIME,&end);end.tv_sec+=timeout/1000;end.tv_nsec+=(long)(timeout%1000)*1000000;if(end.tv_nsec>=1000000000){++end.tv_sec;end.tv_nsec-=1000000000;}pthread_cond_timedwait(&host.changed,&host.mutex,&end);}
    int result=host.stopping?-1:0;host.consumed_wake_sequence=host.wake_sequence;if(host.count){*event=host.events[0];--host.count;memmove(host.events,host.events+1,host.count*sizeof(*event));result=1;}pthread_mutex_unlock(&host.mutex);return result;
}
static int wait_host(void *context,int timeout){FrostpunkAndroidEvent event;(void)context;(void)timeout;(void)event;return 0;}
static FrostpunkAndroidHost interface={.abi_version=1,.struct_size=sizeof(FrostpunkAndroidHost),.acquire_window=acquire,.release_window=release,.poll_event=poll,.wait=wait_host,.wake=wake,.is_stopping=stopped};
static void event(FrostpunkAndroidEvent e){pthread_mutex_lock(&host.mutex);require(host.count<256,"platform fixture queue capacity");if(!e.window_generation)e.window_generation=host.generation;host.events[host.count++]=e;pthread_cond_broadcast(&host.changed);pthread_mutex_unlock(&host.mutex);}
static uint64_t make_window(uint64_t atom){return api("CreateWindowExW",12,V(0),atom,create_title,V(0x90000000u),V(0),V(0),V(960),V(540),V(0),V(0),V(0x140000000),V(0xaabbccdd));}
static uint64_t make_message(uint64_t atom,int use_default){creating_message=1;message_dimension=use_default?INT32_MIN:0;uint64_t hwnd=api("CreateWindowExW",12,V(0),atom,create_title,V(0),V(message_dimension),V(message_dimension),V(message_dimension),V(message_dimension),V(-3),V(0),V(0x140000000),V(0xaabbccdd));creating_message=0;return hwnd;}
static int peek(message *msg,uint64_t hwnd,unsigned low,unsigned high,unsigned remove){return(int)api("PeekMessageW",5,V((uintptr_t)msg),hwnd,V(low),V(high),V(remove));}
static void drain(message *msg){while(peek(msg,0,0,0,1))require(msg->message!=0x12,"no unexpected quit");}
static unsigned registered_regions(void){pthread_mutex_lock(&registry);unsigned count=region_count;pthread_mutex_unlock(&registry);return count;}
static void title_readback(uint64_t hwnd,const uint16_t *expected,size_t units,unsigned capacity){
    size_t bytes=16+(size_t)(capacity?capacity:1)*2;uint64_t allocation=win64_alloc(bytes,0);unsigned char *memory=win64_pointer(allocation,bytes,1);memset(memory,0xa5,bytes);
    size_t copied=capacity?(units<capacity-1?units:capacity-1):0;win64_set_error(0x5a17);errno=1927;
    require(api("DefWindowProcW",4,hwnd,V(14),V(0),V(0))==units&&win64_get_error()==0x5a17&&errno==1927,"DefProc GETTEXTLENGTH UTF16/error preservation");
    require(api("DefWindowProcW",4,hwnd,V(13),V(capacity),capacity?allocation+8:UINT64_MAX)==copied,"DefProc GETTEXT unit count/truncation");
    require(win64_get_error()==0x5a17&&errno==1927,"DefProc GETTEXT error preservation");
    for(unsigned i=0;i<8;++i)require(memory[i]==0xa5&&memory[bytes-8+i]==0xa5,"title output canaries");
    if(capacity){require(!copied||!memcmp(memory+8,expected,copied*2),"title opaque UTF16 active units");uint16_t zero;memcpy(&zero,memory+8+copied*2,2);require(!zero,"title output termination");for(size_t i=8+(copied+1)*2;i<bytes-8;++i)require(memory[i]==0xa5,"title output tail untouched");}
    win64_free(allocation);
}
static void set_copied_title(uint64_t hwnd,const uint16_t *text,size_t units){
    uint64_t source=win64_alloc((units+1)*2,0);memcpy(win64_pointer(source,(units+1)*2,1),text,(units+1)*2);win64_set_error(0x5a17);errno=1927;unsigned before=title_set_calls;
    require(api("SetWindowTextW",2,hwnd,source)==1&&title_set_calls==before+1&&!title_last_null&&win64_get_error()==0x5a17&&errno==1927,"SetWindowTextW synchronous DefProc/error preservation");
    memset(win64_pointer(source,(units+1)*2,1),0xcc,(units+1)*2);win64_free(source);title_readback(hwnd,text,units,(unsigned)units+1);
}
static void check_window_title(uint64_t hwnd){
    static const uint16_t normal[]={'H','e','l','l','o',0x416,0xd83d,0xde42,0},embedded[]={'A',0,'B',0},high[]={0xd800,0},low[]={0xdc00,0},empty[]={0};
    set_copied_title(hwnd,normal,8);const unsigned capacities[]={0,1,2,7,8,9,32};for(unsigned i=0;i<sizeof(capacities)/sizeof(capacities[0]);++i)title_readback(hwnd,normal,8,capacities[i]);
    uint16_t ignored[]={'N','o',0};win64_region_add((uintptr_t)ignored,sizeof(ignored),0);
    title_override=1;
    for(unsigned i=0;i<sizeof(window_title_returns)/sizeof(window_title_returns[0]);++i){title_return=window_title_returns[i].lresult;unsigned before=title_set_calls;win64_set_error(0x5a17);errno=1927;require(api("SetWindowTextW",2,hwnd,V((uintptr_t)ignored))==window_title_returns[i].result&&title_set_calls==before+1&&win64_get_error()==window_title_returns[i].error&&errno==window_title_returns[i].native_errno,"native full64 LRESULT normalization independent of title mutation");title_readback(hwnd,normal,8,9);}
    title_override=0;title_callback_error=1;require(api("SetWindowTextW",2,hwnd,V((uintptr_t)ignored))==1&&win64_get_error()==1234&&errno==2345,"custom WNDPROC LastError/native errno preserved");title_callback_error=0;win64_region_remove((uintptr_t)ignored);
    unsigned before=title_set_calls;require(api("SetWindowTextW",2,hwnd,V(0))==1&&title_set_calls==before+1&&title_last_null,"NULL title passed unchanged and clears in DefProc");title_readback(hwnd,empty,0,2);
    win64_region_add((uintptr_t)embedded,sizeof(embedded),0);require(api("SetWindowTextW",2,hwnd,V((uintptr_t)embedded))==1,"embedded-NUL title accepted");title_readback(hwnd,embedded,1,4);require(embedded[2]=='B',"embedded-NUL input tail unchanged");win64_region_remove((uintptr_t)embedded);
    set_copied_title(hwnd,high,1);set_copied_title(hwnd,low,1);set_copied_title(hwnd,empty,0);
    size_t units=32767;uint16_t *long_title=malloc((units+1)*2);require(long_title!=NULL,"long title fixture allocation");for(size_t i=0;i<units;++i)long_title[i]=(uint16_t)(0x410+i%32);long_title[units]=0;set_copied_title(hwnd,long_title,units);free(long_title);
    require(api("SetWindowTextW",2,hwnd,V(0))==1,"clear long title");title_readback(hwnd,empty,0,1);
    before=title_set_calls;win64_set_error(0x5a17);require(api("SetWindowTextW",2,V(0),UINT64_MAX)==1&&win64_get_error()==1400&&title_set_calls==before,"invalid NULL HWND native BOOL1 before input read");
    win64_set_error(0x5a17);require(api("SetWindowTextW",2,V(1),UINT64_MAX)==1&&win64_get_error()==1400&&title_set_calls==before,"invalid foreign HWND native BOOL1 before input read");
}
static uint64_t clock_ns(clockid_t clock){struct timespec value;require(!clock_gettime(clock,&value),"fixture clock read");return(uint64_t)value.tv_sec*UINT64_C(1000000000)+(uint64_t)value.tv_nsec;}
static void boot_bracket(uint32_t value,uint32_t before,uint32_t after,const char *reason){require((uint32_t)(value-before)<=(uint32_t)(after-before),reason);}
static void check_message_clock(uint64_t hwnd,message *msg){
    uint32_t before=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    require(api("PostMessageW",4,hwnd,V(0x8061),V(0x1234),V(0x5678))&&peek(msg,hwnd,0x8061,0x8061,1),"clock-domain posted message");
    uint32_t after=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    boot_bracket(msg->time,before,after,"PostMessage MSG.time is GetTickCount BOOTTIME domain");
    before=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    uint64_t old_ns=clock_ns(CLOCK_MONOTONIC)-UINT64_C(500000000);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A,.time_ns=old_ns});
    require(peek(msg,hwnd,0x100,0x100,1)&&msg->wp=='A',"clock-domain old key event");
    after=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    /* The before/after interval includes all async processing and scheduling;
     * two milliseconds only cover separate clock-floor quantization. */
    boot_bracket(msg->time,before-502,after-498,"key MONOTONIC age preserved in BOOTTIME domain");
    uint32_t key_time=msg->time;
    require(api("TranslateMessage",1,V((uintptr_t)msg))&&peek(msg,hwnd,0x102,0x102,1)&&msg->time==key_time,"translated character preserves converted event timestamp");
    before=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_A});
    require(peek(msg,hwnd,0x101,0x101,1),"clock-domain zero-time key event");
    after=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    boot_bracket(msg->time,before,after,"zero input timestamp falls back to BOOTTIME now");
    before=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    old_ns=clock_ns(CLOCK_MONOTONIC)-UINT64_C(500000000);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.time_ns=old_ns,.width=2400,.height=1080,.x=600,.y=270});
    require(peek(msg,hwnd,0x200,0x200,1),"clock-domain old motion event");
    after=(uint32_t)(clock_ns(CLOCK_BOOTTIME)/1000000);
    boot_bracket(msg->time,before-502,after-498,"motion MONOTONIC age preserved in BOOTTIME domain");
    drain(msg);
    printf("WINDOW_CLOCK_DOMAIN_PASS post_boottime=1 key_motion_age_ms=500 zero_fallback=1 translated_time=1 dword_brackets=1\n");
}
static pthread_mutex_t thread_mutex=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t thread_changed=PTHREAD_COND_INITIALIZER;static uint32_t worker_tid;static uint64_t worker_hwnd;
static void *worker(void *unused){(void)unused;message msg={0};win64_region_add((uintptr_t)&msg,sizeof(msg),1);peek(&msg,0,0,0,0);
    uint8_t scans[256];require(win64_window_input_snapshot(worker_hwnd,scans)==1,"cross-thread physical keyboard snapshot");
    require(api("SendMessageW",4,worker_hwnd,V(0x8033),V(1),V(2))==0&&win64_get_error()==50,"cross-thread SendMessage rejected");
    unsigned title_before=title_set_calls;require(api("SetWindowTextW",2,worker_hwnd,UINT64_MAX)==0&&win64_get_error()==50&&title_set_calls==title_before,"cross-thread title update explicitly unsupported before source read");
    worker_title_hwnd=make_message(worker_atom,0);require(worker_title_hwnd!=0,"worker owns a message-only title lifecycle");static const uint16_t queued[]={'Q','u','e','u','e',0};set_copied_title(worker_title_hwnd,queued,5);
    pthread_mutex_lock(&thread_mutex);worker_tid=(uint32_t)syscall(SYS_gettid);pthread_cond_broadcast(&thread_changed);pthread_mutex_unlock(&thread_mutex);
    require(api("GetMessageA",4,V((uintptr_t)&msg),UINT64_MAX,V(0x8010),V(0x8010))==1&&msg.message==0x8010&&msg.wp==0x1234,"blocking cross-thread post wake/filter");
    win64_region_remove((uintptr_t)&msg);return NULL;
}
int main(void) {
    win64_window_self_test();require(make_window(0xc000)==0&&win64_get_error()==50,"headless CreateWindow fails honestly");
    uint16_t name[]={'F','r','o','s','t','p','u','n','k','F','i','x','t','u','r','e',0};win64_region_add((uintptr_t)name,sizeof(name),0);
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};win64_region_add((uintptr_t)&cls,sizeof(cls),1);
    uint64_t atom=api("RegisterClassExW",1,V((uintptr_t)&cls));require(atom>=0xc000,"class atom");
    require(api("RegisterClassExW",1,V((uintptr_t)&cls))==0&&win64_get_error()==1410,"duplicate class");
    cls.size=80;require(api("GetClassInfoExW",3,V(0x140000000),atom,V((uintptr_t)&cls))==1&&cls.proc==PROC1&&cls.window_extra==16,"class info80");
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);
    static const uint16_t initial[]={'I','n','i','t','i','a','l',0x416,0};create_title=win64_alloc(sizeof(initial),0);memcpy(win64_pointer(create_title,sizeof(initial),1),initial,sizeof(initial));title_creation_probe=1;title_creation_length=8;
    uint64_t message_hwnd=make_message(atom,1);require(message_hwnd&&api("IsWindow",1,message_hwnd)&&!api("GetParent",1,message_hwnd)&&!api("IsWindowVisible",1,message_hwnd)&&!api("IsIconic",1,message_hwnd)&&api("GetWindowLongPtrW",2,message_hwnd,V(-16))==0x04c00000&&!win64_window_acquire(message_hwnd,NULL),"headless message-only HWND identity/style/parent/no-surface");
    title_creation_probe=0;win64_free(create_title);create_title=0;title_readback(message_hwnd,initial,8,9);
    for(unsigned failure=0;failure<2;++failure){create_title=win64_alloc(sizeof(initial),0);memcpy(win64_pointer(create_title,sizeof(initial),1),initial,sizeof(initial));title_creation_probe=1;reject_nc=!failure;reject_create=!!failure;unsigned saved_regions=registered_regions();require(!make_message(atom,0)&&registered_regions()==saved_regions,"initial title NC/CREATE failure releases guest callback structure");reject_nc=reject_create=title_creation_probe=0;win64_free(create_title);create_title=0;}
    require(!peek(&msg,0,0,0,1),"message-only creation does not invent surface/focus/size messages");
    require(api("PostMessageW",4,message_hwnd,V(0x8051),V(0x123),V(0x456))&&!peek(&msg,UINT64_MAX,0,0,1),"message-only post remains a window message");
    require(peek(&msg,message_hwnd,0x8051,0x8051,1)&&msg.hwnd==message_hwnd&&api("DispatchMessageW",1,V((uintptr_t)&msg))==(0x123^0x456),"headless message-only queue/dispatch");
    win64_window_configure(&interface);require(make_window(atom)==0&&win64_get_error()==21,"missing real host surface fails");
    host.available=1;event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    reject_nc=1;unsigned before=callback_count;require(!make_window(atom),"NCCREATE rejection");require(callback_count==before+2&&callback_messages[before]==0x81&&callback_messages[before+1]==0x82&&host.window.references==1,"NCCREATE rejection cleanup/order");reject_nc=0;
    reject_create=1;before=callback_count;require(!make_window(atom),"CREATE rejection");require(callback_count==before+4&&callback_messages[before]==0x81&&callback_messages[before+1]==1&&callback_messages[before+2]==2&&callback_messages[before+3]==0x82&&host.window.references==1,"CREATE failure destroy order");reject_create=0;
    before=callback_count;uint64_t hwnd=make_window(atom);require(hwnd&&callback_messages[before]==0x81&&callback_messages[before+1]==1&&host.window.references==2,"real host HWND create/refcount");
    require(api("UnregisterClassW",2,atom,V(0x140000000))==0&&win64_get_error()==1412,"live window prevents unregister");
    drain(&msg);
    uint64_t generation=0;ANativeWindow *native=win64_window_acquire(hwnd,&generation);require(native&&generation==1&&host.window.references==3,"DXVK acquire real retained surface");win64_window_release(native);
    int32_t rect[4];win64_region_add((uintptr_t)rect,sizeof(rect),1);require(api("GetClientRect",2,hwnd,V((uintptr_t)rect))==1&&rect[2]==960&&rect[3]==540,"actual native extent");
    int32_t old_rect[4];memcpy(old_rect,rect,sizeof(rect));require(!api("GetClientRect",2,message_hwnd,V((uintptr_t)rect))&&win64_get_error()==50&&!memcmp(old_rect,rect,sizeof(rect)),"message-only desktop geometry explicitly unsupported and output untouched");
    uint64_t second_message=make_message(atom,0);require(second_message&&second_message!=message_hwnd&&api("IsWindow",1,hwnd)&&host.window.references==2&&!win64_window_acquire(second_message,NULL),"message-only helpers coexist with one real top-level surface");require(api("DestroyWindow",1,second_message)&&host.window.references==2,"message-only destroy never releases real surface");
    reject_nc=1;require(!make_message(atom,0)&&api("IsWindow",1,hwnd)&&host.window.references==2,"message-only creation failure preserves unrelated topwindow");reject_nc=0;
    require(api("GetWindowLongPtrW",2,hwnd,V(-21))==0xaabbccdd,"persistent userdata");
    require(api("SetWindowLongPtrW",3,hwnd,V(0),UINT64_C(0xf123456789abcdef))==0&&api("GetWindowLongPtrW",2,hwnd,V(0))==UINT64_C(0xf123456789abcdef),"window extra bounds/64-bit data");
    require(api("SetWindowLongPtrW",3,hwnd,V(-4),PROC2)==PROC1,"subclass old procedure");
    require(api("SendMessageW",4,hwnd,V(0x8044),V(1),V(2))==UINT64_C(0xf123456789abcdef),"subclass64-bit result");
    require(api("CallWindowProcW",5,PROC1,hwnd,V(0x8044),V(0x1234),V(0x5678))==(0x1234^0x5678),"CallWindowProc old target/fifth arg");
    api("SetWindowLongPtrW",3,hwnd,V(-4),PROC1);
    api("PostMessageW",4,hwnd,V(0x8001),V(1),V(2));api("PostThreadMessageA",4,V(syscall(SYS_gettid)),V(0x8002),V(3),V(4));
    require(peek(&msg,UINT64_MAX,0,0,0)&&!msg.hwnd&&msg.message==0x8002,"thread-only filter");message same=msg;
    require(peek(&msg,UINT64_MAX,0,0,0)&&!memcmp(&same,&msg,sizeof(msg)),"PM_NOREMOVE identity");require(peek(&msg,UINT64_MAX,0,0,1)&&msg.message==0x8002,"thread remove");
    require(peek(&msg,hwnd,0x8001,0x8001,1)&&msg.message==0x8001&&msg.wp==1,"HWND+range filter");
    api("PostQuitMessage",1,V(37));api("PostMessageW",4,hwnd,V(0x8003),V(5),V(6));
    require(peek(&msg,0,0,0,1)&&msg.message==0x8003,"posted messages before low-priority quit");
    require(api("GetMessageW",4,V((uintptr_t)&msg),hwnd,V(0x100),V(0x109))==0&&msg.message==0x12&&msg.wp==37,"WM_QUIT bypasses HWND/range filters");
    require(api("GetMessageW",4,V((uintptr_t)&msg),V(0x1234),V(0),V(0))==UINT32_MAX&&win64_get_error()==1400,"invalid HWND GetMessage -1");
    require(api("PostMessageW",4,hwnd,V(0x12),V(91),V(92))&&api("PostMessageW",4,hwnd,V(0x8004),V(0),V(0)),"directly posted quit and later message");
    require(peek(&msg,0,0,0,1)&&msg.message==0x12&&msg.hwnd==hwnd&&msg.wp==91&&msg.lp==92,"direct WM_QUIT retains ordinary FIFO/payload");
    require(peek(&msg,0,0,0,1)&&msg.message==0x8004,"message after direct quit");
    require(api("PostMessageW",4,hwnd,V(0x12),V(93),V(94))&&!peek(&msg,hwnd,0x8090,0x8090,1),"direct posted quit obeys range filter");
    require(peek(&msg,0,0,0,1)&&msg.message==0x12&&msg.wp==93,"direct quit remains after unmatched filter");
    require(api("ShowWindow",2,hwnd,V(6))==1&&api("IsWindowVisible",1,hwnd)==1&&api("IsIconic",1,hwnd)==1,"logical minimize remains visible style");
    native=win64_window_acquire(hwnd,NULL);require(native&&host.window.references==3,"minimized valid HWND can create a swapchain");win64_window_release(native);drain(&msg);
    require(api("ShowWindow",2,hwnd,V(9))==1&&!api("IsIconic",1,hwnd)&&api("GetFocus",0)==hwnd,"logical restore follows actual host focus");drain(&msg);
    require(api("ShowWindow",2,hwnd,V(0))==1&&!api("IsWindowVisible",1,hwnd),"hide keeps a valid HWND");
    native=win64_window_acquire(hwnd,NULL);require(native&&host.window.references==3,"hidden valid HWND can create a swapchain before ShowWindow");win64_window_release(native);drain(&msg);
    require(!api("ShowWindow",2,hwnd,V(5))&&api("IsWindowVisible",1,hwnd),"show hidden window again");drain(&msg);
    check_message_clock(hwnd,&msg);
    check_window_title(hwnd);
    const uint32_t ime_units[]={0x0416,0x65e5,0xd83d,0xde00};
    for(unsigned i=0;i<sizeof(ime_units)/sizeof(*ime_units);++i){
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_TEXT,.keycode=ime_units[i]});
        require(peek(&msg,0,0,0,1)&&msg.message==0x102&&msg.wp==ime_units[i]&&msg.lp==1,"IME Unicode reaches the original WM_CHAR editor without keyboard layout translation");
    }
    for(unsigned i=0;i<sizeof(window_key_scans)/sizeof(window_key_scans[0]);++i){
        unsigned key=window_key_scans[i].android_key,vk=window_key_scans[i].vk,scan=window_key_scans[i].scan;
        int alt=vk==0x12,system=alt||vk==0x79;uint64_t common=1|((uint64_t)(scan&255)<<16)|((scan&0xff00)?UINT64_C(1)<<24:0);
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=key,.scancode=0xdead,.meta=alt?AMETA_ALT_ON:0});
        struct {uint64_t before;uint8_t keys[256];uint64_t after;} input={.before=UINT64_C(0x123456789abcdef0),.after=UINT64_C(0xfedcba9876543210)};
        uint32_t saved_error=win64_get_error();
        require(win64_window_input_snapshot(hwnd,input.keys)==1,"focused physical keyboard snapshot");
        for(unsigned j=0;j<256;++j)require(input.keys[j]==(j==((scan&127)|((scan&0xff00)?128:0))?0x80:0),"native US scan maps to exact DIK high-bit state");
        require(input.before==UINT64_C(0x123456789abcdef0)&&input.after==UINT64_C(0xfedcba9876543210)&&win64_get_error()==saved_error,"keyboard snapshot canaries and LastError");
        event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=key,.scancode=0xdead});
        require(win64_window_input_snapshot(hwnd,input.keys)==1,"released physical keyboard snapshot");
        for(unsigned j=0;j<256;++j)require(!input.keys[j],"physical keyboard key-up clears state");
        int found=peek(&msg,0,0,0,1);
        if(!found || msg.message!=(system?0x104u:0x100u) || msg.wp!=vk || msg.lp!=(common|(alt?UINT64_C(1)<<29:0)))fprintf(stderr,"KEYSCAN_FAIL android=%u vk=%u scan=%x found=%d msg=%x wp=%llx lp=%llx expected=%llx\n",key,vk,scan,found,msg.message,(unsigned long long)msg.wp,(unsigned long long)msg.lp,(unsigned long long)(common|(alt?UINT64_C(1)<<29:0)));
        require(found&&msg.message==(system?0x104u:0x100u)&&msg.wp==vk&&msg.lp==(common|(alt?UINT64_C(1)<<29:0)),"native US scan/extended keydown oracle");
        require(peek(&msg,0,0,0,1)&&msg.message==(system?0x105u:0x101u)&&msg.wp==vk&&msg.lp==(common|UINT64_C(3)<<30),"native US scan/key-up transitions");
        if(vk==0x14 || vk==0x90 || vk==0x91){event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=key});event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=key});drain(&msg);}
    }
    uint8_t physical_keys[256],untouched_keys[256];memset(physical_keys,0xa5,sizeof(physical_keys));memcpy(untouched_keys,physical_keys,sizeof(physical_keys));
    require(win64_window_input_snapshot(UINT64_MAX,physical_keys)==-1&&!memcmp(physical_keys,untouched_keys,256),"invalid snapshot HWND preserves output");
    require(win64_window_input_snapshot(message_hwnd,physical_keys)==-1&&!memcmp(physical_keys,untouched_keys,256),"message-only snapshot has no physical keyboard");
    require(win64_window_input_snapshot(hwnd,NULL)==-1,"NULL native snapshot output rejected");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_CTRL_RIGHT});
    require(win64_window_input_snapshot(hwnd,physical_keys)==1&&physical_keys[0x9d]==0x80,"right control retains extended DIK identity");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=0});
    require(win64_window_input_snapshot(hwnd,physical_keys)==0,"focus loss reports unacquired physical keyboard");
    for(unsigned j=0;j<256;++j)require(!physical_keys[j],"focus loss clears held physical keys");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_CTRL_RIGHT});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    require(win64_window_input_snapshot(hwnd,physical_keys)==1&&!physical_keys[0x9d],"focus regain does not resurrect ignored release");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_CTRL_RIGHT});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,physical_keys)==1&&physical_keys[0x1e]==0x80,"held key before minimize");
    require(api("ShowWindow",2,hwnd,V(2))==1&&win64_window_input_snapshot(hwnd,physical_keys)==0&&!physical_keys[0x1e],"minimize clears physical keyboard");
    require(api("ShowWindow",2,hwnd,V(9))==1&&win64_window_input_snapshot(hwnd,physical_keys)==1&&!physical_keys[0x1e],"restore does not resurrect held key");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_A});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A,.repeat=7});
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp=='A'&&(msg.lp&0xffff)==1&&(msg.lp&(UINT64_C(1)<<30)),"Android cumulative repeat converts to one uncoalesced Win32 repeat");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_A});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_DOWN,.source=AINPUT_SOURCE_TOUCHSCREEN,.pointer_id=7,.width=2400,.height=1080,.x=1200,.y=720});
    require(peek(&msg,0,0,0,1)&&msg.message==0x200&&(int16_t)msg.lp==480&&(int16_t)(msg.lp>>16)==360,"physical view to client coordinate scaling");
    require(peek(&msg,0,0,0,1)&&msg.message==0x201&&msg.wp==1,"touch produces primary mouse down");
    require(api("GetKeyState",1,V(1))==0x8000&&api("GetAsyncKeyState",1,V(1))==0x8001,"mouse queue/async button state");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_UP,.source=AINPUT_SOURCE_TOUCHSCREEN,.pointer_id=7,.width=2400,.height=1080,.x=1200,.y=720});
    require(peek(&msg,0,0,0,1)&&msg.message==0x202,"touch up");require(api("GetKeyState",1,V(1))==0,"mouse up queue state");
    uint64_t before_down=win64_window_ui_begin();
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=1,.repeat=41,.x=120,.y=120});
    require(!frostpunk_android_touch_ready(41),"queued Down cannot acknowledge a tap before delivery");
    require(peek(&msg,hwnd,0x201,0x201,1)&&msg.message==0x201,"synthetic Down is delivered");
    win64_window_ui_end(before_down);require(!frostpunk_android_touch_ready(41),"a UI pass started before Down cannot acknowledge it");
    uint64_t after_down=win64_window_ui_begin();win64_window_ui_end(after_down);
    require(frostpunk_android_touch_ready(41)&&!frostpunk_android_touch_ready(42),"only the delivered Down ticket is acknowledged by a subsequent original UI pass");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.x=120,.y=120});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=1,.x=120,.y=120});
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.buttons=0,.x=120,.y=120});
    usleep(250000); /* A five-FPS consumer must still receive both edges of the short tap. */
    require(peek(&msg,hwnd,0x201,0x202,1)&&msg.message==0x201&&api("GetKeyState",1,V(1))==0x8000,"short tap down survives slower-than-tap message consumption");
    require(peek(&msg,hwnd,0x201,0x202,1)&&msg.message==0x202&&api("GetKeyState",1,V(1))==0,"short tap up remains ordered after its down");drain(&msg);
    const unsigned keycodes[]={AKEYCODE_SHIFT_LEFT,AKEYCODE_A,AKEYCODE_A,AKEYCODE_SHIFT_LEFT};
    for(unsigned i=0;i<4;++i)event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=i>=2?AKEY_EVENT_ACTION_UP:AKEY_EVENT_ACTION_DOWN,.keycode=keycodes[i],.meta=i==3?0:AMETA_SHIFT_ON,.scancode=i==0||i==3?42:30});
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp==0x10,"shift down");require(api("TranslateMessage",1,V((uintptr_t)&msg))==1,"non-character key TranslateMessage true");
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp=='A',"letter key down");
    require(api("GetKeyState",1,V('A'))==0x8000&&api("GetAsyncKeyState",1,V('A'))==1,"message state differs from current physical released state");
    same=msg;require(api("TranslateMessage",1,V((uintptr_t)&msg))==1&&!memcmp(&same,&msg,sizeof(msg)),"TranslateMessage input MSG unchanged");
    require(peek(&msg,0,0,0,1)&&msg.message==0x102&&msg.wp=='A',"translated CHAR prioritized before queued key up");
    require(api("DispatchMessageA",1,V((uintptr_t)&msg))==('A'^msg.lp),"Unicode window receives ASCII A-dispatch");
    drain(&msg);require(!api("GetKeyState",1,V('A'))&&!api("GetKeyState",1,V(0x10)),"released key queue state");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    require(win64_window_input_snapshot(hwnd,physical_keys)==1&&physical_keys[0x1e]==0x80,"held key before surface loss");
    host.available=0;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});require(api("GetClientRect",2,hwnd,V((uintptr_t)rect))&&rect[2]==960&&rect[3]==540&&!win64_window_acquire(hwnd,NULL)&&host.window.references==1,"surface loss preserves logical HWND extent but releases native ref");
    require(win64_window_input_snapshot(hwnd,physical_keys)==0&&!physical_keys[0x1e],"surface loss clears physical keyboard");drain(&msg);
    host.available=1;host.window.width=1280;host.window.height=720;++host.generation;event((FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW});
    require(api("GetClientRect",2,hwnd,V((uintptr_t)rect))&&rect[2]==960&&rect[3]==540&&host.window.references==2,"physical resize preserves requested logical extent/ref acquisition");drain(&msg);
    require(win64_window_input_snapshot(hwnd,physical_keys)==1&&!physical_keys[0x1e],"new surface does not resurrect held key");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_UP,.keycode=AKEYCODE_A});drain(&msg);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=AMOTION_EVENT_ACTION_MOVE,.source=AINPUT_SOURCE_MOUSE,.x=640,.y=360});
    require(peek(&msg,0,0,0,1)&&msg.message==0x200&&msg.x==480&&msg.y==270,"resized physical extent scales to unchanged logical client");
    worker_hwnd=hwnd;worker_atom=atom;pthread_t thread;require(!pthread_create(&thread,NULL,worker,NULL),"create native worker");
    pthread_mutex_lock(&thread_mutex);while(!worker_tid)pthread_cond_wait(&thread_changed,&thread_mutex);uint32_t tid=worker_tid;pthread_mutex_unlock(&thread_mutex);
    require(api("PostThreadMessageW",4,V(tid),V(0x8010),V(0x1234),V(0x5678))==1,"post another thread's owned queue");require(!pthread_join(thread,NULL),"blocking GetMessage worker returned");
    unsigned set_before=title_set_calls;require(api("SetWindowTextW",2,worker_title_hwnd,UINT64_MAX)==1&&win64_get_error()==1400&&title_set_calls==set_before,"thread queue cleanup retires its titled message-only window");
    static const uint16_t final_title[]={'D','t','o','r',0};set_copied_title(hwnd,final_title,4);title_destroy_expected=4;
    before=callback_count;require(api("DestroyWindow",1,hwnd)==1&&callback_messages[before]==2&&callback_messages[before+1]==0x82&&host.window.references==1,"destroy order and native reference release");
    set_before=title_set_calls;require(api("SetWindowTextW",2,hwnd,UINT64_MAX)==1&&win64_get_error()==1400&&title_set_calls==set_before,"destroyed HWND title native BOOL1/error1400 without callback");
    require(!api("IsWindow",1,hwnd)&&!api("GetWindowLongPtrW",2,hwnd,V(-21))&&win64_get_error()==1400,"destroyed HWND identity rejected");
    require(api("IsWindow",1,message_hwnd)&&api("UnregisterClassW",2,atom,V(0x140000000))==0&&win64_get_error()==1412,"message-only HWND retains class after surface window destroyed");
    title_destroy_expected=8;require(api("DestroyWindow",1,message_hwnd)&&!api("IsWindow",1,message_hwnd)&&host.window.references==1,"message-only lifecycle independent of Android surface");title_destroy_expected=-1;
    require(api("UnregisterClassW",2,atom,V(0x140000000))==1,"unregister after all windows destroyed");win64_window_configure(NULL);
    printf("WINDOW_TITLE_FIXTURE_PASS native_return_widths=7 deepcopy=1 utf16=1 max_units=32767 callbacks_errors=1 invalid_bool1=1 lifecycle=1\n");
    printf("WINDOW_FIXTURE_PASS checks=%u callbacks=%u ABI72_80_48_80=1 static_shadow=1 reentrant=1 message_only=1 filters_quit_priority=1 input_coordinates=1 keyboard_CHAR=1 lifecycle_refs=1 threaded_post=1 injected_host_only=1 game_boot_proven=0\n",checks,callback_count);
    return 0;
}
