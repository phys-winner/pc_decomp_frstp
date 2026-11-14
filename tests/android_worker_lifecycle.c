/* Exercise the production app worker/stop functions; no game or Activity.
 * Dead sections of included app.c are removed by --gc-sections. */
#define _GNU_SOURCE
#define FP_GAME_RUNTIME
#include <android/looper.h>
#include <pthread.h>
#include <stdatomic.h>
static int observed_cond_wait(pthread_cond_t *condition,pthread_mutex_t *mutex);
static void observed_looper_wake(ALooper *looper);
#define pthread_cond_wait observed_cond_wait
#define ALooper_wake observed_looper_wake
#include "../android/app/app.c"
#undef pthread_cond_wait
#undef ALooper_wake
#include <sys/wait.h>

static pthread_cond_t wait_entered=PTHREAD_COND_INITIALIZER;
static pthread_cond_t *watched_condition;static unsigned observed_waits;
static atomic_uint observed_looper_wakes;
static void observed_looper_wake(ALooper *looper) {
    if(!looper)abort();
    atomic_fetch_add_explicit(&observed_looper_wakes,1,memory_order_relaxed);
    ALooper_wake(looper);
}
/* Observe the production transition immediately before the real atomic
 * mutex-release+condition-wait; this does not replace condition behavior. */
static int observed_cond_wait(pthread_cond_t *condition,pthread_mutex_t *mutex) {
    if(condition==watched_condition){++observed_waits;pthread_cond_broadcast(&wait_entered);}
    return pthread_cond_wait(condition,mutex);
}

static void initialize(app_context *c,struct android_app *app) {
    memset(c,0,sizeof(*c));memset(app,0,sizeof(*app));app->looper=ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);if(!app->looper)abort();c->app=app;
    pthread_mutex_init(&c->mutex,NULL);pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);
    pthread_cond_init(&c->changed,&attr);pthread_condattr_destroy(&attr);c->api.context=c;c->api.is_stopping=stopping;c->api.poll_event=poll_event;c->api.wake=wake;
}
static void destroy(app_context *c) {pthread_mutex_destroy(&c->mutex);pthread_cond_destroy(&c->changed);}
static void *cooperative_worker(void *opaque) {
    app_context *c=opaque;FrostpunkAndroidEvent event;
    int status=poll_event(c,&event,-1);
    if(status!=-1||!stopping(c))abort();
    pthread_mutex_lock(&c->mutex);c->worker_result=17;c->worker_finished=1;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);return NULL;
}
static void *noncooperative_worker(void *opaque) {(void)opaque;for(;;)pause();return NULL;}
static uint64_t now_ns(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
typedef struct {app_context *context;int use_wait,status;FrostpunkAndroidEvent event;} wake_case;
static void *wake_consumer(void *opaque) {wake_case *c=opaque;c->status=c->use_wait?wait_event(c->context,-1):poll_event(c->context,&c->event,-1);return NULL;}
static unsigned wake_latch_test(void) {
    app_context context;struct android_app app;initialize(&context,&app);FrostpunkAndroidEvent event={0};unsigned checks=0;
    context.api.wake(context.api.context);wake(&context);wake(&context);uint64_t begin=now_ns();
    if(poll_event(&context,&event,-1)!=0||now_ns()-begin>100000000ull||context.consumed_wake_sequence!=context.wake_sequence)abort();++checks;
    if(wait_event(&context,0)!=0)abort();++checks;wake(&context);if(wait_event(&context,-1)!=1||wait_event(&context,0)!=0)abort();++checks;
    push(&context,(FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.keycode=17});push(&context,(FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.pointer_id=23});wake(&context);
    if(wait_event(&context,0)!=1||poll_event(&context,&event,-1)!=1||event.type!=FP_ANDROID_KEY||event.keycode!=17)abort();++checks;
    if(poll_event(&context,&event,-1)!=1||event.type!=FP_ANDROID_MOTION||event.pointer_id!=23||wait_event(&context,0)!=0)abort();++checks;
    /* Preserve existing contract: poll drains queued events before -1 stop;
     * wait reports stop immediately without consuming the queued event. */
    push(&context,(FrostpunkAndroidEvent){.type=FP_ANDROID_DESTROY});pthread_mutex_lock(&context.mutex);context.stopping=1;pthread_mutex_unlock(&context.mutex);
    if(wait_event(&context,-1)!=-1||poll_event(&context,&event,-1)!=1||event.type!=FP_ANDROID_DESTROY||poll_event(&context,&event,-1)!=-1)abort();++checks;
    pthread_mutex_lock(&context.mutex);context.stopping=0;pthread_mutex_unlock(&context.mutex);
    for(unsigned use_wait=0;use_wait<2;++use_wait){
        wake_case c={.context=&context,.use_wait=(int)use_wait};pthread_t thread;watched_condition=&context.changed;observed_waits=0;
        pthread_mutex_lock(&context.mutex);if(pthread_create(&thread,NULL,wake_consumer,&c))abort();
        while(!observed_waits)pthread_cond_wait(&wait_entered,&context.mutex);pthread_mutex_unlock(&context.mutex);
        wake(&context);if(pthread_join(thread,NULL)||c.status!=(use_wait?1:0)||wait_event(&context,0)!=0)abort();++checks;watched_condition=NULL;
    }
    for(unsigned i=0;i<512;++i){wake_case c={.context=&context,.use_wait=(int)(i&1)};pthread_t thread;
        if(i&2)wake(&context);if(pthread_create(&thread,NULL,wake_consumer,&c))abort();if(!(i&2))wake(&context);
        if(pthread_join(thread,NULL)||c.status!=(c.use_wait?1:0)||wait_event(&context,0)!=0)abort();++checks;
    }
    if(atomic_load_explicit(&observed_looper_wakes,memory_order_relaxed)!=0)abort();
    destroy(&context);return checks;
}
int main(void) {
    unsigned wake_checks=wake_latch_test();
    app_context manifest_context={0};snprintf(manifest_context.game,sizeof(manifest_context.game),"/data/local/tmp/frostpunk/worker-manifest-%d",getpid());
    if(mkdir(manifest_context.game,0700))abort();int root=open(manifest_context.game,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)abort();
    int file=openat(root,"animations.dat",O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(file<0||write(file,"test",4)!=4||close(file))abort();
    char lf[]="4\tanimations.dat\n",crlf[]="4\tanimations.dat\r\n",bad_size[]="3\tanimations.dat\r\n",bad_file[]="4\tmissing.dat\n";
    if(!game_inputs_check_text(&manifest_context,root,lf)||!game_inputs_check_text(&manifest_context,root,crlf)||game_inputs_check_text(&manifest_context,root,bad_size)||game_inputs_check_text(&manifest_context,root,bad_file))abort();
    if(unlinkat(root,"animations.dat",0)||close(root)||rmdir(manifest_context.game))abort();
    for(unsigned i=0;i<64;++i){app_context context;struct android_app app;initialize(&context,&app);context.worker_started=1;
        if(pthread_create(&context.worker,NULL,cooperative_worker,&context))abort();stop_game_worker(&context);
        if(!context.worker_finished||context.worker_result!=17)abort();destroy(&context);}
    unsigned cooperative_wakes=atomic_load_explicit(&observed_looper_wakes,memory_order_relaxed);if(cooperative_wakes!=64)abort();
    app_context context;struct android_app app;initialize(&context,&app);
    snprintf(context.game,sizeof(context.game),"/proc/frostpunk-nonexistent-game-inputs");
    if(game_inputs_present(&context)||!strstr(context.error,"Missing game directory"))abort();
    context.error[0]=0;snprintf(context.libraries,sizeof(context.libraries),"/proc/frostpunk-nonexistent-runtime");context.worker_started=1;
    if(pthread_create(&context.worker,NULL,game_worker,&context))abort();stop_game_worker(&context);
    if(!context.worker_finished||context.worker_result!=74||context.runtime_loaded||!strstr(context.error,"Cannot load native game runtime"))abort();destroy(&context);
    unsigned finish_stop_wakes=atomic_load_explicit(&observed_looper_wakes,memory_order_relaxed)-cooperative_wakes;if(finish_stop_wakes!=2)abort();
    uint64_t begin=now_ns();pid_t child=fork();if(child<0)abort();
    if(!child){initialize(&context,&app);context.worker_started=1;if(pthread_create(&context.worker,NULL,noncooperative_worker,&context))_exit(1);stop_game_worker(&context);_exit(2);}
    int status=0;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status)!=74)abort();
    uint64_t elapsed=now_ns()-begin;if(elapsed<1800000000ull||elapsed>5000000000ull)abort();
    printf("{\"success\":true,\"host_wake_latch_checks\":%u,\"runtime_API_looper_wakes\":0,\"explicit_cooperative_stop_looper_wakes\":%u,\"explicit_worker_finish_and_stop_looper_wakes\":%u,\"real_libandroid_wake_observed\":true,\"prewake_poll_infinite\":true,\"coalesced_consumed_wake\":true,\"real_wait_transition_cases\":2,\"prewake_duringwait_races\":512,\"queued_event_stop_priority_preserved\":true,\"single_host_event_consumer\":true,\"manifest_LF_CRLF_real_file_checks\":true,\"incorrect_size_and_missing_file_rejected\":true,\"production_worker_join_cases\":64,\"missing_game_directory_rejected\":true,\"missing_runtime_rejected\":true,\"stuck_worker_process_exit\":74,\"bounded_stop_milliseconds\":%" PRIu64 ",\"activity_launched\":false,\"game_boot_proven\":false}\n",wake_checks,cooperative_wakes,finish_stop_wakes,elapsed/1000000);return 0;
}
