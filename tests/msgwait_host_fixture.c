/* Isolated injected Android-host race proof. Production window/kernel sources
 * are linked unchanged; the POSIX cond-wait wrapper only observes transitions. */
#define main reused_window_fixture_main
#include "window_fixture.c"
#undef main

const char *win64_string_a(uint64_t address){for(size_t n=0;n<65536;++n)if(!*(const char*)win64_pointer(address+n,1,0))return win64_pointer(address,n+1,0);abort();}
static pthread_mutex_t control=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_changed=PTHREAD_COND_INITIALIZER;
static pthread_t owner_thread,poll_identity,requester_identity;static int requester_identified;
static pthread_cond_t *window_condition;
static unsigned stage,gate,allow_read,allow_return,outer_hold,outer_release,ticket_wake,helper_done;
static unsigned poll_calls,active_polls,max_active_polls,poll_threads,keys_delivered,kernel_wait_seen;
static int track,negative_active;static uint64_t test_atom;
int __real_pthread_cond_wait(pthread_cond_t*,pthread_mutex_t*);
int __wrap_pthread_cond_wait(pthread_cond_t *condition,pthread_mutex_t *lock){
    if(condition!=&control_changed && condition!=&host.changed && pthread_equal(pthread_self(),owner_thread)){
        pthread_mutex_lock(&control);
        if(!window_condition)window_condition=condition;
        if(negative_active||condition!=window_condition){kernel_wait_seen=1;pthread_cond_broadcast(&control_changed);}
        pthread_mutex_unlock(&control);
    }
    return __real_pthread_cond_wait(condition,lock);
}
static void control_wait(unsigned *value,unsigned wanted){pthread_mutex_lock(&control);while(*value<wanted)pthread_cond_wait(&control_changed,&control);pthread_mutex_unlock(&control);}
static void set_stage(unsigned value){pthread_mutex_lock(&control);stage=value;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);}
static void controlled_wake(void *opaque){
    wake(opaque);pthread_mutex_lock(&control);if(gate==2&&requester_identified&&pthread_equal(pthread_self(),requester_identity)){ticket_wake=1;pthread_cond_broadcast(&control_changed);}pthread_mutex_unlock(&control);
}
static int controlled_poll(void *opaque,FrostpunkAndroidEvent *out,int timeout){
    pthread_mutex_lock(&control);int tracked=track;if(tracked){
        ++poll_calls;++active_polls;if(active_polls>max_active_polls)max_active_polls=active_polls;
        if(!poll_threads){poll_threads=1;poll_identity=pthread_self();}else if(!pthread_equal(poll_identity,pthread_self()))poll_threads=2;
        if(timeout<0&&outer_hold){while(!outer_release)pthread_cond_wait(&control_changed,&control);}
    }
    int intercept=track&&gate==1&&timeout==0;
    if(intercept){gate=2;pthread_cond_broadcast(&control_changed);while(!allow_read)pthread_cond_wait(&control_changed,&control);}
    pthread_mutex_unlock(&control);
    int status=poll(opaque,out,timeout);
    pthread_mutex_lock(&control);
    if(tracked&&status>0&&out->type==FP_ANDROID_KEY)++keys_delivered;
    if(intercept){require(status==0,"gated final poll consumed the new wake but no input");gate=3;pthread_cond_broadcast(&control_changed);while(!allow_return)pthread_cond_wait(&control_changed,&control);gate=4;}
    if(tracked)--active_polls;pthread_mutex_unlock(&control);return status;
}
static uint32_t msgwait(unsigned timeout,unsigned mask){return(uint32_t)api("MsgWaitForMultipleObjects",5,V(0),V(0),V(0),V(timeout),V(mask));}
static void *peek_requester(void *unused){(void)unused;message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);
    pthread_mutex_lock(&control);requester_identity=pthread_self();requester_identified=1;pthread_mutex_unlock(&control);
    require(!peek(&msg,0,0,0,0),"ticket requester has no unrelated thread input");win64_region_remove((uintptr_t)&msg);
    pthread_mutex_lock(&control);helper_done=1;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);return NULL;
}
static void *owner(void *unused){(void)unused;owner_thread=pthread_self();
    if(negative_active){set_stage(1);(void)msgwait(UINT32_MAX,1);abort();}
    message msg;win64_region_add((uintptr_t)&msg,sizeof(msg),1);
    uint64_t hwnd=make_window(test_atom);require(hwnd!=0,"surface-backed test HWND");drain(&msg);
    require(msgwait(0,0)==258,"start and synchronize the sole host pump before racing a later ticket");
    pthread_mutex_lock(&control);track=1;gate=1;stage=1;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);
    controlled_wake(NULL);control_wait(&stage,3);
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp=='A',"racing new Peek ticket drained native key before completing");
    require(!peek(&msg,0,0,0,1),"first race queue drained");
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_B});
    require(msgwait(0,1)==0,"MsgWait timeout0 observes already queued native input");
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp=='B',"pending native input delivered to owner queue");
    require(msgwait(0,1)==258,"observed old input does not keep MsgWait signaled");
    pthread_mutex_lock(&control);kernel_wait_seen=0;stage=4;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);
    require(msgwait(UINT32_MAX,1)==0,"blocked kernel/message wait wakes from native input");
    require(peek(&msg,0,0,0,1)&&msg.message==0x100&&msg.wp=='C',"blocked wait returns actual native key");
    /* Queue cleanup on this native thread owns the last live HWND and event;
     * its destructor must release the surface and stop/join the host pump. */
    win64_region_remove((uintptr_t)&msg);set_stage(5);return NULL;
}
int main(int argc,char **argv){
    negative_active=argc>1&&!strcmp(argv[1],"active");
    if(negative_active){require(!pthread_create(&owner_thread,NULL,owner,NULL),"active-operation worker");control_wait(&kernel_wait_seen,1);win64_window_configure(NULL);abort();}
    uint16_t name[]={'M','s','g','W','a','i','t','H','o','s','t',0};win64_region_add((uintptr_t)name,sizeof(name),0);
    wc cls={.size=80,.proc=PROC1,.window_extra=16,.instance=0x140000000,.name=(uintptr_t)name};win64_region_add((uintptr_t)&cls,sizeof(cls),1);
    /* RegisterClass creates no queue, so owner cleanup really is the last. */
    test_atom=api("RegisterClassExW",1,V((uintptr_t)&cls));require(test_atom>=0xc000,"fixture class");
    interface.poll_event=controlled_poll;interface.wake=controlled_wake;host.available=1;
    win64_window_configure(&interface);event((FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=1});
    require(!pthread_create(&owner_thread,NULL,owner,NULL),"owner thread");control_wait(&gate,2);
    pthread_t requester;require(!pthread_create(&requester,NULL,peek_requester,NULL),"racing Peek requester");
    control_wait(&ticket_wake,1);pthread_mutex_lock(&control);allow_read=1;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);
    control_wait(&gate,3);
    pthread_mutex_lock(&host.mutex);require(host.wake_sequence==host.consumed_wake_sequence,"racing ticket host wake was actually consumed by final empty poll");pthread_mutex_unlock(&host.mutex);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_A});
    pthread_mutex_lock(&control);outer_hold=1;allow_return=1;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);
    control_wait(&helper_done,1);pthread_mutex_lock(&control);require(keys_delivered==1,"new generation cannot be acknowledged before its fresh drain");outer_release=1;stage=3;pthread_cond_broadcast(&control_changed);pthread_mutex_unlock(&control);
    require(!pthread_join(requester,NULL),"Peek requester cleanup");control_wait(&stage,4);control_wait(&kernel_wait_seen,1);
    event((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=AKEY_EVENT_ACTION_DOWN,.keycode=AKEYCODE_C});
    require(!pthread_join(owner_thread,NULL),"last queue cleanup joins owner and native pump");
    pthread_mutex_lock(&control);require(active_polls==0&&max_active_polls==1&&poll_threads==1&&keys_delivered==3,"single native event consumer and joined pump");unsigned final_calls=poll_calls;pthread_mutex_unlock(&control);
    require(host.window.references==1,"last queue cleanup releases its real host surface reference");
    win64_window_configure(NULL);controlled_wake(NULL);pthread_mutex_lock(&control);require(poll_calls==final_calls,"no host callback after joined pump/reconfigure");pthread_mutex_unlock(&control);
    require(api("UnregisterClassW",2,test_atom,V(0x140000000)),"last queue cleanup removes live HWND ownership");
    printf("{\"success\":true,\"checks\":%u,\"pending_native_input_timeout0\":true,\"capture_before_drain_ticket_race\":true,\"new_ticket_wake_consumed_before_input\":true,\"blocked_kernel_wait_native_input\":true,\"single_poll_thread\":true,\"max_concurrent_polls\":%u,\"native_keys_delivered\":%u,\"last_queue_cleanup_pump_join\":true,\"surface_refs_balanced\":true,\"activity_launched\":false,\"game_boot_proven\":false}\n",checks,max_active_polls,keys_delivered);return 0;
}
