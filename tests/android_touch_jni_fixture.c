#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <jni.h>
#include <android/input.h>
#include <dlfcn.h>
#include "host_api.h"
typedef struct {int stopped,stopping;void *runtime_handle;} app_context;
static app_context *current;
static pthread_mutex_t current_mutex=PTHREAD_MUTEX_INITIALIZER;
static FrostpunkAndroidEvent last;
static unsigned pushed;
static int stopping(app_context *c){return c->stopped;}
static int native_editor_active(app_context *c){assert(c==current);return 0;}
static void push(app_context *c,FrostpunkAndroidEvent event){assert(c==current);last=event;++pushed;}
#include "touch_jni_production.inc"
#define pointer(b,x,y,w) Java_com_frostpunk_staticrecomp_TouchControls_nativePointer(NULL,NULL,b,x,y,w)
#define key(k,d) Java_com_frostpunk_staticrecomp_TouchControls_nativeKey(NULL,NULL,k,d)
int main(void){
    app_context c={0};assert(!pointer(1,.5f,.5f,0));assert(!key(AKEYCODE_SPACE,1));
    current=&c;
    for(int b=0;b<8;++b){assert(pointer(b,.25f,.75f,0));assert(last.type==FP_ANDROID_MOTION&&last.source==AINPUT_SOURCE_MOUSE&&last.buttons==(unsigned)b);
        assert(last.action==AMOTION_EVENT_ACTION_MOVE&&last.x==.25f&&last.y==.75f&&last.width==1&&last.height==1&&last.time_ns>0);}
    assert(pointer(0,0,1,8)&&last.action==AMOTION_EVENT_ACTION_SCROLL&&last.scroll_y==8);
    assert(pointer(0,1,0,-8)&&last.scroll_y==-8);
    unsigned before=pushed;
    assert(!pointer(-1,.5f,.5f,0)&&!pointer(8,.5f,.5f,0)&&!pointer(0,NAN,.5f,0)&&!pointer(0,.5f,INFINITY,0));
    assert(!pointer(0,-.01f,.5f,0)&&!pointer(0,.5f,1.01f,0)&&!pointer(0,.5f,.5f,NAN)&&!pointer(0,.5f,.5f,9));
    assert(!key(AKEYCODE_VOLUME_UP,1)&&!key(-1,0)&&!key(10000,1));assert(pushed==before);
    int keys[]={AKEYCODE_SPACE,AKEYCODE_1,AKEYCODE_2,AKEYCODE_3,AKEYCODE_B,AKEYCODE_V,AKEYCODE_Q,AKEYCODE_E,AKEYCODE_ESCAPE};
    for(unsigned i=0;i<sizeof(keys)/sizeof(*keys);++i)for(int down=0;down<2;++down){
        assert(key(keys[i],down));assert(last.type==FP_ANDROID_KEY&&last.keycode==(unsigned)keys[i]&&last.source==AINPUT_SOURCE_KEYBOARD);
        assert(last.action==(unsigned)(down?AKEY_EVENT_ACTION_DOWN:AKEY_EVENT_ACTION_UP));}
    c.stopped=1;before=pushed;assert(!pointer(0,.5f,.5f,0)&&!key(AKEYCODE_SPACE,1)&&pushed==before);
    c.stopped=0;
    assert(Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(NULL,NULL,AMOTION_EVENT_ACTION_SCROLL,9,AMETA_CTRL_ON,.2f,.3f,-2,3));
    assert(last.buttons==9&&last.meta==AMETA_CTRL_ON&&last.scroll_x==-2&&last.scroll_y==3&&last.x==.2f);
    before=pushed;
    assert(!Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(NULL,NULL,99,0,0,0,0,0,0));
    assert(!Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(NULL,NULL,2,-1,0,0,0,0,0));
    assert(!Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(NULL,NULL,2,0,0,NAN,0,0,0));
    assert(!Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(NULL,NULL,2,0,0,0,0,274,0));
    assert(pushed==before);
    current=NULL;assert(!pointer(0,.5f,.5f,0));
    puts("{\"success\":true,\"pointer_profiles\":10,\"key_events\":18,\"invalid_inputs_rejected\":15,\"physical_mouse_metadata\":true,\"lifecycle\":true}");
}
