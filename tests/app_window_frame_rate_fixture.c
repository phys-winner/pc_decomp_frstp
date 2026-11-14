/* Exact candidate app lifecycle function with instrumented window/API ownership. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
typedef struct {int refs,width,height,id;} ANativeWindow;
typedef struct {unsigned type;int width,height;} FrostpunkAndroidEvent;
enum {FP_ANDROID_WINDOW=1,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT=0};
typedef struct {int mutex;ANativeWindow *window;uint64_t generation;int physical_width,physical_height;} app_context;
static unsigned lifecycle_checks,votes,event_count,log_count;static int api_result;
static app_context *observed;static ANativeWindow *expected_window;static uint64_t expected_generation;
static char last_log[192];static FrostpunkAndroidEvent last_event;
static void check(int valid,const char *why){if(!valid){fprintf(stderr,"WINDOW_GENERATION_FAIL %s\n",why);exit(1);}++lifecycle_checks;}
static int pthread_mutex_lock(int *mutex){check(!*mutex,"lock not recursive");*mutex=1;return 0;}
static int pthread_mutex_unlock(int *mutex){check(*mutex,"unlock owned");*mutex=0;return 0;}
static void ANativeWindow_acquire(ANativeWindow *w){check(observed->mutex&&w->refs>0,"acquire while original owner live");++w->refs;}
static void ANativeWindow_release(ANativeWindow *w){check(!observed->mutex&&w->refs>=2,"release old reference outside lock");--w->refs;}
static int ANativeWindow_getWidth(ANativeWindow *w){check(!observed->mutex&&w->refs>=2,"width reads live published window");return w->width;}
static int ANativeWindow_getHeight(ANativeWindow *w){check(!observed->mutex&&w->refs>=2,"height reads live published window");return w->height;}
static int ANativeWindow_setFrameRate(ANativeWindow *w,float rate,int8_t compatibility){
    check(!observed->mutex,"frame vote outside app mutex");
    check(w==expected_window&&w==observed->window&&w->refs>=2,"vote holds new published window reference");
    check(observed->generation==expected_generation,"vote uses this exact window generation");
    check(rate==120.0f&&compatibility==0,"intended120Hz default game compatibility");
    /* A reentrant read can obtain the app mutex; the vote does not hold it. */
    pthread_mutex_lock(&observed->mutex);check(observed->window==w,"reentrant window publication visible");pthread_mutex_unlock(&observed->mutex);
    ++votes;return api_result;
}
static void log_text(const char *s){check(!observed->mutex,"request diagnostic outside lock");snprintf(last_log,sizeof(last_log),"%s",s);++log_count;}
static void push(app_context *c,FrostpunkAndroidEvent e){check(!c->mutex,"existing event push remains outside lock");last_event=e;++event_count;}
#include "window_generation.inc"
int display_matrix_main(int,char **);
static void transition(app_context *c,ANativeWindow *w,int result){
    unsigned old_votes=votes,old_logs=log_count,old_events=event_count;api_result=result;expected_window=w;expected_generation=c->generation+1;
    set_window(c,w);check(c->generation==expected_generation&&!c->mutex&&c->window==w,"published generation/state retained");
    check(event_count==old_events+1&&last_event.type==FP_ANDROID_WINDOW,"one original window event per generation");
    check(last_event.width==(w?w->width:0)&&last_event.height==(w?w->height:0),"event keeps full physical dimensions");
#ifdef FP_GAME_RUNTIME
    check(votes==old_votes+(w!=NULL)&&log_count==old_logs+(w!=NULL),"one request and diagnostic per nonNULL generation");
    if(w){char value[32];snprintf(value,sizeof(value),"result=%d",result);check(strstr(last_log,value)&&strstr(last_log,"physical_mode_proven=0"),"success/failure reported without selected-mode claim");}
#else
    check(votes==old_votes&&log_count==old_logs,"legacy probe build has no new request/API side effect");
#endif
    if(w)check(c->physical_width==w->width&&c->physical_height==w->height,"physical geometry unchanged by request");
}
int main(int argc,char **argv){
    if(argc!=1)return display_matrix_main(argc,argv);
    check(display_matrix_main(argc,argv)==0,"adapted production display matrices pass");
    app_context context={0};observed=&context;ANativeWindow a={1,2944,1840,1},b={1,1840,2944,2};
    transition(&context,&a,0);check(a.refs==2,"initial surface kept alive");
    transition(&context,&b,-22);check(a.refs==1&&b.refs==2,"failure leaves new surface usable, old reference released");
    transition(&context,NULL,0);check(a.refs==1&&b.refs==1,"teardown drops own surface references");
    transition(&context,&a,0);transition(&context,&a,0);check(a.refs==2,"same pointer new generation neither leaks nor loses owner");
    for(unsigned i=0;i<256;++i){transition(&context,i&1?&a:&b,i%3?-22:0);transition(&context,NULL,0);check(a.refs==1&&b.refs==1,"recreation retries failure without lifetime leak");}
    check(!context.window,"all ownership returned at teardown");
#ifdef FP_GAME_RUNTIME
    check(votes==260&&log_count==260,"all260 nonNULL generations voted exactly once");
#else
    check(votes==0&&log_count==0,"all260 nonNULL probe generations unchanged");
#endif
    printf("{\"success\":true,\"lifecycle_checks\":%u,\"nonNULL_generations\":260,\"window_generations\":517,\"votes\":%u,\"events\":%u,\"failure_reacquire_cycles\":256,\"requested_Hz\":120,\"mock_window_API\":true,\"physical_mode_proven\":false,\"fps90_proven\":false}\n",lifecycle_checks,votes,event_count);return 0;
}
