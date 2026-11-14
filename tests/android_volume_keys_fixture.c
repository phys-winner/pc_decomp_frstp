#include <assert.h>
#include <stdio.h>
#include <android/input.h>
#include <android_native_app_glue.h>
#include "host_api.h"
struct AInputEvent {int32_t code,action,repeat;};
int32_t AKeyEvent_getKeyCode(const AInputEvent *e){return e->code;}
int32_t AKeyEvent_getAction(const AInputEvent *e){return e->action;}
int32_t AKeyEvent_getRepeatCount(const AInputEvent *e){return e->repeat;}
int32_t AKeyEvent_getScanCode(const AInputEvent *e){(void)e;return 91;}
int32_t AKeyEvent_getMetaState(const AInputEvent *e){(void)e;return 4;}
int64_t AKeyEvent_getEventTime(const AInputEvent *e){(void)e;return 123456789;}
int32_t AInputEvent_getSource(const AInputEvent *e){(void)e;return AINPUT_SOURCE_KEYBOARD;}
typedef struct {int unused;} app_context;
static unsigned pushed,finished;
static FrostpunkAndroidEvent last;
static void push(app_context *c,FrostpunkAndroidEvent event){(void)c;last=event;++pushed;}
void ANativeActivity_finish(ANativeActivity *activity){(void)activity;++finished;}
static int key_input(struct android_app *app,AInputEvent *native){
    app_context *c=app->userData;int type=AINPUT_EVENT_TYPE_KEY;
#include "volume_key_branch.inc"
    return 0;
}
int main(void){
    app_context context={0};struct android_app app={0};app.userData=&context;unsigned cases=0;
    for(int code=0;code<=300;++code)for(int action=0;action<=1;++action)for(int repeat=0;repeat<3;++repeat){
        AInputEvent event={code,action,repeat};pushed=finished=0;
        int volume=code==AKEYCODE_VOLUME_UP||code==AKEYCODE_VOLUME_DOWN||code==AKEYCODE_VOLUME_MUTE;
        assert(key_input(&app,&event)==!volume);
        assert(pushed==(unsigned)!volume);
        assert(finished==(unsigned)(code==AKEYCODE_BACK&&action==AKEY_EVENT_ACTION_UP));
        if(!volume)assert(last.type==FP_ANDROID_KEY&&last.keycode==(unsigned)code&&last.action==(unsigned)action&&
            last.repeat==(unsigned)repeat&&last.scancode==91&&last.meta==4&&last.time_ns==123456789&&last.source==AINPUT_SOURCE_KEYBOARD);
        ++cases;
    }
    printf("{\"success\":true,\"cases\":%u,\"volume_events_passed_to_android\":18,\"game_key_events_preserved\":1788}\n",cases);
}
