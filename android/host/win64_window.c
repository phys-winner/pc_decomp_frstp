/* One real Android surface, Win64 USER32 records and native input queues.
 * Original callbacks are always static C dispatch, with a guest shadow frame. */
#define _GNU_SOURCE
#include "win64_window.h"
#include "win64_time.h"
#include "win64.h"
#include "win64_kernel.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CLASS_LIMIT 128
#define WINDOW_LIMIT 128
#define QUEUE_LIMIT 64
#define MESSAGE_LIMIT 1024
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_SIZE 5
#define WM_ACTIVATE 6
#define WM_SETFOCUS 7
#define WM_KILLFOCUS 8
#define WM_SETTEXT 12
#define WM_GETTEXT 13
#define WM_GETTEXTLENGTH 14
#define WM_CLOSE 0x10
#define WM_QUIT 0x12
#define WM_SHOWWINDOW 0x18
#define WM_NCCREATE 0x81
#define WM_NCDESTROY 0x82
#define WM_KEYDOWN 0x100
#define WM_KEYUP 0x101
#define WM_CHAR 0x102
#define WM_SYSKEYDOWN 0x104
#define WM_SYSKEYUP 0x105
#define WM_SYSCHAR 0x106
#define WM_MOUSEMOVE 0x200
#define WM_LBUTTONDOWN 0x201
#define WM_LBUTTONUP 0x202
#define WM_RBUTTONDOWN 0x204
#define WM_RBUTTONUP 0x205
#define WM_MBUTTONDOWN 0x207
#define WM_MBUTTONUP 0x208
#define WM_MOUSEWHEEL 0x20a
#define WM_MOUSEHWHEEL 0x20e
#define WM_MOUSELEAVE 0x2a3

typedef struct { uint32_t size,style;uint64_t proc;int32_t class_extra,window_extra;uint64_t instance,icon,cursor,brush,menu,name,small_icon; } class_ex;
typedef struct { uint32_t style,pad;uint64_t proc;int32_t class_extra,window_extra;uint64_t instance,icon,cursor,brush,menu,name; } class_plain;
typedef struct { uint64_t hwnd;uint32_t message,pad;uint64_t wp,lp;uint32_t time;int32_t x,y;uint32_t reserved; } guest_msg;
typedef struct { uint64_t parameter,instance,menu,parent;int32_t height,width,y,x;uint32_t style,pad;uint64_t title,class_name;uint32_t ex_style,pad2; } create_struct;
typedef struct { int32_t left,top,right,bottom; } guest_rect;
typedef struct { uint32_t size,flags;uint64_t hwnd;uint32_t hover,pad; } mouse_track;
_Static_assert(sizeof(mouse_track)==24 && offsetof(mouse_track,hover)==16,"Win64 TRACKMOUSEEVENT");
_Static_assert(sizeof(guest_rect)==16,"Win64 RECT");
_Static_assert(sizeof(class_plain)==72 && offsetof(class_plain,name)==64,"Win64 WNDCLASS");
_Static_assert(sizeof(class_ex)==80 && offsetof(class_ex,small_icon)==72,"Win64 WNDCLASSEX");
_Static_assert(sizeof(guest_msg)==48 && offsetof(guest_msg,wp)==16 && offsetof(guest_msg,time)==32 && offsetof(guest_msg,y)==40,"Win64 MSG");
_Static_assert(sizeof(create_struct)==80 && offsetof(create_struct,title)==56 && offsetof(create_struct,ex_style)==72,"Win64 CREATESTRUCT");
typedef struct window_class { int active;uint16_t atom;unsigned windows;class_ex data; } window_class;
typedef struct message_queue { int active;uint32_t tid;pthread_t thread;unsigned count;guest_msg messages[MESSAGE_LIMIT];uint8_t kinds[MESSAGE_LIMIT];uint32_t touch_tickets[MESSAGE_LIMIT];int quit;uint32_t exit_code;uint8_t keys[256];uint32_t new_bits,wake_mask;int waiting,blocking;void *event;int32_t cursor_count; } message_queue;
typedef struct guest_window {
    int active,destroying,message_only,visible,iconic,focused,track_leave;uint32_t style,ex_style;int32_t width,height,surface_width,surface_height;
    uint64_t proc,userdata,instance,menu,sdl_window_data;window_class *cls;message_queue *queue;
    uint16_t *title;uint32_t title_length;
    ANativeWindow *native;uint64_t generation,restore_focus_generation;unsigned char *extra;
} guest_window;
static const FrostpunkAndroidHost *platform;
static window_class classes[CLASS_LIMIT];static unsigned class_count;
static guest_window windows[WINDOW_LIMIT];static unsigned window_count;
static message_queue queues[QUEUE_LIMIT];static guest_window *top_window;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER,pump_mutex=PTHREAD_MUTEX_INITIALIZER,pump_lifecycle=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t configuration_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static pthread_t pump_thread;static _Atomic int pump_running,pump_stop;
static uint64_t pump_requested,pump_completed,pump_input_completed;static int pump_input_valid;static _Atomic unsigned pump_reports;
#define PUMP_RESCUE_MS 250
#ifdef FP_WINDOW_TRACE
static _Atomic unsigned window_traces;
#define WINDOW_TRACE(...) do{if(atomic_fetch_add(&window_traces,1)<4000){fprintf(stderr,__VA_ARGS__);}}while(0)
#else
#define WINDOW_TRACE(...) do{}while(0)
#endif
static unsigned api_users;static _Thread_local unsigned api_depth;
static pthread_key_t queue_key;static pthread_once_t queue_once=PTHREAD_ONCE_INIT;
static int host_focus,host_stopping,host_reconfiguring,primary_pointer=-1;static uint32_t mouse_buttons;
static int32_t mouse_x,mouse_y;static int pointer_outside;static uint8_t async_keys[256],async_pressed[256],input_scans[256];
static uint64_t ui_started,touch_delivery_epoch;
static uint32_t delivered_touch,completed_touch;
extern void frostpunk_android_cancel_input(void) __attribute__((weak));
extern void frostpunk_mobile_text_cancel(void) __attribute__((weak));
uint64_t win64_window_ui_begin(void){pthread_mutex_lock(&mutex);uint64_t epoch=++ui_started;pthread_mutex_unlock(&mutex);return epoch;}
void win64_window_ui_end(uint64_t epoch){pthread_mutex_lock(&mutex);if(delivered_touch&&epoch>touch_delivery_epoch)completed_touch=delivered_touch;pthread_mutex_unlock(&mutex);}
__attribute__((visibility("default"))) int frostpunk_android_touch_ready(uint32_t ticket){pthread_mutex_lock(&mutex);int ready=ticket&&completed_touch==ticket;pthread_mutex_unlock(&mutex);return ready;}

static _Noreturn void window_stop(const char *reason) { fprintf(stderr,"STOP Win64 window %s\n",reason);fflush(stderr);_Exit(73); }
static void stop_unused_pump(void);
static void stop_pump_locked(void);
static void window_enter(void) {
    if(api_depth++){return;}pthread_mutex_lock(&mutex);
    while(host_reconfiguring)pthread_cond_wait(&changed,&mutex);
    ++api_users;pthread_mutex_unlock(&mutex);
}
static void window_leave(void) {
    if(--api_depth)return;pthread_mutex_lock(&mutex);--api_users;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
}
static uint32_t thread_id(void) { return (uint32_t)syscall(SYS_gettid); }
static uint32_t milliseconds(void) { return (uint32_t)(win64_game_clock_ns(CLOCK_BOOTTIME)/1000000); }
static uint32_t event_milliseconds(uint64_t monotonic_ns) {
    if(!monotonic_ns)return milliseconds();
    struct timespec mono;
    if(clock_gettime(CLOCK_MONOTONIC,&mono))window_stop("input uptime clocks");
    /* Keep MSG.time in the same pause-adjusted domain as GetTickCount. */
    uint64_t mono_ms=(uint64_t)mono.tv_sec*1000+(uint64_t)mono.tv_nsec/1000000;
    uint64_t boot_ms=win64_game_clock_ns(CLOCK_BOOTTIME)/1000000;
    return (uint32_t)(monotonic_ns/1000000+boot_ms-mono_ms);
}
static uint64_t failure(uint32_t error,uint64_t value) { win64_set_error(error);return value; }
static guest_window *window_locked(uint64_t hwnd) {
    for(unsigned i=0;i<window_count;++i)if(hwnd==(uintptr_t)&windows[i] && windows[i].active)return &windows[i];return NULL;
}
static void queue_cleanup(void *opaque) {
    /* Guest ExitThread may leave a WNDPROC through its native thread boundary. */
    if(api_depth){pthread_mutex_lock(&mutex);api_depth=0;--api_users;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);}
    window_enter();
    message_queue *q=opaque;ANativeWindow *release=NULL;const FrostpunkAndroidHost *host;void *token;uint16_t *titles[WINDOW_LIMIT];unsigned title_count=0;
    pthread_mutex_lock(&mutex);WINDOW_TRACE("WQ_DESTROY tid=%u pending=%u\n",q->tid,q->count);q->active=0;q->count=0;q->quit=0;host=platform;token=q->event;q->event=NULL;
    for(unsigned i=0;i<window_count;++i)if(windows[i].active && windows[i].queue==q) {
        guest_window *w=&windows[i];w->active=0;w->track_leave=0;w->sdl_window_data=0;--w->cls->windows;if(w->native)release=w->native;w->native=NULL;titles[title_count++]=w->title;w->title=NULL;w->title_length=0;if(top_window==w){top_window=NULL;memset(input_scans,0,sizeof(input_scans));}
    }
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
    if(release && host)host->release_window(release);
    for(unsigned i=0;i<title_count;++i)free(titles[i]);
    if(token)win64_kernel_queue_event_destroy(token);stop_unused_pump();window_leave();
}
static void queue_initialize(void) { if(pthread_key_create(&queue_key,queue_cleanup))window_stop("thread queue key"); }
static message_queue *queue_locked(void) {
    if(pthread_once(&queue_once,queue_initialize))window_stop("thread queue initialization");
    message_queue *q=pthread_getspecific(queue_key);
    if(q && q->active)return q;
    for(unsigned i=0;i<QUEUE_LIMIT;++i)if(!queues[i].active) {
        /* Explicit virtual touch-to-mouse profile starts the thread's cursor
         * display count at zero; this does not assert a physical Android mouse. */
        q=&queues[i];memset(q,0,sizeof(*q));q->active=1;q->tid=thread_id();q->thread=pthread_self();
        WINDOW_TRACE("WQ_CREATE tid=%u slot=%u\n",q->tid,i);
        if(pthread_setspecific(queue_key,q))window_stop("thread queue attach");return q;
    }
    window_stop("thread queue capacity");
}
static void notify(void) {
    const FrostpunkAndroidHost *host;pthread_mutex_lock(&mutex);pthread_cond_broadcast(&changed);host=platform;pthread_mutex_unlock(&mutex);
    if(host)host->wake(host->context);
}
static void queue_signal_locked(message_queue *q) {
    if(q->event)win64_kernel_queue_event_set(q->event,q->waiting && ((q->new_bits&q->wake_mask)||host_stopping));
}
static uint32_t message_bits(unsigned kind,uint32_t message) {
    if(kind==1)return 0x108; /* Posted key/mouse IDs remain posted messages. */
    if(message>=WM_KEYDOWN && message<=WM_SYSCHAR)return 1;
    if(message==WM_MOUSEMOVE)return 2;
    if(message>=WM_LBUTTONDOWN && message<=WM_MOUSEHWHEEL)return 4;
    return 0x108;
}
static int enqueue_locked(message_queue *q,uint64_t hwnd,uint32_t msg,uint64_t wp,uint64_t lp,uint32_t time) {
    if(msg==WM_QUIT){q->quit=1;q->exit_code=(uint32_t)wp;q->new_bits|=0x108;queue_signal_locked(q);pthread_cond_broadcast(&changed);return 1;}
    if(q->count==MESSAGE_LIMIT)return 0;
    q->touch_tickets[q->count]=0;q->kinds[q->count]=2;q->messages[q->count++]=(guest_msg){.hwnd=hwnd,.message=msg,.wp=wp,.lp=lp,.time=time,.x=mouse_x,.y=mouse_y};
    q->new_bits|=message_bits(2,msg);queue_signal_locked(q);
    pthread_cond_broadcast(&changed);return 1;
}
static int post_locked(message_queue *q,uint64_t hwnd,uint32_t msg,uint64_t wp,uint64_t lp,uint32_t time) {
    /* PostQuitMessage is a low-priority queue flag. A directly posted WM_QUIT
     * is an ordinary FIFO message retaining its HWND/lParam and filters. */
    if(q->count==MESSAGE_LIMIT)return 0;q->touch_tickets[q->count]=0;q->kinds[q->count]=1;
    q->messages[q->count++]=(guest_msg){.hwnd=hwnd,.message=msg,.wp=wp,.lp=lp,.time=time,.x=mouse_x,.y=mouse_y};
    q->new_bits|=0x108;queue_signal_locked(q);
    pthread_cond_broadcast(&changed);return 1;
}
static void event_locked(guest_window *w,uint32_t msg,uint64_t wp,uint64_t lp,uint32_t time) {
    if(!enqueue_locked(w->queue,(uintptr_t)w,msg,wp,lp,time))window_stop("native input message queue overflow");
}
static void cancel_input_locked(void){
    memset(async_keys,0,sizeof(async_keys));memset(async_pressed,0,sizeof(async_pressed));memset(input_scans,0,sizeof(input_scans));
    mouse_buttons=0;primary_pointer=-1;delivered_touch=completed_touch=0;pointer_outside=1;
    for(unsigned n=0;n<QUEUE_LIMIT;++n){message_queue *q=&queues[n];if(!q->active)continue;
        memset(q->keys,0,sizeof(q->keys));unsigned kept=0;
        for(unsigned i=0;i<q->count;++i){guest_msg *m=&q->messages[i];
            int input=(m->message>=WM_KEYDOWN&&m->message<=WM_SYSCHAR)||m->message==WM_MOUSEMOVE
                ||(m->message>=WM_LBUTTONDOWN&&m->message<=WM_MOUSEHWHEEL)||m->message==WM_MOUSELEAVE;
            int stale_focus=q->kinds[i]==2&&(m->message==WM_SETFOCUS||(m->message==WM_ACTIVATE&&m->wp==1));
            if(input||stale_focus)continue;
            q->messages[kept]=*m;q->kinds[kept]=q->kinds[i];q->touch_tickets[kept]=0;++kept;
        }
        q->count=kept;q->new_bits&=~7u;if(!kept&&!q->quit)q->new_bits=0;queue_signal_locked(q);
    }
}
void win64_window_ime_key(uint32_t key,uint32_t scan){
    window_enter();pthread_mutex_lock(&mutex);guest_window *w=top_window;
    if(w&&w->native&&w->visible&&!w->iconic&&w->focused){
        uint64_t lp=1|((uint64_t)(scan&255)<<16)|((scan&0xff00)?UINT64_C(1)<<24:0);uint32_t time=milliseconds();
        event_locked(w,WM_KEYDOWN,key,lp,time);event_locked(w,WM_KEYUP,key,lp|(UINT64_C(3)<<30),time);
    }
    pthread_mutex_unlock(&mutex);window_leave();
}
static int pointer_inside_locked(const guest_window *w) {
    return w->active && !w->destroying && !w->message_only && w->native && w->visible && !w->iconic &&
        !pointer_outside && mouse_x>=0 && mouse_y>=0 && mouse_x<w->width && mouse_y<w->height;
}
static void mouse_leave_locked(guest_window *w,uint32_t time) {
    if(w->track_leave && !pointer_inside_locked(w)) {
        w->track_leave=0;
        if(!post_locked(w->queue,(uintptr_t)w,WM_MOUSELEAVE,0,0,time))window_stop("mouse leave message queue overflow");
    }
}
static uint16_t *copy_name(uint64_t address,size_t maximum) {
    if(address<65536)return NULL;size_t length=0;
    for(;length<maximum;++length){uint16_t unit;memcpy(&unit,win64_pointer(address+length*2,2,0),2);if(!unit)break;}
    if(length==maximum)return NULL;uint64_t copy=win64_alloc((length+1)*2,0);if(!copy)return NULL;
    memcpy(win64_pointer(copy,(length+1)*2,1),win64_pointer(address,(length+1)*2,0),(length+1)*2);return (uint16_t*)(uintptr_t)copy;
}
/* shortcut: window titles are owned UTF16 state, with a 32767-unit bound.
 * No Android Activity-title ABI or locale conversion is implied. Preserve
 * opaque Windows code units; add a larger capture profile only when needed. */
static int copy_title(uint64_t address,uint16_t **copy,uint32_t *length) {
    *copy=NULL;*length=0;if(!address)return 1;
    uint32_t n=0;
    for(;n<32768;++n){uint16_t unit;if(address>UINT64_MAX-(uint64_t)n*2)window_stop("title address overflow");memcpy(&unit,win64_pointer(address+(uint64_t)n*2,2,0),2);if(!unit)break;}
    if(n==32768)window_stop("title exceeds 32767 UTF16 units");if(!n)return 1;
    uint16_t *text=malloc(((size_t)n+1)*2);if(!text)return 0;
    memcpy(text,win64_pointer(address,(size_t)n*2,0),(size_t)n*2);text[n]=0;*copy=text;*length=n;return 1;
}
static uint64_t title_message(CPU *cpu,uint32_t message,uint64_t address) {
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(cpu->rcx);pthread_mutex_unlock(&mutex);
    if(!w)return failure(1400,0);
    if(message==WM_SETTEXT){
        uint16_t *text;uint32_t length;if(!copy_title(address,&text,&length))return failure(8,0);
        pthread_mutex_lock(&mutex);w=window_locked(cpu->rcx);uint16_t *old=NULL;
        if(w){old=w->title;w->title=text;w->title_length=length;}pthread_mutex_unlock(&mutex);
        if(!w){free(text);return failure(1400,0);}free(old);return 1;
    }
    pthread_mutex_lock(&mutex);w=window_locked(cpu->rcx);
    if(!w){pthread_mutex_unlock(&mutex);return failure(1400,0);}
    if(message==WM_GETTEXTLENGTH){uint32_t length=w->title_length;pthread_mutex_unlock(&mutex);return length;}
    uint64_t capacity=cpu->r8;if(!capacity){pthread_mutex_unlock(&mutex);return 0;}
    uint32_t length=w->title_length;if(capacity<=(uint64_t)length)length=(uint32_t)(capacity-1);
    uint16_t *text=malloc(((size_t)length+1)*2);
    if(text){if(length)memcpy(text,w->title,(size_t)length*2);text[length]=0;}pthread_mutex_unlock(&mutex);
    if(!text)return failure(8,0);memcpy(win64_pointer(address,((size_t)length+1)*2,1),text,((size_t)length+1)*2);free(text);return length;
}
static int equal_name(uint64_t left,uint64_t right) {
    for(unsigned i=0;i<256;++i){uint16_t a,b;memcpy(&a,win64_pointer(left+i*2,2,0),2);memcpy(&b,win64_pointer(right+i*2,2,0),2);
        if(a>='A'&&a<='Z')a+='a'-'A';if(b>='A'&&b<='Z')b+='a'-'A';if(a!=b)return 0;if(!a)return 1;}
    return 0;
}
static window_class *class_locked(uint64_t instance,uint64_t name) {
    for(unsigned i=0;i<class_count;++i){window_class *c=&classes[i];if(!c->active || (instance && c->data.instance!=instance))continue;
        if(name<65536?name==c->atom:equal_name(name,c->data.name))return c;}
    return NULL;
}
static uint64_t invoke(CPU *cpu,uint64_t target,uint64_t hwnd,uint32_t message,uint64_t wp,uint64_t lp) {
    if(!target)window_stop("NULL guest WNDPROC");CPU saved=*cpu;
    if(saved.rsp<64)window_stop("WNDPROC guest stack underflow");uint64_t frame=(saved.rsp-48)&~UINT64_C(15);
    win64_pointer(frame-8,56,1);cpu->rsp=frame;cpu->rcx=hwnd;cpu->rdx=message;cpu->r8=wp;cpu->r9=lp;
    push64(cpu,UINT64_C(0xe5ed0000));dispatch(cpu,target);if(cpu->rsp!=frame)window_stop("WNDPROC Win64 shadow/return stack imbalance");
    uint64_t result=cpu->rax;*cpu=saved;return result;
}
static uint64_t send(CPU *cpu,uint64_t hwnd,uint32_t message,uint64_t wp,uint64_t lp) {
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(hwnd);uint64_t proc=w?w->proc:0;int own=w && w->queue->tid==thread_id();pthread_mutex_unlock(&mutex);
    if(!w)return failure(1400,0);if(!own)return failure(50,0);return invoke(cpu,proc,hwnd,message,wp,lp);
}
static uint64_t destroy(CPU *cpu,uint64_t hwnd) {
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(hwnd);
    if(!w){pthread_mutex_unlock(&mutex);return failure(1400,0);}
    if(w->queue->tid!=thread_id()){pthread_mutex_unlock(&mutex);return failure(5,0);}
    if(w->destroying){pthread_mutex_unlock(&mutex);return failure(1400,0);}
    w->destroying=1;w->track_leave=0;pthread_mutex_unlock(&mutex);
    send(cpu,hwnd,WM_DESTROY,0,0);send(cpu,hwnd,WM_NCDESTROY,0,0);
    pthread_mutex_lock(&mutex);ANativeWindow *native=w->native;const FrostpunkAndroidHost *host=platform;uint16_t *title=w->title;
    w->native=NULL;w->active=0;w->sdl_window_data=0;w->title=NULL;w->title_length=0;--w->cls->windows;if(top_window==w){top_window=NULL;memset(input_scans,0,sizeof(input_scans));}pthread_mutex_unlock(&mutex);
    free(title);if(native)host->release_window(native);return 1;
}
void win64_window_configure(const FrostpunkAndroidHost *host) {
    if(host && (host->abi_version!=1 || host->struct_size<FP_ANDROID_HOST_BASE_SIZE || !host->acquire_window || !host->release_window ||
       !host->poll_event || !host->wait || !host->wake || !host->is_stopping))window_stop("Android host ABI1 callbacks");
    if(api_depth)window_stop("reconfigure inside USER32 operation");
    pthread_mutex_lock(&configuration_mutex);pthread_mutex_lock(&mutex);if(top_window)window_stop("reconfigure with a live HWND");
    if(api_users)window_stop("reconfigure with an active USER32 operation");
    host_reconfiguring=1;pthread_mutex_unlock(&mutex);
    pthread_mutex_lock(&pump_lifecycle);
    stop_pump_locked();pthread_mutex_lock(&pump_mutex);pthread_mutex_lock(&mutex);
    platform=host;pump_input_valid=0;host_stopping=host_focus=0;primary_pointer=-1;mouse_buttons=0;pointer_outside=0;delivered_touch=completed_touch=0;memset(async_keys,0,sizeof(async_keys));memset(async_pressed,0,sizeof(async_pressed));memset(input_scans,0,sizeof(input_scans));
    host_reconfiguring=0;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);pthread_mutex_unlock(&pump_mutex);pthread_mutex_unlock(&pump_lifecycle);pthread_mutex_unlock(&configuration_mutex);
}
ANativeWindow *win64_window_acquire(uint64_t hwnd,uint64_t *generation) {
    window_enter();
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(hwnd);const FrostpunkAndroidHost *host=platform;int eligible=w&&!w->destroying&&!w->message_only&&!host_stopping;pthread_mutex_unlock(&mutex);
    if(!eligible || !host){window_leave();return NULL;}ANativeWindow *native=host->acquire_window(host->context,generation);
    pthread_mutex_lock(&mutex);eligible=w->active&&!w->destroying&&!w->message_only&&!host_stopping;pthread_mutex_unlock(&mutex);
    if(!eligible && native){host->release_window(native);native=NULL;}window_leave();return native;
}
void win64_window_release(ANativeWindow *window) {if(window){window_enter();pthread_mutex_lock(&mutex);const FrostpunkAndroidHost *host=platform;pthread_mutex_unlock(&mutex);if(!host)window_stop("release without host");host->release_window(window);window_leave();}}

static unsigned virtual_key(unsigned key) {
    if(key>=AKEYCODE_A && key<=AKEYCODE_Z)return 'A'+key-AKEYCODE_A;
    if(key>=AKEYCODE_0 && key<=AKEYCODE_9)return '0'+key-AKEYCODE_0;
    if(key>=AKEYCODE_F1 && key<=AKEYCODE_F12)return 0x70+key-AKEYCODE_F1;
    switch(key) {
        case AKEYCODE_BACK:case AKEYCODE_ESCAPE:return 0x1b;case AKEYCODE_ENTER:case AKEYCODE_NUMPAD_ENTER:return 13;
        case AKEYCODE_TAB:return 9;case AKEYCODE_SPACE:return 32;case AKEYCODE_DEL:return 8;case AKEYCODE_FORWARD_DEL:return 0x2e;
        case AKEYCODE_DPAD_LEFT:return 0x25;case AKEYCODE_DPAD_UP:return 0x26;case AKEYCODE_DPAD_RIGHT:return 0x27;case AKEYCODE_DPAD_DOWN:return 0x28;
        case AKEYCODE_SHIFT_LEFT:case AKEYCODE_SHIFT_RIGHT:return 0x10;case AKEYCODE_CTRL_LEFT:case AKEYCODE_CTRL_RIGHT:return 0x11;
        case AKEYCODE_ALT_LEFT:case AKEYCODE_ALT_RIGHT:return 0x12;case AKEYCODE_CAPS_LOCK:return 0x14;case AKEYCODE_NUM_LOCK:return 0x90;
        case AKEYCODE_SCROLL_LOCK:return 0x91;case AKEYCODE_MOVE_HOME:return 0x24;case AKEYCODE_MOVE_END:return 0x23;
        case AKEYCODE_PAGE_UP:return 0x21;case AKEYCODE_PAGE_DOWN:return 0x22;case AKEYCODE_INSERT:return 0x2d;
        case AKEYCODE_MINUS:return 0xbd;case AKEYCODE_EQUALS:return 0xbb;case AKEYCODE_LEFT_BRACKET:return 0xdb;
        case AKEYCODE_RIGHT_BRACKET:return 0xdd;case AKEYCODE_BACKSLASH:return 0xdc;case AKEYCODE_SEMICOLON:return 0xba;
        case AKEYCODE_APOSTROPHE:return 0xde;case AKEYCODE_COMMA:return 0xbc;case AKEYCODE_PERIOD:return 0xbe;
        case AKEYCODE_SLASH:return 0xbf;case AKEYCODE_GRAVE:return 0xc0;default:return 0;
    }
}
static unsigned windows_scan(unsigned key) {
    /* The Android event carries a Linux evdev scan, which is not the Win32
     * Set-1 scan namespace. This module declares a bounded US keyboard map. */
    static const uint8_t letters[]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    if(key>=AKEYCODE_A && key<=AKEYCODE_Z)return letters[key-AKEYCODE_A];
    if(key==AKEYCODE_0)return 0x0b;if(key>=AKEYCODE_1 && key<=AKEYCODE_9)return 2+key-AKEYCODE_1;
    if(key>=AKEYCODE_F1 && key<=AKEYCODE_F10)return 0x3b+key-AKEYCODE_F1;
    switch(key) {
        case AKEYCODE_BACK:case AKEYCODE_ESCAPE:return 1;case AKEYCODE_ENTER:return 0x1c;case AKEYCODE_NUMPAD_ENTER:return 0xe01c;
        case AKEYCODE_TAB:return 0x0f;case AKEYCODE_SPACE:return 0x39;case AKEYCODE_DEL:return 0x0e;case AKEYCODE_FORWARD_DEL:return 0xe053;
        case AKEYCODE_DPAD_LEFT:return 0xe04b;case AKEYCODE_DPAD_UP:return 0xe048;case AKEYCODE_DPAD_RIGHT:return 0xe04d;case AKEYCODE_DPAD_DOWN:return 0xe050;
        case AKEYCODE_SHIFT_LEFT:return 0x2a;case AKEYCODE_SHIFT_RIGHT:return 0x36;case AKEYCODE_CTRL_LEFT:return 0x1d;case AKEYCODE_CTRL_RIGHT:return 0xe01d;
        case AKEYCODE_ALT_LEFT:return 0x38;case AKEYCODE_ALT_RIGHT:return 0xe038;case AKEYCODE_CAPS_LOCK:return 0x3a;case AKEYCODE_NUM_LOCK:return 0x45;
        case AKEYCODE_SCROLL_LOCK:return 0x46;case AKEYCODE_MOVE_HOME:return 0xe047;case AKEYCODE_MOVE_END:return 0xe04f;
        case AKEYCODE_PAGE_UP:return 0xe049;case AKEYCODE_PAGE_DOWN:return 0xe051;case AKEYCODE_INSERT:return 0xe052;
        case AKEYCODE_MINUS:return 0x0c;case AKEYCODE_EQUALS:return 0x0d;case AKEYCODE_LEFT_BRACKET:return 0x1a;
        case AKEYCODE_RIGHT_BRACKET:return 0x1b;case AKEYCODE_BACKSLASH:return 0x2b;case AKEYCODE_SEMICOLON:return 0x27;
        case AKEYCODE_APOSTROPHE:return 0x28;case AKEYCODE_COMMA:return 0x33;case AKEYCODE_PERIOD:return 0x34;
        case AKEYCODE_SLASH:return 0x35;case AKEYCODE_GRAVE:return 0x29;case AKEYCODE_F11:return 0x57;case AKEYCODE_F12:return 0x58;default:return 0;
    }
}
static uint64_t point_parameter(int32_t x,int32_t y) { return (uint16_t)x|((uint32_t)(uint16_t)y<<16); }
static uint32_t button_parameter(uint32_t meta) {return mouse_buttons|((meta&AMETA_SHIFT_ON)?4:0)|((meta&AMETA_CTRL_ON)?8:0);}
static int32_t coordinate(float value,float scale) {
    double result=(double)value*scale;if(!isfinite(result) || result<INT32_MIN || result>INT32_MAX)window_stop("Android input coordinate");return (int32_t)floor(result+.5);
}
static void accept_event(const FrostpunkAndroidEvent *event) {
    const FrostpunkAndroidHost *host;pthread_mutex_lock(&mutex);host=platform;pthread_mutex_unlock(&mutex);
    ANativeWindow *replacement=NULL;uint64_t generation=0;int width=0,height=0;
    if(event->type==FP_ANDROID_WINDOW && host) {
        replacement=host->acquire_window(host->context,&generation);
        if(replacement){width=ANativeWindow_getWidth(replacement);height=ANativeWindow_getHeight(replacement);if(width<0||height<0)window_stop("Android surface dimensions");}
    }
    ANativeWindow *release=NULL;int cancel_input=0;pthread_mutex_lock(&mutex);guest_window *w=top_window;
    uint32_t time=event_milliseconds(event->time_ns);
    int input_restore=w && host_focus && event->window_generation==w->generation &&
        (event->type==FP_ANDROID_KEY || event->type==FP_ANDROID_TEXT || event->type==FP_ANDROID_MOTION);
    /* SDL minimizes fullscreen HWNDs on focus loss; Android foregrounding must restore that virtual HWND. */
    if(w && w->native && w->visible && w->iconic &&
       ((event->type==FP_ANDROID_FOCUS && event->focused) || input_restore)) {
        w->iconic=0;w->style&=~0x20000000u;
        event_locked(w,WM_SIZE,0,point_parameter(w->width,w->height),time);
        if(input_restore && !w->focused){w->focused=1;event_locked(w,WM_ACTIVATE,1,0,time);event_locked(w,WM_SETFOCUS,0,0,time);}
    }
    if(event->type==FP_ANDROID_WINDOW && w) {
        if(generation>=w->generation) {
            if(generation!=w->generation){
                cancel_input_locked();cancel_input=1;
                if(w->focused){w->focused=0;event_locked(w,WM_ACTIVATE,0,0,time);event_locked(w,WM_KILLFOCUS,0,0,time);}
                w->restore_focus_generation=replacement?generation:0;
            }
            release=w->native;w->native=replacement;replacement=NULL;w->generation=generation;
            /* Android surface extent is independent of the requested guest
             * client area (and of DXGI's swapchain buffer extent). */
            w->surface_width=width;w->surface_height=height;
            if(!w->native)pointer_outside=1;
            mouse_leave_locked(w,time);
            int focused=host_focus && w->visible && !w->iconic && w->native;
            if(!focused)memset(input_scans,0,sizeof(input_scans));
            if(w->focused!=focused){w->focused=focused;event_locked(w,WM_ACTIVATE,focused,0,time);event_locked(w,focused?WM_SETFOCUS:WM_KILLFOCUS,0,0,time);}
        }
    } else if(event->type==FP_ANDROID_FOCUS) {
        host_focus=!!event->focused;
        int focused=host_focus && w && w->visible && !w->iconic && w->native;
        if(w && w->focused!=focused){w->focused=focused;event_locked(w,WM_ACTIVATE,focused,0,time);event_locked(w,focused?WM_SETFOCUS:WM_KILLFOCUS,0,0,time);}
        if(!host_focus){cancel_input_locked();cancel_input=1;if(w)w->restore_focus_generation=0;}
    } else if(event->type==FP_ANDROID_DESTROY) {
        host_stopping=1;memset(input_scans,0,sizeof(input_scans));if(w){w->track_leave=0;event_locked(w,WM_CLOSE,0,0,time);}
        for(unsigned i=0;i<QUEUE_LIMIT;++i)if(queues[i].active)enqueue_locked(&queues[i],0,WM_QUIT,0,0,time);
    } else if(w && event->window_generation==w->generation && w->native && w->visible && !w->iconic && w->focused && event->type==FP_ANDROID_TEXT) {
        if(event->keycode && event->keycode<=UINT16_MAX)event_locked(w,WM_CHAR,event->keycode,1,time);
    } else if(w && event->window_generation==w->generation && w->native && w->visible && !w->iconic && w->focused && event->type==FP_ANDROID_KEY) {
        unsigned key=virtual_key(event->keycode);int up=event->action==AKEY_EVENT_ACTION_UP;
        if(key && (up || event->action==AKEY_EVENT_ACTION_DOWN)) {
            if(!up){async_pressed[key]=1;if(!event->repeat && (key==0x14||key==0x90||key==0x91))async_keys[key]^=1;}
            async_keys[key]=(uint8_t)((async_keys[key]&1)|(up?0:0x80));
            int alt=(event->meta&AMETA_ALT_ON)!=0,system=alt || key==0x12 || key==0x79;unsigned scan=windows_scan(event->keycode);
            if(scan)input_scans[(scan&127)|((scan&0xff00)?128:0)]=up?0:0x80;
            /* Android repeat is cumulative; each event here is one uncoalesced
             * Win32 keystroke, whose repeat field must therefore remain one. */
            uint64_t lp=1|((uint64_t)(scan&255)<<16)|((scan&0xff00)?UINT64_C(1)<<24:0)|(alt?UINT64_C(1)<<29:0)|((up||event->repeat)?UINT64_C(1)<<30:0)|(up?UINT64_C(1)<<31:0);
            event_locked(w,system?(up?WM_SYSKEYUP:WM_SYSKEYDOWN):(up?WM_KEYUP:WM_KEYDOWN),key,lp,time);
        }
    } else if(w && event->window_generation==w->generation && w->native && w->visible && !w->iconic && event->type==FP_ANDROID_MOTION) {
        int mouse=(event->source&AINPUT_SOURCE_MOUSE)==AINPUT_SOURCE_MOUSE;unsigned action=event->action;
        int chosen=mouse || action==AMOTION_EVENT_ACTION_DOWN || event->pointer_id==primary_pointer;
        if(action==AMOTION_EVENT_ACTION_DOWN && !mouse)primary_pointer=event->pointer_id;
        if(chosen) {
            int physical_width=event->width>0?event->width:w->surface_width,physical_height=event->height>0?event->height:w->surface_height;
            float sx=physical_width>0?(float)w->width/physical_width:1,sy=physical_height>0?(float)w->height/physical_height:1;
            int32_t x=coordinate(event->x,sx),y=coordinate(event->y,sy);uint32_t before=mouse_buttons,after=before;
            if(mouse){after=(event->buttons&AMOTION_EVENT_BUTTON_PRIMARY?1:0)|(event->buttons&AMOTION_EVENT_BUTTON_SECONDARY?2:0)|(event->buttons&AMOTION_EVENT_BUTTON_TERTIARY?16:0);}
            else if(action==AMOTION_EVENT_ACTION_DOWN)after|=1;
            else if(action==AMOTION_EVENT_ACTION_UP || action==AMOTION_EVENT_ACTION_POINTER_UP || action==AMOTION_EVENT_ACTION_CANCEL){after&=~1u;primary_pointer=-1;}
            mouse_x=x;mouse_y=y;mouse_buttons=after;uint64_t lp=point_parameter(x,y);uint32_t wp=button_parameter(event->meta);
            pointer_outside=action==AMOTION_EVENT_ACTION_HOVER_EXIT || action==AMOTION_EVENT_ACTION_CANCEL;
            unsigned first_message=w->queue->count;
            if(action==AMOTION_EVENT_ACTION_MOVE || action==AMOTION_EVENT_ACTION_HOVER_MOVE || action==AMOTION_EVENT_ACTION_DOWN){
                event_locked(w,WM_MOUSEMOVE,wp,lp,time);
            }
            const unsigned masks[]={1,2,16},down[]={WM_LBUTTONDOWN,WM_RBUTTONDOWN,WM_MBUTTONDOWN},up[]={WM_LBUTTONUP,WM_RBUTTONUP,WM_MBUTTONUP};
            const unsigned keys[]={1,2,4};
            for(unsigned i=0;i<3;++i)if((before^after)&masks[i]){async_keys[keys[i]]=after&masks[i]?0x80:0;if(after&masks[i])async_pressed[keys[i]]=1;event_locked(w,after&masks[i]?down[i]:up[i],wp,lp,time);}
            if(action==AMOTION_EVENT_ACTION_SCROLL) {
                const float scroll[]={event->scroll_y,event->scroll_x};const unsigned messages[]={WM_MOUSEWHEEL,WM_MOUSEHWHEEL};
                for(unsigned i=0;i<2;++i)if(scroll[i]){int32_t delta=coordinate(scroll[i],120);if(delta<INT16_MIN||delta>INT16_MAX)window_stop("wheel delta range");event_locked(w,messages[i],wp|((uint32_t)(uint16_t)delta<<16),lp,time);}
            }
            /* A companion MOVE must not acknowledge a button transition still in the queue. */
            if(w->queue->count>first_message)w->queue->touch_tickets[w->queue->count-1]=event->repeat;
            mouse_leave_locked(w,time);
        }
    }
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
    if(replacement)host->release_window(replacement);if(release)host->release_window(release);
    if(cancel_input){if(frostpunk_mobile_text_cancel)frostpunk_mobile_text_cancel();if(frostpunk_android_cancel_input)frostpunk_android_cancel_input();}
}
static void host_ended(void) {
    pthread_mutex_lock(&mutex);host_stopping=1;memset(input_scans,0,sizeof(input_scans));
    for(unsigned q=0;q<QUEUE_LIMIT;++q)if(queues[q].active){
        if(!queues[q].quit)enqueue_locked(&queues[q],0,WM_QUIT,0,0,milliseconds());else queue_signal_locked(&queues[q]);
    }
    pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
}
static int input_sequence_backend(const FrostpunkAndroidHost *host) {
    return host && host->struct_size>=offsetof(FrostpunkAndroidHost,event_sequence)+sizeof(host->event_sequence)
        && host->event_sequence;
}
static void *pump_main(void *opaque) {
    const FrostpunkAndroidHost *host=opaque;
    while(!atomic_load(&pump_stop)) {
        /* Bounded wait: if a host wake is ever lost, a pump_events requester
         * parked on its ticket is still served within PUMP_RESCUE_MS. */
        struct timespec before,after;clock_gettime(CLOCK_MONOTONIC,&before);
        FrostpunkAndroidEvent event;int status=host->poll_event(host->context,&event,PUMP_RESCUE_MS);
        clock_gettime(CLOCK_MONOTONIC,&after);
        int timed_out=!status && (after.tv_sec-before.tv_sec)*1000+(after.tv_nsec-before.tv_nsec)/1000000>=PUMP_RESCUE_MS;
        if(atomic_load(&pump_stop))break;
        if(status<0){host_ended();break;}
#ifdef FP_WINDOW_TRACE
        {static uint64_t rounds,last_requested;static struct timespec last;++rounds;
            if(after.tv_sec>=last.tv_sec+5){pthread_mutex_lock(&mutex);uint64_t requested=pump_requested;pthread_mutex_unlock(&mutex);
                if(last.tv_sec)WINDOW_TRACE("WINDOW_PUMP_RATE rounds=%llu requests=%llu seconds=%ld\n",(unsigned long long)rounds,(unsigned long long)(requested-last_requested),(long)(after.tv_sec-last.tv_sec));
                rounds=0;last_requested=requested;last=after;}}
#endif
        for(;;) {
            pthread_mutex_lock(&mutex);uint64_t ticket=pump_requested,completed=pump_completed;pthread_mutex_unlock(&mutex);
            if(!status && ticket==completed)break;
            int tracked=input_sequence_backend(host);
            uint64_t input_ticket=tracked?host->event_sequence(host->context):0;
            if(timed_out && atomic_fetch_add(&pump_reports,1)<8)
                fprintf(stderr,"WINDOW_PUMP_LOST_WAKE ticket=%llu completed=%llu\n",(unsigned long long)ticket,(unsigned long long)completed);
            timed_out=0;
            if(status>0)accept_event(&event);
            while(!atomic_load(&pump_stop) && (status=host->poll_event(host->context,&event,0))>0)accept_event(&event);
            if(status<0){host_ended();break;}
            pthread_mutex_lock(&mutex);pump_completed=ticket;int more=pump_requested>ticket;
            if(tracked){pump_input_completed=input_ticket;pump_input_valid=1;}
            pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
            if(!more || atomic_load(&pump_stop))break;
        }
        if(status<0)break;
    }
    return NULL;
}
/* The lifecycle lock serializes join and replacement. Neither the window nor
 * kernel lock may be held across join; host wake is latched by ABI1's provider. */
static void stop_pump_locked(void) {
    if(!atomic_load(&pump_running))return;
    pthread_mutex_lock(&mutex);const FrostpunkAndroidHost *host=platform;atomic_store(&pump_stop,1);pthread_mutex_unlock(&mutex);
    if(pthread_equal(pthread_self(),pump_thread))window_stop("event pump self join");
    host->wake(host->context);if(pthread_join(pump_thread,NULL))window_stop("event pump join");
    pthread_mutex_lock(&mutex);atomic_store(&pump_running,0);pump_input_valid=0;pump_completed=pump_requested;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&mutex);
}
static void stop_unused_pump(void) {
    pthread_mutex_lock(&pump_lifecycle);pthread_mutex_lock(&mutex);int active=0;
    for(unsigned i=0;i<QUEUE_LIMIT;++i)active|=queues[i].active;
    pthread_mutex_unlock(&mutex);if(!active)stop_pump_locked();pthread_mutex_unlock(&pump_lifecycle);
}
static int start_pump(void) {
    if(atomic_load(&pump_running))return 1;
    int result=1;pthread_mutex_lock(&pump_lifecycle);pthread_mutex_lock(&pump_mutex);pthread_mutex_lock(&mutex);
    if(platform && !host_stopping && !atomic_load(&pump_running)) {
        atomic_store(&pump_stop,0);atomic_store(&pump_running,1);
        if(pthread_create(&pump_thread,NULL,pump_main,(void*)platform)){atomic_store(&pump_running,0);win64_set_error(8);result=0;}
    }
    pthread_mutex_unlock(&mutex);pthread_mutex_unlock(&pump_mutex);pthread_mutex_unlock(&pump_lifecycle);return result;
}
static void pump_events(int timeout) {
    /* A synchronous Peek still observes queued native input. Ask the sole
     * consumer to drain once; the generation barrier never polls a timer.
     * Concurrent callers share a round: holding pump_lifecycle across the
     * wait let worker threads that Peek in loops re-take it before a woken
     * waiter ran, starving that thread (startup stalls). Stop completes
     * every ticket, and the window lock orders tickets against it. */
    if(atomic_load(&pump_running)) {
        pthread_mutex_lock(&mutex);
        if(!atomic_load(&pump_running)){pthread_mutex_unlock(&mutex);pump_events(timeout);return;}
        const FrostpunkAndroidHost *host=platform;
        int tracked=input_sequence_backend(host);
        if(tracked){
            pthread_mutex_unlock(&mutex);uint64_t input_ticket=host->event_sequence(host->context);
            pthread_mutex_lock(&mutex);
            if(!atomic_load(&pump_running)){pthread_mutex_unlock(&mutex);pump_events(timeout);return;}
            /* A matching checkpoint covers every earlier publication, including
             * an event already popped by the consumer but not yet delivered. */
            if(pump_input_valid && input_ticket==pump_input_completed){pthread_mutex_unlock(&mutex);return;}
        }
        uint64_t ticket=++pump_requested;pthread_mutex_unlock(&mutex);
        host->wake(host->context);pthread_mutex_lock(&mutex);
        struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);++deadline.tv_sec;
        while(atomic_load(&pump_running) && !host_stopping && pump_completed<ticket) {
            if(pthread_cond_clockwait(&changed,&mutex,CLOCK_MONOTONIC,&deadline)!=ETIMEDOUT || pump_completed>=ticket)continue;
            /* Safety net: a ticket open for a second means a lost wake or a
             * damaged counter; report it, keep the counter at the ticket and
             * wake the pump again. */
            if(atomic_fetch_add(&pump_reports,1)<8)fprintf(stderr,"WINDOW_PUMP_STALL ticket=%llu requested=%llu completed=%llu\n",
                (unsigned long long)ticket,(unsigned long long)pump_requested,(unsigned long long)pump_completed);
            if(pump_requested<ticket)pump_requested=ticket;
            clock_gettime(CLOCK_MONOTONIC,&deadline);++deadline.tv_sec;
            pthread_mutex_unlock(&mutex);host->wake(host->context);pthread_mutex_lock(&mutex);
        }
        pthread_mutex_unlock(&mutex);return;
    }
    pthread_mutex_lock(&pump_mutex);if(atomic_load(&pump_running)){pthread_mutex_unlock(&pump_mutex);pump_events(timeout);return;}
    pthread_mutex_lock(&mutex);const FrostpunkAndroidHost *host=platform;pthread_mutex_unlock(&mutex);
    if(host) {
        for(unsigned i=0;i<MESSAGE_LIMIT;++i) {
            FrostpunkAndroidEvent event;int status=host->poll_event(host->context,&event,i?0:timeout);
            if(status>0)accept_event(&event);
            else {if(status<0)host_ended();break;}
        }
    }
    pthread_mutex_unlock(&pump_mutex);
}
static uint64_t register_class(CPU *cpu,int extended) {
    class_ex data={.size=80};uint64_t input=win64_argument(cpu,0);
    if(extended){memcpy(&data,win64_pointer(input,80,0),80);if(data.size!=80)return failure(87,0);}
    else {class_plain plain;memcpy(&plain,win64_pointer(input,72,0),72);data.style=plain.style;memcpy(&data.proc,&plain.proc,64);}
    if(!data.proc || data.class_extra<0 || data.window_extra<0)return failure(87,0);
    if(data.class_extra>4096 || data.window_extra>4096)return failure(50,0);
    uint16_t *name=copy_name(data.name,256);if(!name)return failure(87,0);
    pthread_mutex_lock(&mutex);
    if(class_locked(data.instance,data.name)){pthread_mutex_unlock(&mutex);win64_free((uintptr_t)name);return failure(1410,0);}
    if(class_count==CLASS_LIMIT){pthread_mutex_unlock(&mutex);win64_free((uintptr_t)name);return failure(8,0);}
    window_class *cls=&classes[class_count];cls->active=1;cls->atom=(uint16_t)(0xc000+class_count++);data.name=(uintptr_t)name;cls->data=data;
    pthread_mutex_unlock(&mutex);return cls->atom;
}
static uint64_t create(CPU *cpu) {
    uint64_t name=win64_argument(cpu,1),style=win64_argument(cpu,3),parent=win64_argument(cpu,8),instance=win64_argument(cpu,10);
    int message_only=parent==UINT64_MAX-2;
    if((parent && !message_only) || (style&0x40000000u))return failure(50,0);
    /* Supplied SDL helpers use style0/exstyle0, no menu. Other message-only
     * style/owner/desktop-geometry combinations remain explicit unsupported. */
    if(message_only && (style || win64_argument(cpu,0) || win64_argument(cpu,9)))return failure(50,0);
    pthread_mutex_lock(&mutex);const FrostpunkAndroidHost *host=platform;pthread_mutex_unlock(&mutex);
    uint64_t generation=0;ANativeWindow *native=NULL;int physical_width=0,physical_height=0;
    if(!message_only){
        if(!host || host->is_stopping(host->context))return failure(50,0);
        pump_events(0);native=host->acquire_window(host->context,&generation);
        if(!native)return failure(21,0);physical_width=ANativeWindow_getWidth(native);physical_height=ANativeWindow_getHeight(native);
        if(physical_width<=0 || physical_height<=0){host->release_window(native);return failure(21,0);}
    }
    int width=(int32_t)win64_argument(cpu,6),height=(int32_t)win64_argument(cpu,7);
    if(width<=0)width=physical_width;if(height<=0)height=physical_height;
    pthread_mutex_lock(&mutex);window_class *cls=class_locked(instance,name);
    if(!cls || (!message_only && top_window) || window_count==WINDOW_LIMIT){uint32_t error=!cls?1411:window_count==WINDOW_LIMIT?8:50;pthread_mutex_unlock(&mutex);if(native)host->release_window(native);return failure(error,0);}
    guest_window *w=&windows[window_count++];w->active=1;w->message_only=message_only;w->visible=!!(style&0x10000000u);w->iconic=!!(style&0x20000000u);w->focused=!message_only && host_focus && w->visible && !w->iconic;w->style=message_only?0x04c00000u:(uint32_t)style;
    w->ex_style=(uint32_t)win64_argument(cpu,0);w->width=width;w->height=height;w->surface_width=physical_width;w->surface_height=physical_height;w->proc=cls->data.proc;w->instance=instance;
    w->menu=win64_argument(cpu,9);w->cls=cls;w->queue=queue_locked();w->native=native;w->generation=generation;if(!message_only)top_window=w;++cls->windows;
    if(cls->data.window_extra){w->extra=calloc(1,(size_t)cls->data.window_extra);if(!w->extra)window_stop("window extra allocation");}
    pthread_mutex_unlock(&mutex);
    /* Windows replaces HWND_MESSAGE with its internal thread Message window.
     * This token is backed by the real adapter queue. Internal-root window
     * queries are outside this bounded API; it never denotes a native surface. */
    uint64_t structure=win64_alloc(80,1);create_struct data={.parameter=win64_argument(cpu,11),.instance=instance,.menu=w->menu,.parent=message_only?(uintptr_t)w->queue:parent,
        .height=(int32_t)win64_argument(cpu,7),.width=(int32_t)win64_argument(cpu,6),.y=(int32_t)win64_argument(cpu,5),.x=(int32_t)win64_argument(cpu,4),
        .style=(uint32_t)style,.title=win64_argument(cpu,2),.class_name=name,.ex_style=w->ex_style};
    memcpy(win64_pointer(structure,80,1),&data,80);uint64_t hwnd=(uintptr_t)w;
    uint64_t accepted=send(cpu,hwnd,WM_NCCREATE,0,structure);pthread_mutex_lock(&mutex);int alive=w->active;pthread_mutex_unlock(&mutex);
    if(!accepted || !alive) {
        if(alive){pthread_mutex_lock(&mutex);w->destroying=1;pthread_mutex_unlock(&mutex);send(cpu,hwnd,WM_NCDESTROY,0,0);pthread_mutex_lock(&mutex);uint16_t *title=w->title;w->title=NULL;w->title_length=0;w->active=0;w->sdl_window_data=0;--cls->windows;if(top_window==w)top_window=NULL;w->native=NULL;pthread_mutex_unlock(&mutex);free(title);if(native)host->release_window(native);}
        win64_free(structure);return 0;
    }
    uint64_t result=send(cpu,hwnd,WM_CREATE,0,structure);win64_free(structure);
    pthread_mutex_lock(&mutex);alive=w->active;pthread_mutex_unlock(&mutex);
    if(result==UINT64_MAX || !alive){if(alive)destroy(cpu,hwnd);return 0;}
    pthread_mutex_lock(&mutex);if(!message_only)event_locked(w,WM_SIZE,0,point_parameter(width,height),milliseconds());
    if(w->visible)event_locked(w,WM_SHOWWINDOW,1,0,milliseconds());
    if(w->focused){event_locked(w,WM_ACTIVATE,1,0,milliseconds());event_locked(w,WM_SETFOCUS,0,0,milliseconds());}pthread_mutex_unlock(&mutex);notify();return hwnd;
}
static void key_removed(message_queue *q,const guest_msg *msg) {
    if(msg->message==WM_KILLFOCUS){memset(q->keys,0,sizeof(q->keys));return;}
    const unsigned down[]={WM_LBUTTONDOWN,WM_RBUTTONDOWN,WM_MBUTTONDOWN},mouse_up[]={WM_LBUTTONUP,WM_RBUTTONUP,WM_MBUTTONUP},mouse[]={1,2,4};
    for(unsigned i=0;i<3;++i)if(msg->message==down[i]||msg->message==mouse_up[i]){q->keys[mouse[i]]=msg->message==down[i]?0x80:0;return;}
    if(msg->message!=WM_KEYDOWN && msg->message!=WM_KEYUP && msg->message!=WM_SYSKEYDOWN && msg->message!=WM_SYSKEYUP)return;
    unsigned key=(unsigned)msg->wp;if(key>=256)return;int up=msg->message==WM_KEYUP || msg->message==WM_SYSKEYUP;
    if(!up && !(msg->lp&(UINT64_C(1)<<30)) && (key==0x14||key==0x90||key==0x91))q->keys[key]^=1;
    q->keys[key]=(uint8_t)((q->keys[key]&1)|(up?0:0x80));
}
static int eligible(const guest_msg *msg,uint64_t hwnd,unsigned low,unsigned high) {
    return (hwnd==UINT64_MAX?msg->hwnd==0:!hwnd || hwnd==msg->hwnd) && ((!low&&!high) || (msg->message>=low && msg->message<=high));
}
static uint64_t message_wait(CPU *cpu) {
    unsigned count=(uint32_t)win64_argument(cpu,0);uint64_t address=win64_argument(cpu,1);
    int all=(uint32_t)win64_argument(cpu,2)!=0;uint32_t timeout=(uint32_t)win64_argument(cpu,3),mask=(uint32_t)win64_argument(cpu,4);
    if(count>63)return failure(87,UINT32_MAX);
    /* shortcut: only the original WaitAny path is implemented; WaitAll needs
     * a separate queue eligibility/atomic-consumption contract before use. */
    if(all)return failure(50,UINT32_MAX);
    uint64_t ids[63];if(count)memcpy(ids,win64_pointer(address,count*8,0),count*8);
    pthread_mutex_lock(&mutex);message_queue *q=queue_locked();
    if(!q->event)q->event=win64_kernel_queue_event_create();
    if(!q->event){pthread_mutex_unlock(&mutex);return UINT32_MAX;}
    q->waiting=1;q->wake_mask=mask;queue_signal_locked(q);pthread_cond_broadcast(&changed);void *token=q->event;pthread_mutex_unlock(&mutex);
    uint32_t result=UINT32_MAX;
    if(start_pump()) {
        pump_events(0);pthread_mutex_lock(&mutex);int fresh=(q->new_bits&mask)||host_stopping;pthread_mutex_unlock(&mutex);
        /* Native MsgWait's already-new-input fast path precedes the handle
         * wait, so it must not consume an otherwise-ready auto event. */
        result=fresh?count:win64_kernel_queue_wait(ids,count,token,timeout);
    }
    pthread_mutex_lock(&mutex);q->waiting=0;queue_signal_locked(q);pthread_mutex_unlock(&mutex);return result;
}
static uint64_t retrieve(CPU *cpu,int blocking) {
    uint64_t output=win64_argument(cpu,0),hwnd=win64_argument(cpu,1);unsigned low=(uint16_t)win64_argument(cpu,2),high=(uint16_t)win64_argument(cpu,3);
    unsigned flags=blocking?1:(uint32_t)win64_argument(cpu,4);int remove=flags&1;
    if(flags&~3u)return failure(50,blocking?UINT32_MAX:0);
    if((low||high) && low>high)return failure(87,blocking?UINT32_MAX:0);
    win64_pointer(output,sizeof(guest_msg),1);pump_events(0);
    for(;;) {
        pthread_mutex_lock(&mutex);message_queue *q=queue_locked();
        if(hwnd && hwnd!=UINT64_MAX) {guest_window *w=window_locked(hwnd);if(!w || w->queue!=q){pthread_mutex_unlock(&mutex);return failure(1400,blocking?UINT32_MAX:0);}}
        uint32_t checked=8;
        if(!low&&!high)checked|=0x107;
        else {
            if(low<=WM_SYSCHAR && high>=WM_KEYDOWN)checked|=1;
            if(low<=WM_MOUSEMOVE && high>=WM_MOUSEMOVE)checked|=2;
            if(low<=WM_MOUSEHWHEEL && high>=WM_LBUTTONDOWN)checked|=4;
        }
        q->new_bits&=~checked;queue_signal_locked(q);
        int selected=-1;
        for(unsigned kind=1;kind<=2 && selected<0;++kind)for(unsigned i=0;i<q->count;++i)
            if(q->kinds[i]==kind && eligible(&q->messages[i],hwnd,low,high)){selected=(int)i;break;}
        guest_msg msg={0};int found=selected>=0;
        if(found){msg=q->messages[selected];if(remove && q->kinds[selected]==1)WINDOW_TRACE("WQ_GET tid=%u msg=0x%x wp=0x%llx left=%u\n",q->tid,msg.message,(unsigned long long)msg.wp,q->count-1);if(remove){key_removed(q,&msg);
            guest_window *restore=msg.message==WM_SETFOCUS?window_locked(msg.hwnd):NULL;int resized=0;
            if(restore&&restore->restore_focus_generation==restore->generation&&host_focus&&restore->native&&restore->visible){
                restore->restore_focus_generation=0;resized=restore->iconic;restore->iconic=0;restore->style&=~0x20000000u;restore->focused=1;
            }
            if(q->touch_tickets[selected]){delivered_touch=q->touch_tickets[selected];touch_delivery_epoch=ui_started;}
            --q->count;memmove(&q->messages[selected],&q->messages[selected+1],(q->count-(unsigned)selected)*sizeof(msg));memmove(&q->kinds[selected],&q->kinds[selected+1],q->count-(unsigned)selected);
            memmove(&q->touch_tickets[selected],&q->touch_tickets[selected+1],(q->count-(unsigned)selected)*sizeof(uint32_t));
            if(resized){
                unsigned kept=0;
                for(unsigned i=0;i<q->count;++i){guest_msg *m=&q->messages[i];
                    if(q->kinds[i]==2&&m->hwnd==msg.hwnd&&(m->message==WM_KILLFOCUS||(m->message==WM_ACTIVATE&&!m->wp)||(m->message==WM_SIZE&&m->wp==1)))continue;
                    q->messages[kept]=*m;q->kinds[kept]=q->kinds[i];q->touch_tickets[kept++]=q->touch_tickets[i];
                }
                q->count=kept;event_locked(restore,WM_SIZE,0,point_parameter(restore->width,restore->height),milliseconds());
            }
        }}
        else if(q->quit || host_stopping){msg=(guest_msg){.message=WM_QUIT,.wp=q->exit_code,.time=milliseconds(),.x=mouse_x,.y=mouse_y};found=1;if(remove)q->quit=0;}
        if(found){pthread_mutex_unlock(&mutex);memcpy(win64_pointer(output,sizeof(msg),1),&msg,sizeof(msg));return blocking && msg.message==WM_QUIT?0:1;}
        if(!blocking){pthread_mutex_unlock(&mutex);return 0;}
        const FrostpunkAndroidHost *host=platform;
        if(!host || atomic_load(&pump_running)){q->blocking=1;pthread_cond_broadcast(&changed);pthread_cond_wait(&changed,&mutex);q->blocking=0;pthread_mutex_unlock(&mutex);}
        else {pthread_mutex_unlock(&mutex);if(!start_pump())return UINT32_MAX;pump_events(0);}
    }
}
static uint64_t post_message(CPU *cpu,int thread) {
    uint64_t handle=win64_argument(cpu,0),wp=win64_argument(cpu,2),lp=win64_argument(cpu,3);uint32_t msg=(uint32_t)win64_argument(cpu,1);
    pthread_mutex_lock(&mutex);message_queue *q=NULL;uint64_t hwnd=0;
    if(thread){for(unsigned i=0;i<QUEUE_LIMIT;++i)if(queues[i].active && queues[i].tid==(uint32_t)handle){q=&queues[i];break;}}
    else if(!handle)q=queue_locked();
    else {guest_window *w=window_locked(handle);if(w){q=w->queue;hwnd=handle;}}
    if(!q){WINDOW_TRACE("WQ_POST from=%u thread=%d target=0x%llx msg=0x%x wp=0x%llx result=no_queue\n",thread_id(),thread,(unsigned long long)handle,msg,(unsigned long long)wp);
        pthread_mutex_unlock(&mutex);return failure(thread?1444:1400,0);}
    int result=post_locked(q,hwnd,msg,wp,lp,milliseconds());
    WINDOW_TRACE("WQ_POST from=%u thread=%d target=0x%llx tid=%u msg=0x%x wp=0x%llx count=%u result=%d\n",thread_id(),thread,(unsigned long long)handle,q->tid,msg,(unsigned long long)wp,q->count,result);
    pthread_mutex_unlock(&mutex);if(!result)return failure(1816,0);notify();return 1;
}
static unsigned translated_character(message_queue *q,unsigned key) {
    int shift=!!(q->keys[0x10]&0x80),control=!!(q->keys[0x11]&0x80),caps=q->keys[0x14]&1;
    if(key>='A' && key<='Z')return control?key-'A'+1:(shift^caps)?key:key+'a'-'A';
    if(key>='0' && key<='9')return shift?(unsigned char)")!@#$%^&*("[key-'0']:key;
    if(key==8 || key==9 || key==13 || key==27 || key==32)return key;
    const unsigned keys[]={0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0,0xdb,0xdc,0xdd,0xde};
    const char *normal=";=,-./`[\\]'",*shifted=":+<_>?~{|}\"";
    for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i)if(key==keys[i])return (unsigned char)(shift?shifted[i]:normal[i]);
    return 0;
}
static uint64_t window_long(CPU *cpu,int setting,int wide_pointer) {
    uint64_t hwnd=win64_argument(cpu,0),value=setting?win64_argument(cpu,2):0;int index=(int)win64_argument(cpu,1);
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(hwnd);uint64_t old=0;
    if(!w){pthread_mutex_unlock(&mutex);return failure(1400,0);}
    if(setting && w->queue->tid!=thread_id()){pthread_mutex_unlock(&mutex);return failure(5,0);}
    if(setting && !wide_pointer)value=(uint64_t)(int64_t)(int32_t)value;
    if(index==-4){if(setting && (!wide_pointer || !value)){pthread_mutex_unlock(&mutex);return failure(87,0);}old=w->proc;if(setting)w->proc=value;}
    else if(index==-21){old=w->userdata;if(setting)w->userdata=value;}
    else if(index==-6){old=w->instance;if(setting)w->instance=value;}
    else if(index==-12){old=w->menu;if(setting)w->menu=value;}
    else if(index==-16){old=w->style;if(setting){if(value&0x40000000u){pthread_mutex_unlock(&mutex);return failure(50,0);}w->style=(uint32_t)value;}}
    else if(index==-20){old=w->ex_style;if(setting)w->ex_style=(uint32_t)value;}
    else if(index==-8){if(setting && value){pthread_mutex_unlock(&mutex);return failure(50,0);}}
    else if(index>=0 && (unsigned)index+(wide_pointer?8u:4u)<=(unsigned)w->cls->data.window_extra){size_t n=wide_pointer?8:4;memcpy(&old,w->extra+index,n);if(setting)memcpy(w->extra+index,&value,n);}
    else {pthread_mutex_unlock(&mutex);return failure(1413,0);}
    pthread_mutex_unlock(&mutex);return wide_pointer?old:(uint32_t)old;
}
static uint64_t window_property(CPU *cpu,int operation) {
    uint64_t hwnd=cpu->rcx,key=cpu->rdx;
    pthread_mutex_lock(&mutex);guest_window *w=window_locked(hwnd);
    if(!w){pthread_mutex_unlock(&mutex);return failure(1400,0);}
    if(!key){pthread_mutex_unlock(&mutex);return operation==1?failure(87,0):0;}
    /* shortcut: only SDL's observed property is provided; add an atom/string
     * registry if another real consumer needs a different property name. */
    static const char expected[]="SDL_WindowData";
    int equal=key>=65536;
    if(equal && key>UINT64_MAX-(sizeof(expected)-1)*2)window_stop("property name address overflow");
    for(unsigned i=0;equal && i<sizeof(expected);++i){uint16_t unit;memcpy(&unit,win64_pointer(key+i*2,2,0),2);
        unsigned target=(unsigned char)expected[i];if(unit>='A'&&unit<='Z')unit+='a'-'A';if(target>='A'&&target<='Z')target+='a'-'A';equal=unit==target;}
    if(!equal){pthread_mutex_unlock(&mutex);return failure(50,0);}
    uint64_t value=w->sdl_window_data;
    if(operation==1){w->sdl_window_data=cpu->r8;value=1;}
    else if(operation==2)w->sdl_window_data=0;
    pthread_mutex_unlock(&mutex);return value;
}
void win64_window_drag_accept_files(CPU *cpu) {
    window_enter();pthread_mutex_lock(&mutex);guest_window *w=window_locked(cpu->rcx);
    if(w)w->ex_style=(w->ex_style&~UINT32_C(0x10))|((uint32_t)cpu->rdx?0x10u:0u);
    pthread_mutex_unlock(&mutex);if(!w)win64_set_error(1400);window_leave();
}
static int pointer_backend(const FrostpunkAndroidHost *host) {
    return host && host->struct_size>=offsetof(FrostpunkAndroidHost,set_pointer_visible)+sizeof(host->set_pointer_visible) && host->set_pointer_visible;
}
static uint64_t show_cursor(CPU *cpu) {
    int saved_errno=errno;uint32_t saved_error=win64_get_error();
    pthread_mutex_lock(&mutex);message_queue *q=queue_locked();guest_window *w=top_window;const FrostpunkAndroidHost *host=platform;
    int affected=w && w->active && !w->destroying && w->native && w->queue==q;
    if(affected && (host_stopping || !pointer_backend(host))){pthread_mutex_unlock(&mutex);window_stop("ShowCursor active window requires pointer backend");}
    int32_t old=q->cursor_count;int show=(uint32_t)cpu->rcx!=0;
    if((show&&old==INT32_MAX)||(!show&&old==INT32_MIN)){pthread_mutex_unlock(&mutex);window_stop("ShowCursor counter exceeds bounded int32 profile");}
    int32_t next=show?old+1:old-1;q->cursor_count=next;pthread_mutex_unlock(&mutex);
    /* shortcut: the guest's current cursor is NULL: LoadCursorW has no backed
     * resources and SetCursor accepts only NULL. The per-thread display count
     * is real, but neither sign can make a NULL image visible. Hide the actual
     * Android pointer instead of fabricating an HCURSOR or an arrow image. */
    if(affected && !host->set_pointer_visible(host->context,0))window_stop("Android pointer hide request failed");
    win64_set_error(saved_error);errno=saved_errno;return(uint32_t)next;
}
static void hide_null_cursor(void) {
    int saved_errno=errno;uint32_t saved_error=win64_get_error();
    pthread_mutex_lock(&mutex);guest_window *w=top_window;const FrostpunkAndroidHost *host=platform;
    int available=w && w->active && !w->destroying && w->native && !host_stopping && w->queue->tid==thread_id() && pointer_backend(host);pthread_mutex_unlock(&mutex);
    if(available && !host->set_pointer_visible(host->context,0))window_stop("Android NULL cursor hide request failed");
    win64_set_error(saved_error);errno=saved_errno;
}
/* shortcut: one origin-zero guest desktop, own active native window, and
 * inbounds client coordinates only. Warp private guest input state and its
 * real queue; never inject Android/global pointer motion or invent an image. */
static uint64_t set_cursor_position(CPU *cpu) {
    int saved_errno=errno;uint32_t saved_error=win64_get_error();pump_events(0);
    int32_t x=(int32_t)(uint32_t)cpu->rcx,y=(int32_t)(uint32_t)cpu->rdx;uint32_t time=milliseconds();
    pthread_mutex_lock(&mutex);guest_window *w=top_window;
    int valid=w&&w->active&&!w->destroying&&w->native&&!host_stopping&&w->queue->tid==thread_id();
    if(!valid){pthread_mutex_unlock(&mutex);errno=saved_errno;return failure(50,0);}
    if(x<0||y<0||x>=w->width||y>=w->height){pthread_mutex_unlock(&mutex);window_stop("SetCursorPos outside inbounds virtual desktop profile");}
    if(w->queue->count==MESSAGE_LIMIT){pthread_mutex_unlock(&mutex);window_stop("SetCursorPos input message queue overflow");}
    mouse_x=x;mouse_y=y;pointer_outside=0;uint32_t buttons=mouse_buttons|((async_keys[0x10]&0x80)?4:0)|((async_keys[0x11]&0x80)?8:0);
    enqueue_locked(w->queue,(uintptr_t)w,WM_MOUSEMOVE,buttons,point_parameter(x,y),time);pthread_mutex_unlock(&mutex);
    notify();errno=saved_errno;win64_set_error(saved_error);return 1;
}
static uint64_t track_mouse(CPU *cpu) {
    int saved_errno=errno;uint32_t saved_error=win64_get_error(),error=0,size;
    if(!cpu->rcx){errno=saved_errno;return failure(87,0);}
    memcpy(&size,win64_pointer(cpu->rcx,4,0),4);
    if(size!=sizeof(mouse_track)){errno=saved_errno;return failure(87,0);}
    mouse_track request;memcpy(&request,win64_pointer(cpu->rcx,sizeof(request),0),sizeof(request));
    /* shortcut: client LEAVE only; add hover timers/nonclient geometry on a
     * proven caller. SDL141B1EEE0 requests flags2 and leaves hover uninitialized. */
    if(request.flags&~UINT32_C(0xc0000002)){errno=saved_errno;return failure(50,0);}
    if(request.flags!=2 && request.flags!=UINT32_C(0x80000002) && request.flags!=UINT32_C(0x40000000)){
        errno=saved_errno;return failure(87,0);
    }
    void *output=request.flags==UINT32_C(0x40000000)?win64_pointer(cpu->rcx,sizeof(request),1):NULL;
    pump_events(0);pthread_mutex_lock(&mutex);message_queue *q=queue_locked();
    if(output) {
        request.flags=0;request.hwnd=0;request.hover=0;request.pad=0;
        for(unsigned i=0;i<window_count;++i)if(windows[i].active && windows[i].queue==q && windows[i].track_leave){
            request.flags=2;request.hwnd=(uintptr_t)&windows[i];break;
        }
    } else {
        guest_window *w=window_locked(request.hwnd);
        if(!w || w->destroying)error=1400;
        else if(w->message_only || w->queue!=q)error=50;
        else if(request.flags==UINT32_C(0x80000002))w->track_leave=0;
        else {w->track_leave=1;mouse_leave_locked(w,milliseconds());}
    }
    pthread_mutex_unlock(&mutex);
    if(output)memcpy(output,&request,sizeof(request));
    if(!error)notify();errno=saved_errno;
    if(error)return failure(error,0);win64_set_error(saved_error);return 1;
}
/* shortcut: the virtual guest keyboard uses the native-oracle US layout only.
 * Add a layout provider before accepting other HKLs or translation modes. */
static uint64_t map_virtual_key_ex(CPU *cpu) {
    static const uint16_t us_vk_to_vsc_ex[256]={
        0x0000,0x0000,0x0000,0xe046,0x0000,0x0000,0x0000,0x0000,0x000e,0x000f,0x0000,0x0000,0x004c,0x001c,0x0000,0x0000,
        0x002a,0x001d,0x0038,0xe11d,0x003a,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0001,0x0000,0x0000,0x0000,0x0000,
        0x0039,0x0049,0x0051,0x004f,0x0047,0x004b,0x0048,0x004d,0x0050,0x0000,0x0000,0x0000,0x0054,0x0052,0x0053,0x0063,
        0x000b,0x0002,0x0003,0x0004,0x0005,0x0006,0x0007,0x0008,0x0009,0x000a,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x001e,0x0030,0x002e,0x0020,0x0012,0x0021,0x0022,0x0023,0x0017,0x0024,0x0025,0x0026,0x0032,0x0031,0x0018,
        0x0019,0x0010,0x0013,0x001f,0x0014,0x0016,0x002f,0x0011,0x002d,0x0015,0x002c,0xe05b,0xe05c,0xe05d,0x0000,0xe05f,
        0x0052,0x004f,0x0050,0x0051,0x004b,0x004c,0x004d,0x0047,0x0048,0x0049,0x0037,0x004e,0x0000,0x004a,0x0053,0xe035,
        0x003b,0x003c,0x003d,0x003e,0x003f,0x0040,0x0041,0x0042,0x0043,0x0044,0x0057,0x0058,0x0064,0x0065,0x0066,0x0067,
        0x0068,0x0069,0x006a,0x006b,0x006c,0x006d,0x006e,0x0076,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0045,0x0046,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x002a,0x0036,0x001d,0xe01d,0x0038,0xe038,0xe06a,0xe069,0xe067,0xe068,0xe065,0xe066,0xe032,0xe020,0xe02e,0xe030,
        0xe019,0xe010,0xe024,0xe022,0xe06c,0xe06d,0xe06b,0xe021,0x0000,0x0000,0x0027,0x000d,0x0033,0x000c,0x0034,0x0035,
        0x0029,0x0073,0x007e,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
        0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x001a,0x002b,0x001b,0x0028,0x0000,
        0x0000,0x0000,0x0056,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0071,0x005c,0x007b,0x0000,0x006f,0x005a,0x0000,
        0x0000,0x005b,0x0000,0x005f,0x0000,0x005e,0x0000,0x0000,0x0000,0x005d,0x0000,0x0062,0x0000,0x0000,0x0000,0x0000,
    };
    if((uint32_t)cpu->rdx!=4 || cpu->r8)return failure(50,0);
    uint32_t code=(uint32_t)cpu->rcx;
    return code<256?us_vk_to_vsc_ex[code]:0;
}
static int window_call(CPU *cpu,const char *name) {
    /* Explicit virtual guest policy, independent of Android user settings. */
    if(!strcmp(name,"GetDoubleClickTime")){cpu->rax=500;return 1;}
    /* shortcut: this touch-oriented backend has no native cursor resources.
     * Failed LoadCursorW leaves SDL's real allocated cursor object alive.
     * SetCursor(NULL) is a valid hidden/no-cursor transition; no handle is
     * invented, and non-NULL cursor operations remain unavailable. */
    if(!strcmp(name,"LoadCursorW")){cpu->rax=failure(50,0);return 1;}
    if(!strcmp(name,"SetCursor")){if(cpu->rcx)cpu->rax=failure(50,0);else{hide_null_cursor();cpu->rax=0;}return 1;}
    if(!strcmp(name,"ShowCursor")){cpu->rax=show_cursor(cpu);return 1;}
    if(!strcmp(name,"SetCursorPos")){cpu->rax=set_cursor_position(cpu);return 1;}
    if(!strcmp(name,"TrackMouseEvent")){cpu->rax=track_mouse(cpu);return 1;}
    if(!strcmp(name,"DestroyCursor") || !strcmp(name,"DestroyIcon")){cpu->rax=failure(cpu->rcx?50:1402,0);return 1;}
    /* shortcut: no clipboard backend; SDL's zero initial sequence stays
     * unchanged, so this reports no fabricated external clipboard events. */
    if(!strcmp(name,"GetClipboardSequenceNumber")){cpu->rax=failure(50,0);return 1;}
    /* Android-generated messages carry no Windows extra-info metadata. */
    if(!strcmp(name,"GetMessageExtraInfo")){cpu->rax=0;return 1;}
    /* shortcut: no Windows-layout provider; SDL keeps its compiled keymap
     * on zero at all three original calls. Add real layouts when needed. */
    if(!strcmp(name,"MapVirtualKeyW")){cpu->rax=failure(50,0);return 1;}
    if(!strcmp(name,"MapVirtualKeyExA")){cpu->rax=map_virtual_key_ex(cpu);return 1;}
    if(!strcmp(name,"GetPropW") || !strcmp(name,"SetPropW") || !strcmp(name,"RemovePropW")){cpu->rax=window_property(cpu,name[0]=='S'?1:name[0]=='R'?2:0);return 1;}
    if(!strcmp(name,"GetMenu")){pthread_mutex_lock(&mutex);guest_window *w=window_locked(cpu->rcx);uint64_t menu=w?w->menu:0;pthread_mutex_unlock(&mutex);cpu->rax=w?menu:failure(1400,0);return 1;}
    /* shortcut: Android owns the surface placement and pointer confinement;
     * SDL ignores these failures. Do not invent a move or WINDOWPOS event. */
    if(!strcmp(name,"SetWindowPos")){pthread_mutex_lock(&mutex);int valid=window_locked(cpu->rcx)!=NULL;pthread_mutex_unlock(&mutex);cpu->rax=failure(valid?50:1400,0);return 1;}
    if(!strcmp(name,"ClipCursor")){cpu->rax=failure(50,0);return 1;}
    /* shortcut: the empty-controller profile has no device-notification
     * backend. SDL's optional helper cleans up normally on this NULL result. */
    if(!strcmp(name,"RegisterDeviceNotificationW")){cpu->rax=failure(50,0);return 1;}
    if(!strcmp(name,"MsgWaitForMultipleObjects")){cpu->rax=message_wait(cpu);return 1;}
    if(!strcmp(name,"RegisterClassW") || !strcmp(name,"RegisterClassExW")){cpu->rax=register_class(cpu,!strcmp(name,"RegisterClassExW"));return 1;}
    if(!strcmp(name,"UnregisterClassW") || !strcmp(name,"GetClassInfoExW")) {
        int query=name[0]=='G';uint64_t instance=win64_argument(cpu,query?0:1),key=win64_argument(cpu,query?1:0);
        pthread_mutex_lock(&mutex);window_class *cls=class_locked(instance,key);class_ex data;if(cls)data=cls->data;
        if(!cls || (!query && cls->windows)){uint32_t error=!cls?1411:1412;pthread_mutex_unlock(&mutex);cpu->rax=failure(error,0);return 1;}
        if(!query)cls->active=0;pthread_mutex_unlock(&mutex);
        if(query){uint64_t out=win64_argument(cpu,2);uint32_t size;memcpy(&size,win64_pointer(out,4,0),4);if(size!=80){cpu->rax=failure(87,0);return 1;}memcpy(win64_pointer(out,80,1),&data,80);}
        cpu->rax=1;return 1;
    }
    if(!strcmp(name,"CreateWindowExW")){cpu->rax=create(cpu);return 1;}
    if(!strcmp(name,"DestroyWindow")){cpu->rax=destroy(cpu,win64_argument(cpu,0));return 1;}
    if(!strcmp(name,"SetWindowTextW")){
        pthread_mutex_lock(&mutex);guest_window *w=window_locked(cpu->rcx);int own=w&&w->queue->tid==thread_id();pthread_mutex_unlock(&mutex);
        /* Native SetWindowTextW normalizes the full LRESULT != -1, including
         * an invalid HWND's zero result/error1400. Cross-thread delivery is
         * unavailable in this port and must not report a fabricated success. */
        if(!w)cpu->rax=failure(1400,1);else if(!own)cpu->rax=failure(50,0);
        else cpu->rax=send(cpu,cpu->rcx,WM_SETTEXT,0,cpu->rdx)!=UINT64_MAX;
        return 1;
    }
    if(!strcmp(name,"GetParent")){pthread_mutex_lock(&mutex);int valid=window_locked(win64_argument(cpu,0))!=NULL;pthread_mutex_unlock(&mutex);cpu->rax=valid?0:failure(1400,0);return 1;}
    if(!strcmp(name,"SendMessageW") || !strcmp(name,"SendMessageA")) {
        uint32_t msg=(uint32_t)win64_argument(cpu,1);if(name[11]=='A' && msg>=WM_SETTEXT && msg<=WM_GETTEXTLENGTH){cpu->rax=failure(50,0);return 1;}
        cpu->rax=send(cpu,win64_argument(cpu,0),msg,win64_argument(cpu,2),win64_argument(cpu,3));return 1;
    }
    if(!strcmp(name,"CallWindowProcW")){
        pthread_mutex_lock(&mutex);guest_window *w=window_locked(win64_argument(cpu,1));int valid=w && w->queue->tid==thread_id();pthread_mutex_unlock(&mutex);
        cpu->rax=valid?invoke(cpu,win64_argument(cpu,0),win64_argument(cpu,1),(uint32_t)win64_argument(cpu,2),win64_argument(cpu,3),win64_argument(cpu,4)):failure(w?50:1400,0);return 1;
    }
    if(!strcmp(name,"DefWindowProcW") || !strcmp(name,"DefWindowProcA")) {
        uint32_t msg=(uint32_t)win64_argument(cpu,1);
        if((msg>=WM_SETTEXT&&msg<=WM_GETTEXTLENGTH) || (msg==WM_NCCREATE&&name[13]=='W')){
            if(name[13]!='W'){cpu->rax=failure(50,0);return 1;}
            int saved_errno=errno;
            if(msg==WM_NCCREATE){
                pthread_mutex_lock(&mutex);int valid=window_locked(cpu->rcx)!=NULL;pthread_mutex_unlock(&mutex);
                if(!valid)cpu->rax=failure(1400,0);
                else{create_struct data;memcpy(&data,win64_pointer(cpu->r9,sizeof(data),0),sizeof(data));cpu->rax=title_message(cpu,WM_SETTEXT,data.title);}
            }else cpu->rax=title_message(cpu,msg,cpu->r9);
            errno=saved_errno;return 1;
        }
        if(msg==WM_CLOSE)destroy(cpu,win64_argument(cpu,0));cpu->rax=msg==WM_NCCREATE?1:0;return 1;
    }
    if(!strcmp(name,"GetWindowLongPtrW") || !strcmp(name,"SetWindowLongPtrW") || !strcmp(name,"GetWindowLongW") || !strcmp(name,"SetWindowLongW")) {
        cpu->rax=window_long(cpu,name[0]=='S',strstr(name,"Ptr")!=NULL);return 1;
    }
    if(!strcmp(name,"PeekMessageW") || !strcmp(name,"PeekMessageA") || !strcmp(name,"GetMessageW") || !strcmp(name,"GetMessageA")) {cpu->rax=retrieve(cpu,name[0]=='G');return 1;}
    if(!strcmp(name,"PostMessageW") || !strcmp(name,"PostMessageA") || !strcmp(name,"PostThreadMessageA") || !strcmp(name,"PostThreadMessageW")) {cpu->rax=post_message(cpu,!strncmp(name,"PostThread",10));return 1;}
    if(!strcmp(name,"PostQuitMessage")){pthread_mutex_lock(&mutex);message_queue *q=queue_locked();enqueue_locked(q,0,WM_QUIT,(uint32_t)win64_argument(cpu,0),0,milliseconds());pthread_mutex_unlock(&mutex);notify();return 1;}
    if(!strcmp(name,"DispatchMessageW") || !strcmp(name,"DispatchMessageA")) {
        guest_msg msg;memcpy(&msg,win64_pointer(win64_argument(cpu,0),48,0),48);cpu->rax=msg.hwnd && msg.message!=WM_QUIT?send(cpu,msg.hwnd,msg.message,msg.wp,msg.lp):0;return 1;
    }
    if(!strcmp(name,"TranslateMessage")) {
        guest_msg msg;memcpy(&msg,win64_pointer(win64_argument(cpu,0),48,0),48);int key=msg.message==WM_KEYDOWN||msg.message==WM_KEYUP||msg.message==WM_SYSKEYDOWN||msg.message==WM_SYSKEYUP;
        pthread_mutex_lock(&mutex);message_queue *q=queue_locked();
        unsigned character=(msg.message==WM_KEYDOWN||msg.message==WM_SYSKEYDOWN)?translated_character(q,(unsigned)msg.wp):0;
        if(character && !post_locked(q,msg.hwnd,msg.message==WM_SYSKEYDOWN?WM_SYSCHAR:WM_CHAR,character,msg.lp,msg.time))window_stop("translated character queue overflow");
        pthread_mutex_unlock(&mutex);if(character)notify();cpu->rax=key;return 1;
    }
    if(!strcmp(name,"IsRectEmpty")) {
        if(!cpu->rcx){cpu->rax=1;return 1;}
        guest_rect rect;memcpy(&rect,win64_pointer(cpu->rcx,sizeof(rect),0),sizeof(rect));
        cpu->rax=rect.left>=rect.right || rect.top>=rect.bottom;return 1;
    }
    if(!strcmp(name,"GetClientRect") || !strcmp(name,"GetWindowRect")) {
        pump_events(0);pthread_mutex_lock(&mutex);guest_window *w=window_locked(win64_argument(cpu,0));int message_only=w&&w->message_only;guest_rect rect={0,0,w?w->width:0,w?w->height:0};pthread_mutex_unlock(&mutex);
        if(!w)cpu->rax=failure(1400,0);else if(message_only)cpu->rax=failure(50,0);else{memcpy(win64_pointer(win64_argument(cpu,1),16,1),&rect,16);cpu->rax=1;}return 1;
    }
    if(!strcmp(name,"ShowWindow")) {
        int command=(int)win64_argument(cpu,1);if(command<0 || command>11){cpu->rax=failure(87,0);return 1;}
        pthread_mutex_lock(&mutex);guest_window *w=window_locked(win64_argument(cpu,0));
        if(!w){pthread_mutex_unlock(&mutex);cpu->rax=failure(1400,0);return 1;}
        if(w->message_only){pthread_mutex_unlock(&mutex);cpu->rax=failure(50,0);return 1;}
        if(w->queue->tid!=thread_id()){pthread_mutex_unlock(&mutex);cpu->rax=failure(50,0);return 1;}
        int before=w->visible,old_iconic=w->iconic;w->visible=command!=0;
        if(command)w->iconic=command==2 || command==6 || command==7 || command==11;
        w->style=w->visible?w->style|0x10000000u:w->style&~0x10000000u;
        w->style=w->iconic?w->style|0x20000000u:w->style&~0x20000000u;
        mouse_leave_locked(w,milliseconds());
        if(before!=w->visible)event_locked(w,WM_SHOWWINDOW,w->visible,0,milliseconds());
        if(old_iconic!=w->iconic)event_locked(w,WM_SIZE,w->iconic?1:0,w->iconic?0:point_parameter(w->width,w->height),milliseconds());
        int focused=host_focus && w->visible && !w->iconic && w->native;if(w->focused!=focused){w->focused=focused;event_locked(w,WM_ACTIVATE,focused,0,milliseconds());event_locked(w,focused?WM_SETFOCUS:WM_KILLFOCUS,0,0,milliseconds());}
        if(!focused)memset(input_scans,0,sizeof(input_scans));
        pthread_mutex_unlock(&mutex);notify();cpu->rax=before;return 1;
    }
    if(!strcmp(name,"GetFocus") || !strcmp(name,"GetForegroundWindow") || !strcmp(name,"SetForegroundWindow")) {
        pump_events(0);pthread_mutex_lock(&mutex);guest_window *w=top_window;uint64_t focus=w && w->native && w->focused && w->visible && !w->iconic?(uintptr_t)w:0;pthread_mutex_unlock(&mutex);
        cpu->rax=name[0]=='S'?(focus && focus==win64_argument(cpu,0)?1:failure(50,0)):focus;return 1;
    }
    if(!strcmp(name,"IsWindow") || !strcmp(name,"IsWindowVisible") || !strcmp(name,"IsIconic")) {
        pthread_mutex_lock(&mutex);guest_window *w=window_locked(win64_argument(cpu,0));cpu->rax=w?(!strcmp(name,"IsWindow")?1:!strcmp(name,"IsWindowVisible")?w->visible:w->iconic):0;pthread_mutex_unlock(&mutex);return 1;
    }
    if(!strcmp(name,"GetKeyboardState") || !strcmp(name,"GetKeyState") || !strcmp(name,"GetAsyncKeyState")) {
        pump_events(0);pthread_mutex_lock(&mutex);message_queue *q=queue_locked();unsigned key=(uint32_t)win64_argument(cpu,0);
        if(!strcmp(name,"GetKeyboardState")){memcpy(win64_pointer(win64_argument(cpu,0),256,1),q->keys,256);cpu->rax=1;}
        else if(key>=256)cpu->rax=0;
        else if(!strcmp(name,"GetAsyncKeyState")){cpu->rax=(async_keys[key]&0x80?0x8000:0)|async_pressed[key];async_pressed[key]=0;}
        else cpu->rax=(q->keys[key]&0x80?0x8000:0)|(q->keys[key]&1);
        pthread_mutex_unlock(&mutex);return 1;
    }
    if(!strcmp(name,"GetCursorPos")){pump_events(0);pthread_mutex_lock(&mutex);int valid=top_window && top_window->native;int32_t point[]={mouse_x,mouse_y};pthread_mutex_unlock(&mutex);if(!valid)cpu->rax=failure(50,0);else{memcpy(win64_pointer(win64_argument(cpu,0),8,1),point,8);cpu->rax=1;}return 1;}
    if(!strcmp(name,"ScreenToClient") || !strcmp(name,"ClientToScreen")){pthread_mutex_lock(&mutex);guest_window *w=window_locked(win64_argument(cpu,0));int valid=w&&!w->message_only;pthread_mutex_unlock(&mutex);if(valid)win64_pointer(win64_argument(cpu,1),8,1);cpu->rax=valid?1:failure(w?50:1400,0);return 1;}
    return 0;
}
int win64_window_input_snapshot(uint64_t hwnd,uint8_t keys[256]) {
    if(!keys)return -1;
    window_enter();pump_events(0);pthread_mutex_lock(&mutex);
    guest_window *w=window_locked(hwnd);
    int result=-1;
    if(w && !w->destroying && !w->message_only)
        result=!host_stopping && w->native && w->visible && !w->iconic && w->focused?1:0;
    if(result==1)memcpy(keys,input_scans,256);else if(result==0)memset(keys,0,256);
    pthread_mutex_unlock(&mutex);window_leave();return result;
}
int win64_window_call(CPU *cpu,const char *name) {
    window_enter();int result=window_call(cpu,name);window_leave();return result;
}
/* Direct handlers for win64.c's import cache (cached step 32): window_call's branches without the
 * name comparisons in front of them.  The render thread's message loop (140E27CF0) calls
 * MsgWaitForMultipleObjects and PeekMessageA every time it wakes up. */
static int fast_message_wait(CPU *cpu){window_enter();cpu->rax=message_wait(cpu);window_leave();return 1;}
static int fast_peek_message(CPU *cpu){window_enter();cpu->rax=retrieve(cpu,0);window_leave();return 1;}
static int fast_get_message(CPU *cpu){window_enter();cpu->rax=retrieve(cpu,1);window_leave();return 1;}
int (*win64_window_fast_import(const char *name))(CPU *){
    if(!strcmp(name,"MsgWaitForMultipleObjects"))return fast_message_wait;
    if(!strcmp(name,"PeekMessageA")||!strcmp(name,"PeekMessageW"))return fast_peek_message;
    if(!strcmp(name,"GetMessageA")||!strcmp(name,"GetMessageW"))return fast_get_message;
    return NULL;
}
void win64_window_self_test(void) {
    uint32_t saved_error=win64_get_error();int saved_errno=errno;
    CPU cpu={0};cpu.rsp=0x12345678;cpu.rcx=0x12345678;
    if(!win64_window_call(&cpu,"IsWindow") || cpu.rax || cpu.rsp!=0x12345678 || win64_window_call(&cpu,"UnknownWindowFunction"))window_stop("ABI/no-op import protocol self-test");
    if(!win64_window_call(&cpu,"GetDoubleClickTime") || cpu.rax!=500)window_stop("virtual double-click policy self-test");
    cpu.rcx=0;cpu.rdx=0x7f00;
    if(!win64_window_call(&cpu,"LoadCursorW") || cpu.rax || win64_get_error()!=50)window_stop("cursor unavailable self-test");
    win64_set_error(0x5a17);
    if(!win64_window_call(&cpu,"SetCursor") || cpu.rax || win64_get_error()!=0x5a17)window_stop("hidden cursor self-test");
    if(!win64_window_call(&cpu,"DestroyIcon") || cpu.rax || win64_get_error()!=1402)window_stop("NULL icon self-test");
    win64_set_error(saved_error);errno=saved_errno;
    fprintf(stderr,"WIN64_WINDOW_SELF_TEST WNDCLASS=72 WNDCLASSEX=80 MSG=48 CREATESTRUCT=80 strict_HWND=1 virtual_doubleclick500=1 cursor_unavailable50=1 hidden_cursor=1 no_rsp_pop=1 passed\n");
}
