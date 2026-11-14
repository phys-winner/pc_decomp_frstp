#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {int refs,width,height;} ANativeWindow;
typedef struct {unsigned type;int width,height;} FrostpunkAndroidEvent;
typedef struct {int mutex;ANativeWindow *window;uint64_t generation;int physical_width,physical_height;} app_context;
enum {FP_ANDROID_WINDOW=1};
static app_context state;
static unsigned votes,events;
static FrostpunkAndroidEvent last_event;
static int pthread_mutex_lock(int *p){assert(!*p);*p=1;return 0;}
static int pthread_mutex_unlock(int *p){assert(*p);*p=0;return 0;}
static void ANativeWindow_acquire(ANativeWindow *w){assert(state.mutex&&w->refs);++w->refs;}
static void ANativeWindow_release(ANativeWindow *w){assert(!state.mutex&&w->refs>=2);--w->refs;}
static int ANativeWindow_getWidth(ANativeWindow *w){assert(!state.mutex&&w->refs>=2);return w->width;}
static int ANativeWindow_getHeight(ANativeWindow *w){assert(!state.mutex&&w->refs>=2);return w->height;}
static void request_window_frame_rate(ANativeWindow *w,uint64_t generation){
    assert(!state.mutex&&state.window==w&&state.generation==generation&&w->refs>=2);++votes;
}
static void push(app_context *c,FrostpunkAndroidEvent event){assert(!c->mutex);++events;last_event=event;}
#include "window_resize.inc"

int main(void){
    ANativeWindow a={1,2944,1840},b={1,1840,2944};
    set_window(&state,&a);assert(state.generation==1&&a.refs==2&&votes==1);
    for(unsigned i=0;i<1024;++i){
        a.width=i&1?1472:2944;a.height=i&1?920:1840;
        resize_window(&state,&a);
        assert(state.generation==1&&votes==1&&a.refs==2&&last_event.width==a.width&&last_event.height==a.height);
        assert(state.physical_width==a.width&&state.physical_height==a.height);
    }
    resize_window(&state,&b);assert(state.generation==2&&votes==2&&a.refs==1&&b.refs==2);
    set_window(&state,0);assert(state.generation==3&&b.refs==1&&votes==2);
    resize_window(&state,0);assert(state.generation==3&&!last_event.width&&!last_event.height);
    resize_window(&state,&a);assert(state.generation==4&&a.refs==2&&votes==3);
    set_window(&state,&a);assert(state.generation==5&&a.refs==2&&votes==4);
    set_window(&state,0);assert(state.generation==6&&a.refs==1&&b.refs==1&&!state.window);
    assert(events==1031);
    puts("{\"success\":true,\"same_surface_resizes\":1024,\"generations\":6,\"events\":1031,\"frame_rate_votes\":4,\"ownership_balanced\":true}");
}
