#define _GNU_SOURCE
#define FP_GAME_RUNTIME
#include <android/looper.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../android/app/app.c"
static _Thread_local int client_thread;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed=PTHREAD_COND_INITIALIZER;
static int hold_poll,poll_held,release_poll,client_waiting;
static atomic_uint wakes;
static int observed_clockwait(pthread_cond_t*,pthread_mutex_t*,clockid_t,const struct timespec*);
#define pthread_cond_clockwait observed_clockwait
#include "../android/host/win64_window.c"
#undef pthread_cond_clockwait

static unsigned checks,publication_cases,parallel_calls;
static void check(int value,const char*message){++checks;if(!value){fprintf(stderr,"INPUT_EPOCH_FIXTURE_FAIL %s\n",message);abort();}}
void win64_set_error(uint32_t value){(void)value;abort();}
uint64_t win64_game_clock_ns(clockid_t clock){struct timespec t;if(clock_gettime(clock,&t))abort();return(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
void win64_kernel_queue_event_set(void*event,int signaled){(void)event;(void)signaled;abort();}
static int observed_clockwait(pthread_cond_t*condition,pthread_mutex_t*mutex,clockid_t clock,const struct timespec*deadline){
    if(client_thread){pthread_mutex_lock(&gate);client_waiting=1;pthread_cond_broadcast(&gate_changed);pthread_mutex_unlock(&gate);}
    return pthread_cond_clockwait(condition,mutex,clock,deadline);
}
static void observed_wake(void*opaque){atomic_fetch_add(&wakes,1);wake(opaque);}
static int observed_poll(void*opaque,FrostpunkAndroidEvent*event,int timeout){
    int status=poll_event(opaque,event,timeout);
    if(status>0){pthread_mutex_lock(&gate);if(hold_poll){poll_held=1;pthread_cond_broadcast(&gate_changed);while(!release_poll)pthread_cond_wait(&gate_changed,&gate);}pthread_mutex_unlock(&gate);}
    return status;
}
static void initialize(app_context*c,struct android_app*app,int legacy){
    memset(c,0,sizeof(*c));memset(app,0,sizeof(*app));app->looper=ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);check(app->looper!=NULL,"real Android looper");c->app=app;c->generation=1;
    check(!pthread_mutex_init(&c->mutex,NULL),"provider mutex");pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    check(!pthread_cond_init(&c->changed,&attr),"provider condition");pthread_condattr_destroy(&attr);
    c->api=(FrostpunkAndroidHost){.abi_version=1,.struct_size=sizeof(FrostpunkAndroidHost),.context=c,.acquire_window=acquire_window,.release_window=release_window,
        .poll_event=observed_poll,.wait=wait_event,.wake=observed_wake,.is_stopping=stopping,.event_sequence=event_sequence};
    if(legacy==1){c->api.struct_size=offsetof(FrostpunkAndroidHost,event_sequence);c->api.event_sequence=(void*)1;}
    if(legacy==2)c->api.event_sequence=NULL;
    win64_window_configure(&c->api);check(!pump_input_valid,"new provider has no stale checkpoint");
    memset(windows,0,sizeof(windows));memset(queues,0,sizeof(queues));window_count=1;queues[0].active=1;queues[0].tid=41;
    top_window=&windows[0];*top_window=(guest_window){.active=1,.visible=1,.focused=1,.width=640,.height=360,.surface_width=640,.surface_height=360,
        .native=(ANativeWindow*)(uintptr_t)0x1000,.generation=1,.queue=&queues[0]};host_focus=1;
    uint64_t consumed=c->consumed_wake_sequence;wake(c);
    uint64_t first=event_sequence(c),second=event_sequence(c);
    check(first==second&&c->consumed_wake_sequence==consumed,"checkpoint does not consume a latched explicit wake");
    check(start_pump(),"start sole input consumer");pump_events(0);
}
static void cleanup(app_context*c){
    pthread_mutex_lock(&mutex);top_window=NULL;windows[0].active=0;pthread_mutex_unlock(&mutex);
    win64_window_configure(NULL);check(!atomic_load(&pump_running)&&!pump_input_valid,"joined pump invalidates checkpoint");
    check(!pthread_mutex_destroy(&c->mutex),"destroy provider mutex");check(!pthread_cond_destroy(&c->changed),"destroy provider condition");
}
static void publish(app_context*c,unsigned code){push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_TEXT,.keycode=code});++publication_cases;}
static void wait_flag(int*flag,const char*message){
    struct timespec limit;clock_gettime(CLOCK_REALTIME,&limit);limit.tv_sec+=3;
    while(!*flag){int status=pthread_cond_timedwait(&gate_changed,&gate,&limit);check(status!=ETIMEDOUT,message);}
}
static void*one_client(void*opaque){client_thread=1;pump_events(0);atomic_store((atomic_int*)opaque,1);return NULL;}
static void held_publication(app_context*c,unsigned code,int stopping_case){
    pthread_mutex_lock(&gate);hold_poll=1;poll_held=release_poll=client_waiting=0;pthread_mutex_unlock(&gate);
    unsigned before=queues[0].count;publish(c,code);
    pthread_mutex_lock(&gate);wait_flag(&poll_held,"consumer popped the event");pthread_mutex_unlock(&gate);
    pthread_mutex_lock(&c->mutex);check(c->count==0,"event is in flight outside the native queue");pthread_mutex_unlock(&c->mutex);
    atomic_int done=0;pthread_t worker;check(!pthread_create(&worker,NULL,one_client,&done),"start barrier requester");
    pthread_mutex_lock(&gate);wait_flag(&client_waiting,"request waits for in-flight delivery");
    check(!atomic_load(&done),"empty native queue cannot bypass an in-flight publication");
    if(stopping_case){pthread_mutex_unlock(&gate);stop_game_worker(c);pthread_mutex_lock(&gate);}
    release_poll=1;hold_poll=0;pthread_cond_broadcast(&gate_changed);pthread_mutex_unlock(&gate);
    check(!pthread_join(worker,NULL)&&atomic_load(&done),"barrier requester completes");
    pthread_mutex_lock(&mutex);check(queues[0].count==before+1&&queues[0].messages[before].message==WM_CHAR&&queues[0].messages[before].wp==code,"held event delivered exactly once");
    if(stopping_case)check(host_stopping&&queues[0].quit,"real provider stop reaches the Win32 queue");pthread_mutex_unlock(&mutex);
}
static void*parallel_client(void*opaque){unsigned count=*(unsigned*)opaque;for(unsigned i=0;i<count;++i)pump_events(0);return NULL;}
int main(void){
    int enabled=1;
    check(offsetof(FrostpunkAndroidHost,event_sequence)==offsetof(FrostpunkAndroidHost,set_pointer_visible)+sizeof(((FrostpunkAndroidHost*)0)->set_pointer_visible),"ABI1 prefix is unchanged");
    app_context c;struct android_app app;initialize(&c,&app,0);
    unsigned begin=atomic_load(&wakes);for(unsigned i=0;i<128;++i)pump_events(0);
    check(atomic_load(&wakes)-begin==(enabled?0u:128u),"unchanged checkpoint avoids only redundant wakes");
    wake(&c);pump_events(0);
    for(unsigned i=0;i<128;++i){publish(&c,65+i);pump_events(0);pthread_mutex_lock(&mutex);
        check(queues[0].count==i+1&&queues[0].messages[i].message==WM_CHAR&&queues[0].messages[i].wp==65+i,"all earlier publications delivered in FIFO order");pthread_mutex_unlock(&mutex);}
    for(unsigned i=0;i<64;++i)held_publication(&c,900+i,0);
    unsigned count=1000;pthread_t workers[8];begin=atomic_load(&wakes);
    for(unsigned i=0;i<8;++i)check(!pthread_create(&workers[i],NULL,parallel_client,&count),"parallel reader create");
    for(unsigned i=0;i<8;++i)check(!pthread_join(workers[i],NULL),"parallel reader join");parallel_calls=8*count;
    check(atomic_load(&wakes)-begin==(enabled?0u:parallel_calls),"parallel unchanged readers preserve drain semantics");
    held_publication(&c,901,1);cleanup(&c);
    for(int legacy=1;legacy<=2;++legacy){initialize(&c,&app,legacy);begin=atomic_load(&wakes);for(unsigned i=0;i<32;++i)pump_events(0);
        check(atomic_load(&wakes)-begin==32,"old/NULL-tail provider retains the full barrier");publish(&c,700+legacy);pump_events(0);
        pthread_mutex_lock(&mutex);check(queues[0].count==1&&queues[0].messages[0].wp==(unsigned)(700+legacy),"legacy event delivery");pthread_mutex_unlock(&mutex);cleanup(&c);}
    printf("{\"success\":true,\"enabled\":%d,\"checks\":%u,\"publication_cases\":%u,\"parallel_calls\":%u,\"in_flight_publications\":65,\"real_provider_stop\":true,\"legacy_provider_cases\":2,\"checkpoint_nonconsuming\":true}\n",enabled,checks,publication_cases,parallel_calls);return 0;
}
