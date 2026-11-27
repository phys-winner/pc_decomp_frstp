#define _GNU_SOURCE
#include "host_api.h"
#include "../host/zip_files.h"
#include "window_probe.h"
#include "../gpu/driver.h"
#include <android/asset_manager.h>
#include <android/input.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <jni.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#define EVENTS 1024u
#ifdef FP_GAME_RUNTIME
#ifndef FP_ORIGINAL_EXE_SHA256
#error FP_ORIGINAL_EXE_SHA256 must pin the original PE before process startup
#endif
#define FIXTURE_NAME "static-recompiled-original-game-worker"
#define FIXTURE_TITLE "Frostpunk static recompilation"
#elif defined(FP_DXVK_FIXTURE)
#define FIXTURE_NAME "offline-lifted-win64-com-dxvk-android-present"
#define FIXTURE_TITLE "Frostpunk Win64 COM / DXVK present probe"
#else
#define FIXTURE_NAME "native-window-input-vulkan"
#define FIXTURE_TITLE "Frostpunk native window probe"
#endif
typedef struct {
    struct android_app *app;FrostpunkAndroidHost api;pthread_mutex_t mutex;pthread_cond_t changed;
    ANativeWindow *window;uint64_t generation,wake_sequence,consumed_wake_sequence;unsigned head,count;int stopping,destroyed,focused,overflow;
    FrostpunkAndroidEvent events[EVENTS];PFN_vkGetInstanceProcAddr provider;window_probe *probe;
    char private_path[PATH_MAX],game[PATH_MAX],profile[PATH_MAX],libraries[PATH_MAX],driver[PATH_MAX],error[512];
    int physical_width,physical_height,paused;unsigned touches,keys,consumed,acquire_checks;uint64_t total_frames;float x,y;
    pthread_t worker;int worker_started,worker_finished,worker_result,runtime_loaded;void *runtime_handle;
#ifdef FP_GAME_RUNTIME
    char reported_game_state[512];
#endif
#ifdef FP_POINTER_FIXTURE
    int pointer_fixture_visible;
#endif
} app_context;
static app_context *current;
static pthread_mutex_t current_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t foreground_changed=PTHREAD_COND_INITIALIZER;
static void log_text(const char *text){__android_log_print(ANDROID_LOG_INFO,"FrostpunkProbe","%s",text);fprintf(stderr,"%s\n",text);}
static void notify_foreground(void){
    pthread_mutex_lock(&current_mutex);pthread_cond_broadcast(&foreground_changed);pthread_mutex_unlock(&current_mutex);
}
static void set_game_paused(app_context *c,int paused) {
    pthread_mutex_lock(&c->mutex);c->paused=paused;
    void (*apply)(int)=c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_android_set_paused"):NULL;
    if(apply)apply(paused);pthread_mutex_unlock(&c->mutex);
    notify_foreground();
}
__attribute__((visibility("default"))) int frostpunk_android_timezone_offset(int64_t seconds,int32_t *offset,int32_t *is_dst) {
    if(!offset || !is_dst || seconds>INT64_MAX/1000 || seconds<INT64_MIN/1000)return 0;
    pthread_mutex_lock(&current_mutex);app_context *c=current;int ok=0,attached=0;JNIEnv *env=NULL;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL,date_cls=NULL;jobject zone=NULL,date=NULL;
    if(!vm)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->FindClass(env,"java/util/TimeZone");if(!cls)goto done;
    jmethodID get=(*env)->GetStaticMethodID(env,cls,"getDefault","()Ljava/util/TimeZone;");
    jmethodID value=(*env)->GetMethodID(env,cls,"getOffset","(J)I");if(!get||!value)goto done;
    zone=(*env)->CallStaticObjectMethod(env,cls,get);if(!zone||(*env)->ExceptionCheck(env))goto done;
    jint milliseconds=(*env)->CallIntMethod(env,zone,value,(jlong)(seconds*1000));
    if((*env)->ExceptionCheck(env))goto done;
    date_cls=(*env)->FindClass(env,"java/util/Date");if(!date_cls)goto done;
    jmethodID create=(*env)->GetMethodID(env,date_cls,"<init>","(J)V");
    jmethodID daylight=(*env)->GetMethodID(env,cls,"inDaylightTime","(Ljava/util/Date;)Z");if(!create||!daylight)goto done;
    date=(*env)->NewObject(env,date_cls,create,(jlong)(seconds*1000));if(!date||(*env)->ExceptionCheck(env))goto done;
    jboolean dst=(*env)->CallBooleanMethod(env,zone,daylight,date);
    if(!(*env)->ExceptionCheck(env)){*offset=milliseconds/1000;*is_dst=dst==JNI_TRUE;ok=1;}
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(date)(*env)->DeleteLocalRef(env,date);if(date_cls)(*env)->DeleteLocalRef(env,date_cls);if(zone)(*env)->DeleteLocalRef(env,zone);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);return ok;
}
__attribute__((visibility("default"))) uint32_t frostpunk_android_frame_rates(uint32_t *rates,uint32_t capacity){
    if(!rates||!capacity)return 0;
    pthread_mutex_lock(&current_mutex);app_context *c=current;JNIEnv *env=NULL;int attached=0;uint32_t count=0;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL;jintArray result=NULL;
    if(!vm||c->stopping)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,"getFrameRates","()[I");if(!method)goto done;
    result=(jintArray)(*env)->CallObjectMethod(env,c->app->activity->clazz,method);
    if(!result||(*env)->ExceptionCheck(env))goto done;
    jsize size=(*env)->GetArrayLength(env,result);if(size<2||size>33||(uint32_t)(size-1)>capacity)goto done;
    jint values[33];(*env)->GetIntArrayRegion(env,result,0,size,values);if((*env)->ExceptionCheck(env))goto done;
    for(jsize i=1;i<size;++i)if(values[i]<=0||values[i]>1000||(i>1&&values[i]<=values[i-1]))goto done;
    count=(uint32_t)(size-1);for(uint32_t i=0;i<count;++i)rates[i]=(uint32_t)values[i+1];
    char line[512];int used=snprintf(line,sizeof(line),"ANDROID_FRAME_RATES current=%d modes=",values[0]);
    for(uint32_t i=0;i<count&&used>0&&(size_t)used<sizeof(line);++i)used+=snprintf(line+used,sizeof(line)-(size_t)used,"%s%u",i?",":"",rates[i]);
    log_text(line);
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(result)(*env)->DeleteLocalRef(env,result);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);return count;
}
#ifdef FP_GAME_RUNTIME
__attribute__((visibility("default"))) void frostpunk_android_request_frame_rate(uint32_t hz){
    if(!hz||hz>1000)return;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    if(c){pthread_mutex_lock(&c->mutex);ANativeWindow *window=c->window;if(window)ANativeWindow_acquire(window);pthread_mutex_unlock(&c->mutex);
        if(window){ANativeWindow_setFrameRate(window,(float)hz,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);ANativeWindow_release(window);}}
    pthread_mutex_unlock(&current_mutex);
}
#endif
__attribute__((visibility("default"))) void frostpunk_android_exiting(uint32_t code){
    pthread_mutex_lock(&current_mutex);app_context *c=current;JNIEnv *env=NULL;int attached=0;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL;if(!vm)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,"onNativeExiting","(I)V");if(method)(*env)->CallVoidMethod(env,c->app->activity->clazz,method,(jint)code);
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);
}
__attribute__((visibility("default"))) void frostpunk_android_show_help(const uint16_t *title,uint32_t title_len,const uint16_t *body,uint32_t body_len,const uint16_t *close,uint32_t close_len){
    if(!title||!body||!close||!title_len||!body_len||!close_len||title_len>8192||body_len>8192||close_len>8192)return;
    pthread_mutex_lock(&current_mutex);app_context *c=current;JNIEnv *env=NULL;int attached=0;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL;jstring strings[3]={NULL,NULL,NULL};
    if(!vm||c->stopping)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,"showGestureHelp","(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");if(!method)goto done;
    strings[0]=(*env)->NewString(env,(const jchar*)title,(jsize)title_len);
    if(!strings[0]||(*env)->ExceptionCheck(env))goto done;
    strings[1]=(*env)->NewString(env,(const jchar*)body,(jsize)body_len);
    if(!strings[1]||(*env)->ExceptionCheck(env))goto done;
    strings[2]=(*env)->NewString(env,(const jchar*)close,(jsize)close_len);
    if(strings[0]&&strings[1]&&strings[2]&&!(*env)->ExceptionCheck(env))
        (*env)->CallVoidMethod(env,c->app->activity->clazz,method,strings[0],strings[1],strings[2]);
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);for(unsigned i=0;i<3;++i)if(strings[i])(*env)->DeleteLocalRef(env,strings[i]);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);
}
__attribute__((visibility("default"))) int frostpunk_android_touch_text(const uint16_t *loading,uint32_t loading_len,const uint16_t *description,uint32_t description_len){
    if(!loading||!description||!loading_len||!description_len||loading_len>8192||description_len>8192)return 0;
    pthread_mutex_lock(&current_mutex);app_context *c=current;JNIEnv *env=NULL;int attached=0,ok=0;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL;jstring a=NULL,b=NULL;
    if(!vm||c->stopping)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,"localizeTouchControls","(Ljava/lang/String;Ljava/lang/String;)V");if(!method)goto done;
    a=(*env)->NewString(env,(const jchar*)loading,(jsize)loading_len);if(!a||(*env)->ExceptionCheck(env))goto done;
    b=(*env)->NewString(env,(const jchar*)description,(jsize)description_len);if(!b||(*env)->ExceptionCheck(env))goto done;
    (*env)->CallVoidMethod(env,c->app->activity->clazz,method,a,b);ok=!(*env)->ExceptionCheck(env);
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(a)(*env)->DeleteLocalRef(env,a);if(b)(*env)->DeleteLocalRef(env,b);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);return ok;
}
static void set_status(void *opaque,const char *text) {
    app_context *c=opaque;JNIEnv *env=NULL;JavaVM *vm=c->app->activity->vm;int attached=0;
    if((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK)return;attached=1;}
    if(!env)return;jclass cls=(*env)->GetObjectClass(env,c->app->activity->clazz);
    jmethodID method=(*env)->GetMethodID(env,cls,"showNativeStatus","(Ljava/lang/String;)V");
    if(method){jstring message=(*env)->NewStringUTF(env,text);(*env)->CallVoidMethod(env,c->app->activity->clazz,method,message);(*env)->DeleteLocalRef(env,message);}
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->DeleteLocalRef(env,cls);if(attached)(*vm)->DetachCurrentThread(vm);
}
/* Intervals between presented unique game frames for the launcher's FPS overlay (FpsOverlay.Source).
 * The runtime's present profile calls frostpunk_android_frame once per accepted frame id, for DXVK
 * and the native Vulkan renderer alike. */
#include "../launcher-base/native/frame_times.h"
#include "../launcher-base/native/replacements/renderer_failure.h"
static PortFrameTimes overlay_frames = PORT_FRAME_TIMES_INIT;
static atomic_int overlay_reads; /* nothing is recorded until the overlay asks: it is off by default */
__attribute__((visibility("default"))) void frostpunk_android_frame(uint64_t monotonic_ns) {
    if(atomic_load_explicit(&overlay_reads,memory_order_relaxed))port_frame_presented(&overlay_frames, monotonic_ns);
}
JNIEXPORT jlong JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeFrameTimes(JNIEnv *env,jclass cls,jintArray intervals) {
    (void)cls;
    int32_t values[PORT_FRAME_TIMES];
    jsize length=intervals?(*env)->GetArrayLength(env,intervals):0;
    if(length<=0)return 0;
    if(length>(jsize)PORT_FRAME_TIMES)length=(jsize)PORT_FRAME_TIMES;
    atomic_store_explicit(&overlay_reads,1,memory_order_relaxed);
    uint64_t count=port_frame_times(&overlay_frames,values,(size_t)length);
    (*env)->SetIntArrayRegion(env,intervals,0,length,(const jint *)values);
    return (jlong)count;
}
/* Crash journal of the game process.  Two records, both written with pwrite/ftruncate only:
 * the shared launcher's binary marker (PORT_CRASH_JOURNAL: signal and PID at 0, first present at 5;
 * the launcher reads it where Android has no exit reasons, API < 30) and a text line in
 * frostpunk-crash.txt for GameWatchActivity.  The handler then gives the signal to the previous
 * handler (debuggerd's tombstone, or a renderer handler installed earlier), never swallowing it. */
#include "crash_marker.h"
#include <signal.h>
#include <sys/syscall.h>
static int crash_journal_fd=-1,crash_text_fd=-1;
static struct sigaction crash_previous[32];
static atomic_int crash_handling;
static const int crash_signals[]={SIGSEGV,SIGBUS,SIGILL,SIGFPE,SIGABRT,SIGTRAP,SIGSYS};
static size_t crash_put(char *out,size_t at,size_t size,const char *text){while(*text&&at+1<size)out[at++]=*text++;return at;}
static size_t crash_number(char *out,size_t at,size_t size,uint64_t value,unsigned base){
    char digits[20];size_t count=0;
    do{unsigned digit=(unsigned)(value%base);digits[count++]=(char)(digit<10?'0'+digit:'a'+digit-10);value/=base;}while(value&&count<sizeof(digits));
    while(count&&at+1<size)out[at++]=digits[--count];
    return at;
}
static void crash_signal(int sig,siginfo_t *info,void *context) {
    int saved=errno;
    if(!atomic_exchange(&crash_handling,1)){
        port_crash_marker_signal(crash_journal_fd,sig);
        if(crash_text_fd>=0){
            const char *name=sig==SIGSEGV?"SIGSEGV":sig==SIGBUS?"SIGBUS":sig==SIGILL?"SIGILL":sig==SIGFPE?"SIGFPE":sig==SIGABRT?"SIGABRT":sig==SIGTRAP?"SIGTRAP":"SIGSYS";
            char line[192];size_t at=crash_put(line,0,sizeof(line),"Native signal ");
            at=crash_number(line,at,sizeof(line),(uint64_t)sig,10);at=crash_put(line,at,sizeof(line)," (");at=crash_put(line,at,sizeof(line),name);
            int code=info?info->si_code:0;
            at=crash_put(line,at,sizeof(line),code<0?") code=-":") code=");at=crash_number(line,at,sizeof(line),(uint64_t)(code<0?-(int64_t)code:code),10);
            at=crash_put(line,at,sizeof(line)," addr=0x");at=crash_number(line,at,sizeof(line),(uint64_t)(uintptr_t)(info?info->si_addr:NULL),16);
#if defined(__aarch64__)
            if(context){at=crash_put(line,at,sizeof(line)," pc=0x");at=crash_number(line,at,sizeof(line),(uint64_t)((const ucontext_t *)context)->uc_mcontext.pc,16);}
#else
            (void)context;
#endif
            at=crash_put(line,at,sizeof(line)," tid=");at=crash_number(line,at,sizeof(line),(uint64_t)syscall(SYS_gettid),10);
            at=crash_put(line,at,sizeof(line),"\n");
            if(!ftruncate(crash_text_fd,0)){size_t done=0;while(done<at){ssize_t n=pwrite(crash_text_fd,line+done,at-done,(off_t)done);if(n>0)done+=(size_t)n;else if(n>=0||errno!=EINTR)break;}}
        }
    }
    /* A hardware fault repeats under the previous handler when this one returns; a signal that was
     * sent (abort, tgkill) has to be sent again. */
    sigaction(sig,&crash_previous[sig],NULL);
    errno=saved;
    if(!info||info->si_code<=0)syscall(SYS_tgkill,getpid(),syscall(SYS_gettid),sig);
}
static void crash_journal_install(const char *private_path) {
    char setting[PROP_VALUE_MAX]={0};__system_property_get("debug.fp.crash_journal",setting);
    if(setting[0]=='0')return;
    crash_journal_fd=port_crash_marker_open();
    char path[PATH_MAX];
    if(snprintf(path,sizeof(path),"%s/frostpunk-crash.txt",private_path)<(int)sizeof(path))
        crash_text_fd=open(path,O_CREAT|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    struct sigaction action;memset(&action,0,sizeof(action));
    action.sa_sigaction=crash_signal;action.sa_flags=SA_SIGINFO|SA_ONSTACK;sigemptyset(&action.sa_mask);
    for(size_t i=0;i<sizeof(crash_signals)/sizeof(*crash_signals);++i)sigaction(crash_signals[i],&action,&crash_previous[crash_signals[i]]);
}
__attribute__((visibility("default"))) void frostpunk_android_presented(void) {
    port_crash_marker_present(crash_journal_fd);
    pthread_mutex_lock(&current_mutex);
    if(current && !current->stopping)set_status(current,"\1");
    pthread_mutex_unlock(&current_mutex);
}
/* current_mutex keeps the Activity context alive while reading its lifecycle state. */
static int foreground_state(void){
    if(!current)return -1;
    pthread_mutex_lock(&current->mutex);
    int result=current->stopping?-1:current->window&&!current->paused;
    pthread_mutex_unlock(&current->mutex);return result;
}
__attribute__((visibility("default"))) int frostpunk_android_foreground(void){
    pthread_mutex_lock(&current_mutex);int result=foreground_state()>0;
    pthread_mutex_unlock(&current_mutex);return result;
}
__attribute__((visibility("default"))) int frostpunk_android_wait_foreground(void){
    pthread_mutex_lock(&current_mutex);int state;
    while((state=foreground_state())==0)pthread_cond_wait(&foreground_changed,&current_mutex);
    pthread_mutex_unlock(&current_mutex);return state>0;
}
__attribute__((visibility("default"))) void frostpunk_android_cancel_input(void){
    pthread_mutex_lock(&current_mutex);if(current&&!current->stopping)set_status(current,"\4");pthread_mutex_unlock(&current_mutex);
}
static int activity_flag(app_context *c,const char *name){
    JNIEnv *env=NULL;int attached=0,active=0;JavaVM *vm=c->app->activity->vm;jclass cls=NULL;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,name,"()Z");
    if(method)active=(*env)->CallBooleanMethod(env,c->app->activity->clazz,method)==JNI_TRUE;
done:
    if(env){if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);active=0;}if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);return active;
}
static int native_editor_active(app_context *c){
    pthread_mutex_lock(&c->mutex);void *runtime=c->runtime_handle;pthread_mutex_unlock(&c->mutex);
    int (*editing)(void)=runtime?dlsym(runtime,"frostpunk_mobile_text_editing"):NULL;return editing&&editing();
}
__attribute__((visibility("default"))) int frostpunk_android_photo_saved(const char *path) {
    if(!path)return 0;
    pthread_mutex_lock(&current_mutex);app_context *c=current;JNIEnv *env=NULL;int attached=0,ok=0;
    JavaVM *vm=c?c->app->activity->vm:NULL;jclass cls=NULL;jstring source=NULL,result=NULL;
    if(!vm||c->stopping)goto done;
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls)goto done;
    jmethodID method=(*env)->GetMethodID(env,cls,"exportPhoto","(Ljava/lang/String;)Ljava/lang/String;");if(!method)goto done;
    source=(*env)->NewStringUTF(env,path);if(!source)goto done;
    result=(jstring)(*env)->CallObjectMethod(env,c->app->activity->clazz,method,source);
    ok=result&&!(*env)->ExceptionCheck(env)&&(*env)->GetStringLength(env,result)>0;
    if(!ok)log_text("FP_PHOTO_EXPORT_FAILED original_retained=1");
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(result)(*env)->DeleteLocalRef(env,result);if(source)(*env)->DeleteLocalRef(env,source);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);pthread_mutex_unlock(&current_mutex);return ok;
}
static ANativeWindow *acquire_window(void *opaque,uint64_t *generation) {
    app_context *c=opaque;pthread_mutex_lock(&c->mutex);ANativeWindow *w=c->window;
    if(w)ANativeWindow_acquire(w);if(generation)*generation=c->generation;pthread_mutex_unlock(&c->mutex);return w;
}
static void release_window(ANativeWindow *window){if(window)ANativeWindow_release(window);}
static int stopping(void *opaque){app_context *c=opaque;pthread_mutex_lock(&c->mutex);int value=c->stopping;pthread_mutex_unlock(&c->mutex);return value;}
static int set_pointer_visible(void *opaque,int visible) {
    app_context *c=opaque;JNIEnv *env=NULL;jclass cls=NULL;int attached=0,accepted=0;
    const char *stage="context";JavaVM *vm=NULL;
    if(!c||!c->app||!c->app->activity||!c->app->activity->vm||!c->app->activity->clazz)goto done;
    if(stopping(c)){stage="stopping";goto done;}
    vm=c->app->activity->vm;jint state=(*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){stage="attach";if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}
    else if(state!=JNI_OK){env=NULL;stage="get_env";goto done;}
    if(!env){stage="env_null";goto done;}
    stage="pending_exception";if((*env)->ExceptionCheck(env))goto done;
    stage="activity_class";cls=(*env)->GetObjectClass(env,c->app->activity->clazz);if(!cls||(*env)->ExceptionCheck(env))goto done;
    stage="show_pointer_method";jmethodID method=(*env)->GetMethodID(env,cls,"showPointer","(Z)Z");if(!method||(*env)->ExceptionCheck(env))goto done;
    stage="show_pointer_call";jboolean result=(*env)->CallBooleanMethod(env,c->app->activity->clazz,method,visible?JNI_TRUE:JNI_FALSE);
    if((*env)->ExceptionCheck(env))goto done;accepted=result==JNI_TRUE;stage=accepted?"accepted":"java_rejected";
done:
    if(env){if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);accepted=0;}if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached&&(*vm)->DetachCurrentThread(vm)!=JNI_OK){accepted=0;stage="detach";}
    char line[192];snprintf(line,sizeof(line),"ANDROID_POINTER_JNI_RESULT visible=%d accepted=%d stage=%s",visible!=0,accepted,stage);log_text(line);return accepted;
}
static void wake(void *opaque){app_context *c=opaque;pthread_mutex_lock(&c->mutex);++c->wake_sequence;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);
#ifndef FP_GAME_RUNTIME
    /* The game runtime's sole event consumer waits on changed, not the looper. */
    ALooper_wake(c->app->looper);
#endif
}
static uint64_t event_sequence(void *opaque) {
    app_context *c=opaque;pthread_mutex_lock(&c->mutex);
    uint64_t result=c->wake_sequence;pthread_mutex_unlock(&c->mutex);return result;
}
static int wait_locked(app_context *c,int timeout) {
    /* ABI1 has one host-event consumer. Retain explicit wakes until it observes
     * them, including a stop request arriving just before poll_event(-1). */
    if(c->count||c->stopping||c->wake_sequence!=c->consumed_wake_sequence)return 0;if(!timeout)return ETIMEDOUT;
    if(timeout<0)return pthread_cond_wait(&c->changed,&c->mutex);
    struct timespec end;clock_gettime(CLOCK_MONOTONIC,&end);end.tv_sec+=timeout/1000;end.tv_nsec+=(long)(timeout%1000)*1000000;
    if(end.tv_nsec>=1000000000L){++end.tv_sec;end.tv_nsec-=1000000000L;}return pthread_cond_timedwait(&c->changed,&c->mutex,&end);
}
static int wait_event(void *opaque,int timeout) {
    app_context *c=opaque;pthread_mutex_lock(&c->mutex);int status=wait_locked(c,timeout),result=c->stopping?-1:c->count?1:status==ETIMEDOUT?0:1;
    c->consumed_wake_sequence=c->wake_sequence;pthread_mutex_unlock(&c->mutex);return result;
}
static int poll_event(void *opaque,FrostpunkAndroidEvent *event,int timeout) {
    app_context *c=opaque;pthread_mutex_lock(&c->mutex);wait_locked(c,timeout);int result=c->stopping?-1:0;
    if(c->count){*event=c->events[c->head];c->head=(c->head+1)%EVENTS;--c->count;result=1;}
    c->consumed_wake_sequence=c->wake_sequence;pthread_mutex_unlock(&c->mutex);return result;
}
static void push(app_context *c,FrostpunkAndroidEvent event) {
    pthread_mutex_lock(&c->mutex);event.window_generation=c->generation;
    if(c->count==EVENTS){c->overflow=1;c->stopping=1;log_text("FROSTPUNK_HOST_FAIL input_queue_overflow");}
    else{c->events[(c->head+c->count)%EVENTS]=event;++c->count;}
    ++c->wake_sequence;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);
}
#ifdef FP_GAME_RUNTIME
static jboolean touch_submit(FrostpunkAndroidEvent event) {
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    event.time_ns=(uint64_t)now.tv_sec*1000000000u+(uint64_t)now.tv_nsec;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    int accepted=c&&!stopping(c);
    if(accepted)push(c,event);
    pthread_mutex_unlock(&current_mutex);return accepted?JNI_TRUE:JNI_FALSE;
}
static _Atomic uint32_t touch_sequence;
JNIEXPORT jlong JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeClickToken(JNIEnv *env,jclass cls){(void)env;(void)cls;return atomic_load(&touch_sequence);}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeClickReady(JNIEnv *env,jclass cls,jlong ticket){
    (void)env;(void)cls;if(ticket<=0||ticket>UINT32_MAX)return JNI_FALSE;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    int (*ready)(uint32_t)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_android_touch_ready"):NULL;
    int result=ready?ready((uint32_t)ticket):0;pthread_mutex_unlock(&current_mutex);return result?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeCamera(JNIEnv *env,jclass cls,jint action,jfloat dx,jfloat dy,jfloat zoom,jfloat angle) {
    (void)env;(void)cls;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    int (*submit)(unsigned,float,float,float,float)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_submit"):NULL;
    int result=submit?submit((unsigned)action,dx,dy,zoom,angle):0;
    pthread_mutex_unlock(&current_mutex);return result?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeContext(JNIEnv *env,jclass cls,jfloat x,jfloat y) {
    (void)env;(void)cls;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    unsigned (*context)(float,float)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_context"):NULL;
    unsigned result=context?context(x,y):0;
    pthread_mutex_unlock(&current_mutex);return (jint)result;
}
JNIEXPORT jlong JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeSwipeTarget(JNIEnv *env,jclass cls,jfloat x,jfloat y,jint direction) {
    (void)env;(void)cls;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    uint64_t (*target)(float,float,int)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_swipe_target"):NULL;
    uint64_t result=target?target(x,y,direction):0;
    pthread_mutex_unlock(&current_mutex);return (jlong)result;
}
JNIEXPORT jint JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeConstructionView(JNIEnv *env,jclass cls,jfloatArray output,jcharArray detail,jint painter) {
    (void)cls;
    if(!output||!detail||(*env)->GetArrayLength(env,output)!=8)return 0;
    float corners[8];
    uint16_t text[256];
    unsigned capacity=(unsigned)(*env)->GetArrayLength(env,detail);
    if(capacity>sizeof(text)/sizeof(*text))capacity=sizeof(text)/sizeof(*text);
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    unsigned (*view)(float*,uint16_t*,unsigned,unsigned)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_construction_view"):NULL;
    unsigned state=view?view(corners,text,capacity,(unsigned)painter):0;
    pthread_mutex_unlock(&current_mutex);
    if(state&1)(*env)->SetFloatArrayRegion(env,output,0,8,corners);
    if(state>>1)(*env)->SetCharArrayRegion(env,detail,0,(jsize)(state>>1),(const jchar*)text);
    return (jint)state;
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativePointer(JNIEnv *env,jclass cls,jint buttons,jfloat x,jfloat y,jfloat wheel) {
    (void)env;(void)cls;
    if((buttons&~7)||!isfinite(x)||!isfinite(y)||!isfinite(wheel)||x<0||x>1||y<0||y>1||fabsf(wheel)>8)return JNI_FALSE;
    uint32_t ticket=atomic_fetch_add(&touch_sequence,1)+1;
    if(!ticket)ticket=atomic_fetch_add(&touch_sequence,1)+1;
    return touch_submit((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=wheel?AMOTION_EVENT_ACTION_SCROLL:AMOTION_EVENT_ACTION_MOVE,
        .source=AINPUT_SOURCE_MOUSE,.buttons=(uint32_t)buttons,.repeat=ticket,.width=1,.height=1,.x=x,.y=y,.scroll_y=wheel});
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeKey(JNIEnv *env,jclass cls,jint key,jboolean down) {
    (void)env;(void)cls;
    switch(key){case AKEYCODE_SPACE:case AKEYCODE_1:case AKEYCODE_2:case AKEYCODE_3:case AKEYCODE_B:case AKEYCODE_V:case AKEYCODE_Q:case AKEYCODE_E:case AKEYCODE_ESCAPE:
        case AKEYCODE_ENTER:case AKEYCODE_DEL:case AKEYCODE_FORWARD_DEL:case AKEYCODE_DPAD_LEFT:case AKEYCODE_DPAD_RIGHT:case AKEYCODE_MOVE_HOME:case AKEYCODE_MOVE_END:break;default:return JNI_FALSE;}
    return touch_submit((FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=down?AKEY_EVENT_ACTION_DOWN:AKEY_EVENT_ACTION_UP,
        .source=AINPUT_SOURCE_KEYBOARD,.keycode=(uint32_t)key});
}
static jboolean ime_submit(unsigned action,const jchar *text,unsigned units,unsigned before,unsigned after){
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    int (*submit)(unsigned,const uint16_t*,unsigned,unsigned,unsigned)=c&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_text_submit"):NULL;
    int accepted=submit?submit(action,(const uint16_t*)text,units,before,after):0;
    pthread_mutex_unlock(&current_mutex);return accepted?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeText(JNIEnv *env,jclass cls,jstring text) {
    (void)cls;if(!text)return JNI_FALSE;jsize count=(*env)->GetStringLength(env,text);
    if(count>256)return JNI_FALSE;
    const jchar *units=(*env)->GetStringChars(env,text,NULL);if(!units)return JNI_FALSE;
    jboolean ok=ime_submit(0,units,(unsigned)count,0,0);
    (*env)->ReleaseStringChars(env,text,units);return ok;
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeDelete(JNIEnv *env,jclass cls,jint before,jint after,jboolean points){
    (void)env;(void)cls;if(before<0||after<0||before>256||after>256)return JNI_FALSE;
    return ime_submit(points?2:1,NULL,0,(unsigned)before,(unsigned)after);
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeEditorKey(JNIEnv *env,jclass cls,jint key){
    (void)env;(void)cls;unsigned vk,scan;
    switch(key){case AKEYCODE_ENTER:vk=13;scan=0x1c;break;case AKEYCODE_DPAD_LEFT:vk=0x25;scan=0xe04b;break;
        case AKEYCODE_DPAD_RIGHT:vk=0x27;scan=0xe04d;break;case AKEYCODE_MOVE_HOME:vk=0x24;scan=0xe047;break;
        case AKEYCODE_MOVE_END:vk=0x23;scan=0xe04f;break;default:return JNI_FALSE;}
    return ime_submit(3,NULL,0,vk,scan);
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeEditorActive(JNIEnv *env,jclass cls){
    (void)env;(void)cls;pthread_mutex_lock(&current_mutex);app_context *c=current;
    int active=c&&!c->stopping&&native_editor_active(c);pthread_mutex_unlock(&current_mutex);return active?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeImeViewport(JNIEnv *env,jclass cls,jfloat fraction){
    (void)env;(void)cls;if(!isfinite(fraction)||fraction<0||fraction>1)return;
    pthread_mutex_lock(&current_mutex);app_context *c=current;
    void (*set)(float)=c&&!c->stopping&&c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_mobile_ime_viewport"):NULL;
    if(set)set(fraction);pthread_mutex_unlock(&current_mutex);
}
JNIEXPORT jboolean JNICALL Java_com_frostpunk_staticrecomp_TouchControls_nativeMouse(JNIEnv *env,jclass cls,jint action,jint buttons,jint meta,jfloat x,jfloat y,jfloat sx,jfloat sy) {
    (void)env;(void)cls;
    if(buttons<0||!isfinite(x)||!isfinite(y)||!isfinite(sx)||!isfinite(sy)||fabsf(x)>2||fabsf(y)>2||fabsf(sx)>273||fabsf(sy)>273)return JNI_FALSE;
    switch(action){case AMOTION_EVENT_ACTION_DOWN:case AMOTION_EVENT_ACTION_UP:case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_CANCEL:case AMOTION_EVENT_ACTION_HOVER_ENTER:case AMOTION_EVENT_ACTION_HOVER_MOVE:case AMOTION_EVENT_ACTION_HOVER_EXIT:
        case AMOTION_EVENT_ACTION_SCROLL:case AMOTION_EVENT_ACTION_BUTTON_PRESS:case AMOTION_EVENT_ACTION_BUTTON_RELEASE:break;default:return JNI_FALSE;}
    return touch_submit((FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=(uint32_t)action,.source=AINPUT_SOURCE_MOUSE,
        .buttons=(uint32_t)buttons,.meta=(uint32_t)meta,.width=1,.height=1,.x=x,.y=y,.scroll_x=sx,.scroll_y=sy});
}
#endif
static void *provider(void *opaque){app_context *c=opaque;pthread_mutex_lock(&c->mutex);void *p=(void *)c->provider;pthread_mutex_unlock(&c->mutex);return p;}
const FrostpunkAndroidHost *frostpunk_android_host(void){return current?&current->api:NULL;}
#ifdef FP_GAME_RUNTIME
static void request_window_frame_rate(ANativeWindow *window,uint64_t generation) {
    uint32_t rates[32],count=frostpunk_android_frame_rates(rates,32);if(!count){log_text("ANDROID_FRAME_RATE_QUERY_FAILED");return;}
    uint32_t hz=rates[count-1];app_context *c=current;
    if(c){pthread_mutex_lock(&c->mutex);
        void (*configure)(const uint32_t*,uint32_t)=c->runtime_handle?dlsym(c->runtime_handle,"fp_port_frame_rates_configure"):NULL;
        uint32_t (*limit)(void)=c->runtime_handle?dlsym(c->runtime_handle,"frostpunk_android_frame_limit"):NULL;
        if(configure)configure(rates,count);if(limit)hz=limit();pthread_mutex_unlock(&c->mutex);}
    int result=ANativeWindow_setFrameRate(window,(float)hz,ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    char text[192];snprintf(text,sizeof(text),"ANDROID_FRAME_RATE_REQUEST generation=%llu fps=%u compatibility=default result=%d physical_mode_proven=0",(unsigned long long)generation,hz,result);log_text(text);
}
#endif
static void render_note(const char *line);
static void set_window(app_context *c,ANativeWindow *window) {
    pthread_mutex_lock(&c->mutex);ANativeWindow *old=c->window;if(window)ANativeWindow_acquire(window);c->window=window;++c->generation;pthread_mutex_unlock(&c->mutex);
    if(window){
#ifdef FP_GAME_RUNTIME
        request_window_frame_rate(window,c->generation);
#endif
        c->physical_width=ANativeWindow_getWidth(window);c->physical_height=ANativeWindow_getHeight(window);
        char noted[96];snprintf(noted,sizeof(noted),"window init %dx%d",c->physical_width,c->physical_height);render_note(noted);}if(old)ANativeWindow_release(old);
    push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW,.width=window?ANativeWindow_getWidth(window):0,.height=window?ANativeWindow_getHeight(window):0});
    notify_foreground();
}
static void resize_window(app_context *c,ANativeWindow *window) {
    pthread_mutex_lock(&c->mutex);int same=c->window==window;pthread_mutex_unlock(&c->mutex);
    if(!same){set_window(c,window);return;}
    // Buffer-size changes keep the same consumer; invalidating its Vulkan surface creates a resize loop.
    if(window){c->physical_width=ANativeWindow_getWidth(window);c->physical_height=ANativeWindow_getHeight(window);
        char noted[96];snprintf(noted,sizeof(noted),"window resize %dx%d",c->physical_width,c->physical_height);render_note(noted);}
    push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_WINDOW,.width=window?ANativeWindow_getWidth(window):0,.height=window?ANativeWindow_getHeight(window):0});
}
static int write_all(int fd,const char *buffer,size_t bytes);
static void render_note(const char *line) {
    const char *path=getenv("FROSTPUNK_RENDER_LOG");
    char text[320];
    int fd,n;
    if(!path||!path[0]||!line||!line[0])return;
    n=snprintf(text,sizeof(text),"%s\n",line);
    if(n<=0)return;
    if(n>=(int)sizeof(text))n=(int)sizeof(text)-1;
    fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd<0)return;
    if(!write_all(fd,text,(size_t)n)){/* the capped writer in the runtime trims the same file */}
    close(fd);
}
static int write_all(int fd,const char *buffer,size_t bytes) {
    while(bytes){ssize_t n=write(fd,buffer,bytes);if(n<0&&errno==EINTR)continue;if(n<=0)return 0;buffer+=n;bytes-=(size_t)n;}return 1;
}
static int extract_driver(app_context *c) {
#ifdef FP_GAME_RUNTIME
    const char *custom=getenv("PORT_TURNIP_DRIVER");
    if(custom && custom[0])return 1;
    char path[PATH_MAX];
    int length=snprintf(path,sizeof(path),"%s/libpurple_turnip.so",c->libraries);
    if(length<0 || (size_t)length>=sizeof(path) || access(path,R_OK))return 0;
    log_text("FROSTPUNK_TURNIP bundled=purple-T30");
    /* The launcher exports PORT_TURNIP_DRIVER="" for the port's own driver.  setenv(...,0) kept that
     * empty value, so adrenotools looked for files/driver/vulkan.ad07xx.so: a stale copy left by
     * builds before 0.2-dev on upgraded phones, nothing on a fresh install (then stat() failed,
     * "no loader/proc returned", and the Qualcomm driver was refused: OnePlus 13 / Adreno 830). */
    char stale[PATH_MAX];
    if(snprintf(stale,sizeof(stale),"%s/driver/vulkan.ad07xx.so",c->private_path)<(int)sizeof(stale)&&!unlink(stale))
        log_text("FROSTPUNK_TURNIP removed stale files/driver/vulkan.ad07xx.so");
    return !setenv("PORT_TURNIP_DRIVER",path,1);
#elif defined(FP_GPU_SYSTEM)
    (void)c;return 1;
#else
    int root=open(c->private_path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return 0;
    if(mkdirat(root,"driver",0700)&&errno!=EEXIST){close(root);return 0;}
    int directory=openat(root,"driver",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(root);if(directory<0)return 0;
    AAsset *asset=NULL;char soc[PROP_VALUE_MAX]={0};int sm8250=0;
    if(__system_property_get("ro.soc.model",soc)>0&&!strcmp(soc,"SM8250")){
        asset=AAssetManager_open(c->app->activity->assetManager,"turnip/vulkan.sm8250.so",AASSET_MODE_STREAMING);
        sm8250=asset!=NULL;
    }
    if(!asset)asset=AAssetManager_open(c->app->activity->assetManager,"turnip/vulkan.ad07xx.so",AASSET_MODE_STREAMING);
    if(asset)log_text(sm8250?"FROSTPUNK_TURNIP_ASSET model=SM8250 purple=T30":"FROSTPUNK_TURNIP_ASSET model=default purple=T30");
    if(!asset){close(directory);return 0;}
    unlinkat(directory,"driver.tmp",0);int fd=openat(directory,"driver.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0400);int ok=fd>=0;int64_t count=0;
    char buffer[65536];int bytes;while(ok&&(bytes=AAsset_read(asset,buffer,sizeof(buffer)))>0){ok=write_all(fd,buffer,(size_t)bytes);count+=bytes;}
    ok=ok&&bytes==0&&count==AAsset_getLength64(asset);AAsset_close(asset);
    if(fd>=0){if(fsync(fd))ok=0;if(close(fd))ok=0;}
    if(ok)ok=!renameat(directory,"driver.tmp",directory,"vulkan.ad07xx.so");if(!ok)unlinkat(directory,"driver.tmp",0);close(directory);return ok;
#endif
}
static int library_path(app_context *c) {
    JNIEnv *env=NULL;JavaVM *vm=c->app->activity->vm;if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK)return 0;
    jobject activity=c->app->activity->clazz;jclass cls=(*env)->GetObjectClass(env,activity);
    jmethodID get=(*env)->GetMethodID(env,cls,"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;");jobject info=get?(*env)->CallObjectMethod(env,activity,get):NULL;
    int ok=0;if(info){jclass ic=(*env)->GetObjectClass(env,info);jfieldID field=(*env)->GetFieldID(env,ic,"nativeLibraryDir","Ljava/lang/String;");
        jstring path=field?(jstring)(*env)->GetObjectField(env,info,field):NULL;if(path){const char *text=(*env)->GetStringUTFChars(env,path,NULL);if(text){ok=snprintf(c->libraries,sizeof(c->libraries),"%s",text)<(int)sizeof(c->libraries);(*env)->ReleaseStringUTFChars(env,path,text);}(*env)->DeleteLocalRef(env,path);}(*env)->DeleteLocalRef(env,ic);(*env)->DeleteLocalRef(env,info);}
    if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);(*env)->DeleteLocalRef(env,cls);(*vm)->DetachCurrentThread(vm);return ok;
}
static void report(app_context *c) {
#ifdef FP_GAME_RUNTIME
    char error[sizeof(c->error)];pthread_mutex_lock(&c->mutex);
    int started=c->worker_started,finished=c->worker_finished,result=c->worker_result,loaded=c->runtime_loaded;
    snprintf(error,sizeof(error),"%s",c->error);pthread_mutex_unlock(&c->mutex);
    char path[PATH_MAX],temporary[PATH_MAX];snprintf(path,sizeof(path),"%s/game-state.json",c->private_path);snprintf(temporary,sizeof(temporary),"%s/game-state.tmp",c->private_path);
    char state[sizeof(c->reported_game_state)];int length=snprintf(state,sizeof(state),"{\"kind\":\"" FIXTURE_NAME "\",\"host_abi\":1,\"runtime_loaded\":%s,\"worker_started\":%s,\"worker_finished\":%s,\"worker_result\":%d,\"real_window\":%s,\"window_generation\":%" PRIu64 ",\"queue_overflow\":%s,\"game_boot_proven\":false}\n",loaded?"true":"false",started?"true":"false",finished?"true":"false",result,c->window?"true":"false",c->generation,c->overflow?"true":"false");
    /* Event-pump wakes often report identical state; cache only committed JSON. */
    if(length>=0 && (size_t)length<sizeof(state) && strcmp(state,c->reported_game_state)){
        FILE *file=fopen(temporary,"w");if(file){int written=fputs(state,file)>=0;int closed=fclose(file)==0;
            if(written && closed && rename(temporary,path)==0)memcpy(c->reported_game_state,state,(size_t)length+1);}
    }
    if(error[0]){char failure[sizeof(error)+1];snprintf(failure,sizeof(failure),"\5%s",error);set_status(c,failure);}
    else if(!started)set_status(c,FIXTURE_TITLE "\nWaiting for Android surface and native runtime");
#else
    uint32_t present_width=0,present_height=0;window_probe_extent(c->probe,&present_width,&present_height);
    uint64_t frames=c->total_frames+window_probe_frames(c->probe);char path[PATH_MAX],temporary[PATH_MAX];
    snprintf(path,sizeof(path),"%s/probe-state.json",c->private_path);snprintf(temporary,sizeof(temporary),"%s/probe-state.tmp",c->private_path);
    FILE *file=fopen(temporary,"w");if(file){fprintf(file,"{\"fixture\":\"" FIXTURE_NAME "\",\"game_boot_proven\":false,\"real_window\":%s,\"width\":%d,\"height\":%d,\"physical_width\":%d,\"physical_height\":%d,\"present_width\":%u,\"present_height\":%u,\"window_generation\":%" PRIu64 ",\"focused\":%s,\"presented_frames\":%" PRIu64 ",\"touch_events\":%u,\"key_events\":%u,\"queue_events_consumed\":%u,\"window_acquire_checks\":%u,\"queue_overflow\":%s,\"Vulkan13_BC\":%s,\"device\":\"%s\",\"d3d11\":%s}\n",
        c->window?"true":"false",c->window?ANativeWindow_getWidth(c->window):0,c->window?ANativeWindow_getHeight(c->window):0,c->physical_width,c->physical_height,present_width,present_height,c->generation,c->focused?"true":"false",frames,c->touches,c->keys,c->consumed,c->acquire_checks,c->overflow?"true":"false",c->probe?"true":"false",window_probe_device(c->probe),window_probe_details(c->probe));fclose(file);rename(temporary,path);}
    char status[768];snprintf(status,sizeof(status),FIXTURE_TITLE "\n%s\nReal Vulkan presents: %" PRIu64 " | Touch: %u | Key: %u | Window generation: %" PRIu64 "\nThis is a platform fixture; original game is not running.",
        c->probe?window_probe_device(c->probe):c->error[0]?c->error:"Waiting for Android surface",frames,c->touches,c->keys,c->generation);set_status(c,status);
#endif
}
#ifdef FP_GAME_RUNTIME
static int verify_game_executable(app_context *c,char *error,size_t error_size) {
    char path[PATH_MAX];FILE *file=NULL;int ok=0,attached=0;JNIEnv *env=NULL;JavaVM *vm=c->app->activity->vm;
    jclass cls=NULL;jobject digest=NULL;jstring algorithm=NULL;jbyteArray buffer=NULL,result=NULL;struct stat before,after;
    if(snprintf(path,sizeof(path),"%s/Frostpunk.exe",c->game)>=(int)sizeof(path)){snprintf(error,error_size,"Game executable path too long");return 0;}
    file=fp_data_fopen(path,"rb");
    if(!file||(!fp_data_stat("Frostpunk.exe",&before)&&lstat(path,&before))||!S_ISREG(before.st_mode)){snprintf(error,error_size,"Cannot read original game executable errno=%d",errno);goto done;}
    jint state=(*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6);
    if(state==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK){env=NULL;goto done;}attached=1;}else if(state!=JNI_OK){env=NULL;goto done;}
    cls=(*env)->FindClass(env,"java/security/MessageDigest");if(!cls)goto done;
    jmethodID create=(*env)->GetStaticMethodID(env,cls,"getInstance","(Ljava/lang/String;)Ljava/security/MessageDigest;");
    jmethodID update=(*env)->GetMethodID(env,cls,"update","([BII)V"),finish=(*env)->GetMethodID(env,cls,"digest","()[B");
    if(!create||!update||!finish)goto done;
    algorithm=(*env)->NewStringUTF(env,"SHA-256");if(!algorithm)goto done;
    digest=(*env)->CallStaticObjectMethod(env,cls,create,algorithm);if(!digest||(*env)->ExceptionCheck(env))goto done;
    buffer=(*env)->NewByteArray(env,65536);if(!buffer)goto done;
    unsigned char bytes[65536];uint64_t total=0;
    for(;;){size_t count=fread(bytes,1,sizeof(bytes),file);if(!count){if(ferror(file))goto done;break;}
        (*env)->SetByteArrayRegion(env,buffer,0,(jsize)count,(const jbyte*)bytes);if((*env)->ExceptionCheck(env))goto done;
        (*env)->CallVoidMethod(env,digest,update,buffer,0,(jint)count);if((*env)->ExceptionCheck(env))goto done;total+=(uint64_t)count;
        if(stopping(c)){snprintf(error,error_size,"Game executable verification cancelled");goto done;}}
    if((!fp_data_stat("Frostpunk.exe",&after)&&lstat(path,&after))||total!=(uint64_t)before.st_size||before.st_size!=after.st_size||before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec){snprintf(error,error_size,"Game executable changed during SHA-256 verification");goto done;}
    result=(jbyteArray)(*env)->CallObjectMethod(env,digest,finish);if(!result||(*env)->ExceptionCheck(env)||(*env)->GetArrayLength(env,result)!=32)goto done;
    unsigned char hash[32];char hex[65];(*env)->GetByteArrayRegion(env,result,0,32,(jbyte*)hash);if((*env)->ExceptionCheck(env))goto done;
    for(unsigned i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
    if(strcmp(hex,FP_ORIGINAL_EXE_SHA256)){snprintf(error,error_size,"Original Frostpunk.exe SHA-256 mismatch: %s",hex);goto done;}
    log_text("FROSTPUNK_PE_SHA256_VERIFIED " FP_ORIGINAL_EXE_SHA256);ok=1;
done:
    if(env){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(result)(*env)->DeleteLocalRef(env,result);if(buffer)(*env)->DeleteLocalRef(env,buffer);if(digest)(*env)->DeleteLocalRef(env,digest);if(algorithm)(*env)->DeleteLocalRef(env,algorithm);if(cls)(*env)->DeleteLocalRef(env,cls);}
    if(attached)(*vm)->DetachCurrentThread(vm);if(file)fclose(file);if(!ok&&!error[0])snprintf(error,error_size,"Game executable SHA-256 verification failed");return ok;
}
static int game_inputs_check_text(app_context *c,int root,char *text) {
    char *line=text;unsigned count=0;int ok=1;
    while(*line&&ok){char *end=strchr(line,'\n');if(end)*end=0;char *tab=strchr(line,'\t');
        size_t length=strlen(line);if(length&&line[length-1]=='\r')line[length-1]=0;
        if(!tab){ok=0;break;}*tab++=0;struct stat info;
        if(!*line||!*tab||strchr(tab,'/')||strchr(tab,'\\')||strchr(tab,':')||!strcmp(tab,".")||!strcmp(tab,"..")){ok=0;break;}
        if((!fp_data_stat(tab,&info)&&fstatat(root,tab,&info,AT_SYMLINK_NOFOLLOW))||!S_ISREG(info.st_mode)){snprintf(c->error,sizeof(c->error),"Missing game input: %s",tab);ok=0;break;}
        int size_ok=0;
        if(!strcmp(line,"*")&&!strcmp(tab,"textures-s3.dat"))size_ok=info.st_size>0; /* TextureBackend validates original/converted entry spans. */
        else for(char *value=line;;){char *parsed=NULL;errno=0;
            if(*value<'0'||*value>'9'){ok=0;break;}
            uint64_t bytes=strtoull(value,&parsed,10);
            if(errno||(*parsed&&*parsed!=',')){ok=0;break;}
            if((uint64_t)info.st_size==bytes)size_ok=1;
            if(!*parsed)break;value=parsed+1;
        }
        if(!ok)break;
        if(!size_ok&&(strcmp(tab,"GfxConfig.ini")||info.st_size<=0)){snprintf(c->error,sizeof(c->error),"Incorrect game input size: %s\nGame directory: %s",tab,c->game);ok=0;break;}
        ++count;if(!end)break;line=end+1;
    }
    if(!ok||!count){if(!c->error[0])snprintf(c->error,sizeof(c->error),"Invalid game input manifest");return 0;}return 1;
}
static int game_inputs_present(app_context *c) {
    int root=open(c->game,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0){snprintf(c->error,sizeof(c->error),"Missing game directory: %s",c->game);return 0;}
    AAsset *asset=AAssetManager_open(c->app->activity->assetManager,"game-inputs.tsv",AASSET_MODE_BUFFER);
    if(!asset){close(root);snprintf(c->error,sizeof(c->error),"Missing packaged game-inputs.tsv");return 0;}
    off64_t length=AAsset_getLength64(asset);char *text=length>0&&length<32768?malloc((size_t)length+1):NULL;
    int ok=text&&AAsset_read(asset,text,(size_t)length)==length;AAsset_close(asset);
    if(!ok){free(text);close(root);snprintf(c->error,sizeof(c->error),"Invalid packaged game-inputs.tsv");return 0;}
    text[length]=0;ok=game_inputs_check_text(c,root,text);free(text);close(root);return ok;
}
static int prepare_game_runtime(app_context *c,char *path,size_t capacity,char *error,size_t error_size) {
    JNIEnv *env=NULL;JavaVM *vm=c->app->activity->vm;int attached=0,ok=0;
    if((*vm)->GetEnv(vm,(void **)&env,JNI_VERSION_1_6)==JNI_EDETACHED){
        if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK)env=NULL;else attached=1;
    }
    if(!env){snprintf(error,error_size,"Cannot attach native runtime preparation");return 0;}
    jclass cls=(*env)->GetObjectClass(env,c->app->activity->clazz);
    jstring result=NULL;
    jmethodID profile_method=cls?(*env)->GetMethodID(env,cls,"prepareNativeProfile","()Ljava/lang/String;"):NULL;
    jstring profile=profile_method?(jstring)(*env)->CallObjectMethod(env,c->app->activity->clazz,profile_method):NULL;
    if(profile&&!(*env)->ExceptionCheck(env)){
        const char *text=(*env)->GetStringUTFChars(env,profile,NULL);
        if(text){ok=text[0]=='/'&&snprintf(c->profile,sizeof(c->profile),"%s",text)<(int)sizeof(c->profile);(*env)->ReleaseStringUTFChars(env,profile,text);}
    }
    if(profile)(*env)->DeleteLocalRef(env,profile);
    if(!ok||(*env)->ExceptionCheck(env))goto failed;
    ok=0;
    jmethodID method=cls?(*env)->GetMethodID(env,cls,"prepareNativeRuntime","()Ljava/lang/String;"):NULL;
    result=method?(jstring)(*env)->CallObjectMethod(env,c->app->activity->clazz,method):NULL;
    if(!(*env)->ExceptionCheck(env)&&result){
        const char *text=(*env)->GetStringUTFChars(env,result,NULL);
        if(text){ok=snprintf(path,capacity,"%s",text)<(int)capacity;(*env)->ReleaseStringUTFChars(env,result,text);}
    }
failed:
    if(!ok)snprintf(error,error_size,"Cannot prepare native game runtime");
    if((*env)->ExceptionCheck(env)){
        jthrowable failure=(*env)->ExceptionOccurred(env);(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);
        jclass type=failure?(*env)->GetObjectClass(env,failure):NULL;
        jmethodID describe=type?(*env)->GetMethodID(env,type,"toString","()Ljava/lang/String;"):NULL;
        jstring message=describe?(jstring)(*env)->CallObjectMethod(env,failure,describe):NULL;
        const char *text=message&&!(*env)->ExceptionCheck(env)?(*env)->GetStringUTFChars(env,message,NULL):NULL;
        if(text){snprintf(error,error_size,"Cannot prepare native game runtime: %s",text);(*env)->ReleaseStringUTFChars(env,message,text);}
        if(message)(*env)->DeleteLocalRef(env,message);if(type)(*env)->DeleteLocalRef(env,type);if(failure)(*env)->DeleteLocalRef(env,failure);
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
    }
    if(result)(*env)->DeleteLocalRef(env,result);if(cls)(*env)->DeleteLocalRef(env,cls);
    if(attached)(*vm)->DetachCurrentThread(vm);return ok;
}
/* The native Vulkan renderer's build-time shader library (tools/build_shaderlib.py) is a stored
 * asset of this APK.  Its file descriptor range goes to libfrostpunk_rdvk.so, which maps the
 * SPIR-V straight from the APK; the descriptor stays open for the process. */
static void export_shader_library(app_context *c) {
    static int fd=-1;
    if(fd>=0)return;
    AAsset *asset=AAssetManager_open(c->app->activity->assetManager,"shaders/rdvk_shaderlib.bin",AASSET_MODE_UNKNOWN);
    off64_t start=0,length=0;
    if(asset){fd=AAsset_openFileDescriptor64(asset,&start,&length);AAsset_close(asset);}
    char text[96];
    if(fd<0){unsetenv("FROSTPUNK_RDVK_SHADERLIB");log_text("FROSTPUNK_RDVK_SHADERLIB none (asset missing or compressed)");return;}
    snprintf(text,sizeof(text),"%d:%lld:%lld",fd,(long long)start,(long long)length);
    setenv("FROSTPUNK_RDVK_SHADERLIB",text,1);
    char line[160];snprintf(line,sizeof(line),"FROSTPUNK_RDVK_SHADERLIB apk asset fd=%s",text);log_text(line);
}
static PFN_vkGetInstanceProcAddr open_vulkan(app_context *c,char *error,size_t error_size);
static void apply_debug_environment(void);
/* The selected renderer's driver did not load or is not the driver it names (renderer failure policy of
 * 24 November 2026: no other driver is tried). The record lets the launcher offer the next renderer of its
 * list; the crash watcher (GameWatchActivity) opens the launcher when this process ends. Started without the
 * launcher (no record path), the error dialog stays as before. */
static void renderer_start_failed(const char *reason) {
    int recorded=port_renderer_failed(reason,0);
    char line[640];snprintf(line,sizeof(line),"FROSTPUNK_RENDERER_FAILURE kind=start recorded=%d %s",recorded,reason);log_text(line);
    if(recorded)return;
    fflush(NULL);_exit(73);
}
static void *game_worker(void *opaque) {
    app_context *c=opaque;int result=74;char error[512]={0},path[PATH_MAX];
    apply_debug_environment();
    if(!verify_game_executable(c,error,sizeof(error))){/* No runtime mapping or guest dispatch after an identity failure. */}
    else if(!prepare_game_runtime(c,path,sizeof(path),error,sizeof(error))){}
    else{
        PFN_vkGetInstanceProcAddr proc=open_vulkan(c,error,sizeof(error));
        pthread_mutex_lock(&c->mutex);c->provider=proc;pthread_mutex_unlock(&c->mutex);
        if(proc)export_shader_library(c);
        else if(!stopping(c))renderer_start_failed(error[0]?error:"Vulkan driver could not be opened");
        void *library=proc&&!stopping(c)?dlopen(path,RTLD_NOW|RTLD_GLOBAL):NULL;
        if(!proc)goto finished;
        if(!library)snprintf(error,sizeof(error),"Cannot load native game runtime: %s",dlerror());
        else{pthread_mutex_lock(&c->mutex);c->runtime_handle=library;c->runtime_loaded=1;
            void (*pause)(int)=dlsym(library,"frostpunk_android_set_paused");if(pause)pause(c->paused);
            pthread_mutex_unlock(&c->mutex);ALooper_wake(c->app->looper);
            dlerror();int (*run)(const FrostpunkAndroidHost *)=(int (*)(const FrostpunkAndroidHost *))dlsym(library,"frostpunk_android_run");const char *detail=dlerror();
            if(detail||!run)snprintf(error,sizeof(error),"Native runtime entry missing: %s",detail?detail:"frostpunk_android_run");
            else if(!stopping(c)){set_status(c,FIXTURE_TITLE "\nStarting original statically recompiled entry");log_text("FROSTPUNK_GAME_WORKER_CALL host_abi=1 fixture_renderer=0 event_consumer=USER32");result=run(&c->api);snprintf(error,sizeof(error),"Native game runtime returned %d; gameplay is not confirmed",result);}
        }
    }
finished:
    pthread_mutex_lock(&c->mutex);if(error[0])snprintf(c->error,sizeof(c->error),"%s",error);c->worker_result=result;c->worker_finished=1;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);
    if(error[0])log_text(error);ALooper_wake(c->app->looper);return NULL;
}
static void stop_probe(app_context *c){(void)c;}
/* Vulkan driver under DXVK: Android's system driver (default) or Mesa Turnip
 * through adrenotools (Adreno only). The Graphics menu stores vulkan_driver= in
 * port-settings.txt (applied at start); debug.frostpunk.vulkan_driver overrides it. */
static int turnip_requested(app_context *c) {
    const char *renderer=getenv("PORT_RENDERER");
    if(renderer && *renderer)return !strcmp(renderer,"vulkan-turnip") || !strcmp(renderer,"turnip") || !strcmp(renderer,"native_vulkan_turnip");
    char value[PROP_VALUE_MAX]={0};
    if(__system_property_get("debug.frostpunk.vulkan_driver",value)>0)return !strcmp(value,"turnip");
    char path[PATH_MAX],line[128];int turnip=0;snprintf(path,sizeof(path),"%s/port-settings.txt",c->profile);
    FILE *file=fopen(path,"re");if(!file)return 0;
    while(fgets(line,sizeof(line),file))if(!strncmp(line,"vulkan_driver=",14))turnip=!strncmp(line+14,"turnip",6);
    fclose(file);return turnip;
}
/* Turnip must create an instance and see a GPU; otherwise the system driver is used. */
static int turnip_usable(PFN_vkGetInstanceProcAddr get,char *error,size_t error_size) {
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)get(VK_NULL_HANDLE,"vkCreateInstance");
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance=VK_NULL_HANDLE;VkResult result=create?create(&info,NULL,&instance):VK_ERROR_INITIALIZATION_FAILED;
    if(result!=VK_SUCCESS){snprintf(error,error_size,"vkCreateInstance=%d",result);return 0;}
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)get(instance,"vkEnumeratePhysicalDevices");
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)get(instance,"vkDestroyInstance");
    uint32_t count=0;result=enumerate?enumerate(instance,&count,NULL):VK_ERROR_INITIALIZATION_FAILED;
    if(destroy)destroy(instance,NULL);
    if(result!=VK_SUCCESS||!count){snprintf(error,error_size,"vkEnumeratePhysicalDevices=%d devices=%u",result,count);return 0;}
    return 1;
}
/* Diagnostics only: debug.fp.env = "NAME=VALUE;NAME=VALUE" (e.g. TU_DEBUG=sysmem) is applied
 * before the Vulkan driver loads. Release behaviour without the property is unchanged. */
static void apply_debug_environment(void) {
    char value[PROP_VALUE_MAX]={0};
    if(__system_property_get("debug.fp.env",value)<=0||!value[0])return;
    char *save=NULL;
    for(char *item=strtok_r(value,"; ",&save);item;item=strtok_r(NULL,"; ",&save)){
        char *eq=strchr(item,'=');if(!eq||eq==item)continue;*eq=0;
        setenv(item,eq+1,1);
        char message[160];snprintf(message,sizeof(message),"FROSTPUNK_DEBUG_ENV %s=%s",item,eq+1);log_text(message);
    }
}
static PFN_vkGetInstanceProcAddr open_vulkan(app_context *c,char *error,size_t error_size) {
    const char *renderer=getenv("PORT_RENDERER");
    if(renderer&&(!strcmp(renderer,"vulkan-panvk")||!strcmp(renderer,"native_vulkan_panvk")||!strcmp(renderer,"dxvk_panvk"))){
        char cache[PATH_MAX], product[PROP_VALUE_MAX]={0};
        if(snprintf(cache,sizeof(cache),"%s/mesa-panvk-cache",c->private_path)<(int)sizeof(cache)&&
           (!mkdir(cache,0700)||errno==EEXIST))setenv("MESA_SHADER_CACHE_DIR",cache,0);
        setenv("PANVK_DEBUG","startup",0);
        setenv("MESA_LOG","file,android",0);
        setenv("MESA_LOG_LEVEL","info",0);
        const char *linear=getenv("MESA_GRALLOC_LINEAR");
        log_text(linear&&linear[0]&&linear[0]!='0'?"FROSTPUNK_PANVK_GRALLOC requested=linear":
            "FROSTPUNK_PANVK_GRALLOC requested=default");
        __system_property_get("ro.product.device",product);
        if(!strcmp(product,"TB373FU")){
            // The KCPU fence path can cause a Mali shareability fault on window resume.
            setenv("PANVK_KBASE_CPU_FENCE_EXPORT","1",0);
        }
        char reason[256]={0};
        PFN_vkGetInstanceProcAddr panvk=frostpunk_gpu_open_panvk(c->libraries,reason,sizeof(reason));
        if(panvk){
            setenv("FROSTPUNK_VULKAN_DRIVER","panvk",1);
            log_text("FROSTPUNK_VULKAN_DRIVER active=panvk requested=panvk");
            return panvk;
        }
        snprintf(error,error_size,"PanVK: %s",reason);
        setenv("FROSTPUNK_VULKAN_DRIVER","none",1);
        return NULL;
    }
    char reason[256]={0},message[512],hardware[PROP_VALUE_MAX]={0};const char *active="system";PFN_vkGetInstanceProcAddr get=NULL;
    int requested=turnip_requested(c);__system_property_get("ro.hardware.vulkan",hardware);
    int adreno=!strcmp(hardware,"adreno")&&!access("/dev/kgsl-3d0",R_OK|W_OK);
    /* A launcher selection must use the same driver that passed the format probe. */
    if(requested || (adreno && (!renderer || !*renderer))){
        if(!adreno)snprintf(reason,sizeof(reason),"not an Adreno/KGSL GPU (ro.hardware.vulkan=%s)",hardware);
        else if(!extract_driver(c))snprintf(reason,sizeof(reason),"driver asset extraction failed errno=%d",errno);
        else{
            /* Adreno 650 (2026-11-06): the KGSL GPU hangs came from the engine's tessellation,
             * which the runtime now disables on Turnip (win64.c). With it off, GMEM rendering ran
             * ~33 min of city without a GPU fault and 2-4% faster, while system-memory rendering
             * (TU_DEBUG=sysmem) still faulted twice and lost the device in ~30 min, so Turnip keeps
             * its default GMEM/autotune path. debug.fp.env (applied above) can set TU_DEBUG. */
            /* Mesa keeps compiled shaders only with a cache directory; apps have no HOME. */
            char cache[PATH_MAX];snprintf(cache,sizeof(cache),"%s/mesa-shader-cache",c->private_path);mkdir(cache,0700);
            setenv("MESA_SHADER_CACHE_DIR",cache,0);
            get=frostpunk_gpu_open_turnip(c->libraries,c->driver,reason,sizeof(reason));
        }
        if(get&&!turnip_usable(get,reason,sizeof(reason)))get=NULL;
        if(get){active="turnip";if(!requested)snprintf(reason,sizeof(reason),"the Qualcomm system driver has no BC textures");}
    }
    if(!get && requested)snprintf(error,error_size,"Turnip: %s",reason);
    else if(!get)get=frostpunk_gpu_open_system(error,error_size);
    setenv("FROSTPUNK_VULKAN_DRIVER",get?active:"none",1);
    snprintf(message,sizeof(message),"FROSTPUNK_VULKAN_DRIVER active=%s requested=%s%s%s",get?active:"none",
        requested?"turnip":"system",reason[0]?" reason=":"",reason);log_text(message);
    return get;
}
static void start_probe(app_context *c) {
    if(c->worker_started||!c->window||stopping(c))return;
    c->error[0]=0;if(!game_inputs_present(c)){log_text(c->error);report(c);return;}
    pthread_mutex_lock(&c->mutex);c->worker_started=1;pthread_mutex_unlock(&c->mutex);
    int status=pthread_create(&c->worker,NULL,game_worker,c);if(status){pthread_mutex_lock(&c->mutex);c->worker_started=0;snprintf(c->error,sizeof(c->error),"pthread_create game worker failed %d",status);pthread_mutex_unlock(&c->mutex);}report(c);
}
static void stop_game_worker(app_context *c) {
    pthread_mutex_lock(&c->mutex);c->stopping=1;++c->wake_sequence;pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->mutex);notify_foreground();pthread_mutex_lock(&c->mutex);
    struct timespec end;clock_gettime(CLOCK_MONOTONIC,&end);end.tv_sec+=2;
    while(c->worker_started&&!c->worker_finished){int status=pthread_cond_timedwait(&c->changed,&c->mutex,&end);if(status==ETIMEDOUT)break;}
    int started=c->worker_started,finished=c->worker_finished;pthread_mutex_unlock(&c->mutex);ALooper_wake(c->app->looper);
    /* The activity was destroyed (the player swiped the game away in Recents, or Android closed it):
     * the process ends anyway.  Status 74 made the launcher report "Game process stopped unexpectedly"
     * at the next start, which players read as a crash at launch; 0 is a normal end for GameWatchActivity. */
    if(started&&!finished&&(c->destroyed||c->app->destroyRequested)){log_text("FROSTPUNK_GAME_STOP_TIMEOUT after activity destroy; worker still active, quiet exit");fflush(NULL);_exit(0);}
    if(started&&!finished){log_text("FROSTPUNK_GAME_STOP_TIMEOUT worker still active; terminating own app process to preserve host lifetime");fflush(NULL);_exit(74);}
    if(started)pthread_join(c->worker,NULL);
}
#else
static void stop_probe(app_context *c){if(c->probe){c->total_frames+=window_probe_frames(c->probe);window_probe_stop(c->probe);c->probe=NULL;}}
static void start_probe(app_context *c) {
    if(!c->provider){if(!extract_driver(c)){snprintf(c->error,sizeof(c->error),"Driver asset extraction failed errno=%d",errno);report(c);return;}
        PFN_vkGetInstanceProcAddr proc=frostpunk_gpu_open(c->libraries,c->driver,c->error,sizeof(c->error));pthread_mutex_lock(&c->mutex);c->provider=proc;pthread_mutex_unlock(&c->mutex);}
    if(!c->provider){report(c);return;}
    uint64_t generation=0;ANativeWindow *window=acquire_window(c,&generation);
    if(window){
#ifdef FP_NATIVE_PRESENT
        /* DXVK renders the explicit 960x540 DXGI backbuffer, then scales it to
         * native Android presentation. Avoid reduced native-surface geometry. */
        int geometry=ANativeWindow_setBuffersGeometry(window,0,0,0);
#else
        int geometry=ANativeWindow_setBuffersGeometry(window,960,540,0);
#endif
        if(geometry){snprintf(c->error,sizeof(c->error),"ANativeWindow_setBuffersGeometry failed %d",geometry);release_window(window);report(c);return;}++c->acquire_checks;c->probe=window_probe_start(window,c->provider,c->error,sizeof(c->error));release_window(window);}
    if(c->probe)log_text("APK_WINDOW_VULKAN_READY real_surface=1 Vulkan13_BC=1 game_boot_proven=0");else log_text(c->error);report(c);
}
#endif
static void command(struct android_app *app,int32_t value) {
    app_context *c=app->userData;
    if(value==APP_CMD_INIT_WINDOW||value==APP_CMD_TERM_WINDOW){
        void (*block)(unsigned,int)=c->runtime_handle?dlsym(c->runtime_handle,"port_adpf_block"):NULL;
        if(block)block(2,value==APP_CMD_TERM_WINDOW);
    }
    if(value==APP_CMD_PAUSE){set_game_paused(c,1);c->focused=0;push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=0});}
    else if(value==APP_CMD_RESUME){set_game_paused(c,0);c->focused=activity_flag(c,"hasWindowFocus");push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=c->focused});}
    else if(value==APP_CMD_INIT_WINDOW){stop_probe(c);set_window(c,app->window);start_probe(c);}
    else if(value==APP_CMD_TERM_WINDOW){stop_probe(c);set_window(c,NULL);report(c);}
    else if(value==APP_CMD_WINDOW_RESIZED){stop_probe(c);resize_window(c,app->window);start_probe(c);}
    else if(value==APP_CMD_GAINED_FOCUS||value==APP_CMD_LOST_FOCUS){
        if(value==APP_CMD_LOST_FOCUS&&!c->paused&&(activity_flag(c,"keepNativeInputFocus")||native_editor_active(c)))return;
        c->focused=value==APP_CMD_GAINED_FOCUS;push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_FOCUS,.focused=c->focused});report(c);
#ifdef FP_POINTER_FIXTURE
        if(c->focused){c->pointer_fixture_visible=0;c->api.set_pointer_visible(c->api.context,0);}
#endif
    }
    else if(value==APP_CMD_DESTROY){push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_DESTROY});pthread_mutex_lock(&c->mutex);c->stopping=c->destroyed=1;pthread_cond_broadcast(&c->changed);pthread_mutex_unlock(&c->mutex);}
}
static int32_t input(struct android_app *app,AInputEvent *native) {
    app_context *c=app->userData;int type=AInputEvent_getType(native);
#ifdef FP_GAME_RUNTIME
    int source=AInputEvent_getSource(native);
    if((source&AINPUT_SOURCE_GAMEPAD)==AINPUT_SOURCE_GAMEPAD || (source&AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK) {
        if(type==AINPUT_EVENT_TYPE_KEY && AKeyEvent_getRepeatCount(native)>0)return 1;
        JNIEnv *env=NULL;JavaVM *vm=app->activity->vm;int attached=0;
        if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)==JNI_EDETACHED){if((*vm)->AttachCurrentThread(vm,&env,NULL)!=JNI_OK)return 0;attached=1;}
        if(!env)return 0;
        jclass cls=(*env)->GetObjectClass(env,app->activity->clazz);
        jmethodID method=(*env)->GetMethodID(env,cls,"onNativeGamepad","(IIII[F)V");
        jfloatArray axes=NULL;int action=0,key=-1;
        if(type==AINPUT_EVENT_TYPE_KEY){action=AKeyEvent_getAction(native);key=AKeyEvent_getKeyCode(native);}
        else if(type==AINPUT_EVENT_TYPE_MOTION){
            float values[7];const int names[]={AMOTION_EVENT_AXIS_X,AMOTION_EVENT_AXIS_Y,AMOTION_EVENT_AXIS_Z,AMOTION_EVENT_AXIS_RZ,AMOTION_EVENT_AXIS_LTRIGGER,AMOTION_EVENT_AXIS_RTRIGGER,AMOTION_EVENT_AXIS_HAT_Y};
            for(unsigned i=0;i<7;++i)values[i]=AMotionEvent_getAxisValue(native,names[i],0);
            axes=(*env)->NewFloatArray(env,7);if(axes)(*env)->SetFloatArrayRegion(env,axes,0,7,values);
        }
        if(method)(*env)->CallVoidMethod(env,app->activity->clazz,method,action,key,AInputEvent_getDeviceId(native),source,axes);
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionDescribe(env);(*env)->ExceptionClear(env);}
        if(axes)(*env)->DeleteLocalRef(env,axes);(*env)->DeleteLocalRef(env,cls);if(attached)(*vm)->DetachCurrentThread(vm);
        return 1;
    }
    /* NativeActivity's queue runs before its Views; let the Java gesture layer own fingers and pens. */
    if(type==AINPUT_EVENT_TYPE_MOTION){int source=AInputEvent_getSource(native);
        if((source&AINPUT_SOURCE_TOUCHSCREEN)==AINPUT_SOURCE_TOUCHSCREEN || (source&AINPUT_SOURCE_STYLUS)==AINPUT_SOURCE_STYLUS)return 0;}
#endif
    if(type==AINPUT_EVENT_TYPE_MOTION){int action=AMotionEvent_getAction(native),kind=action&AMOTION_EVENT_ACTION_MASK;size_t count=AMotionEvent_getPointerCount(native);
        size_t selected=(size_t)((action&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        for(size_t i=0;i<count;++i){if(kind!=AMOTION_EVENT_ACTION_MOVE&&kind!=AMOTION_EVENT_ACTION_CANCEL&&i!=selected)continue;
            push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_MOTION,.action=(uint32_t)kind,.source=(uint32_t)AInputEvent_getSource(native),.meta=(uint32_t)AMotionEvent_getMetaState(native),
                .buttons=(uint32_t)AMotionEvent_getButtonState(native),.time_ns=(uint64_t)AMotionEvent_getEventTime(native),.pointer_id=AMotionEvent_getPointerId(native,i),.width=c->physical_width,.height=c->physical_height,
                .x=AMotionEvent_getX(native,i),.y=AMotionEvent_getY(native,i),.scroll_x=AMotionEvent_getAxisValue(native,AMOTION_EVENT_AXIS_HSCROLL,i),.scroll_y=AMotionEvent_getAxisValue(native,AMOTION_EVENT_AXIS_VSCROLL,i)});}
        return 1;
    }
    if(type==AINPUT_EVENT_TYPE_KEY){
        int keycode=AKeyEvent_getKeyCode(native);
        /* Leave volume keys to Android's media-volume controls. */
        if(keycode==AKEYCODE_VOLUME_UP || keycode==AKEYCODE_VOLUME_DOWN || keycode==AKEYCODE_VOLUME_MUTE)return 0;
#ifdef FP_POINTER_FIXTURE
        /* Isolated probe flag only: F9 toggles through the actual host/JNI path.
         * Device verification must wait for each APPLIED log before next F9. */
        if(AKeyEvent_getKeyCode(native)==AKEYCODE_F9&&AKeyEvent_getAction(native)==AKEY_EVENT_ACTION_DOWN){c->pointer_fixture_visible=!c->pointer_fixture_visible;c->api.set_pointer_visible(c->api.context,c->pointer_fixture_visible);}
#endif
        push(c,(FrostpunkAndroidEvent){.type=FP_ANDROID_KEY,.action=(uint32_t)AKeyEvent_getAction(native),.source=(uint32_t)AInputEvent_getSource(native),
        .keycode=(uint32_t)AKeyEvent_getKeyCode(native),.scancode=(uint32_t)AKeyEvent_getScanCode(native),.meta=(uint32_t)AKeyEvent_getMetaState(native),.repeat=(uint32_t)AKeyEvent_getRepeatCount(native),.time_ns=(uint64_t)AKeyEvent_getEventTime(native)});
#ifndef FP_GAME_RUNTIME
        if(AKeyEvent_getKeyCode(native)==AKEYCODE_BACK&&AKeyEvent_getAction(native)==AKEY_EVENT_ACTION_UP)ANativeActivity_finish(app->activity);
#endif
        return 1;}
    return 0;
}
static void log_build_fingerprint(void *unused,const char *name,const char *value,uint32_t serial) {
    (void)unused;(void)name;(void)serial;
    fprintf(stderr,"ANDROID_BUILD_FINGERPRINT=%s\n",value);
}
static int open_native_log(const char *path) {
    int directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(directory<0)return -1;
    /* Keep one previous log when the append-only log exceeds 16 MiB. */
    struct stat info;
    if(!fstatat(directory,"native.log",&info,AT_SYMLINK_NOFOLLOW)&&S_ISREG(info.st_mode)&&info.st_size>16*1024*1024)
        renameat(directory,"native.log",directory,"native.log.1");
    int output=openat(directory,"native.log",O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    close(directory);
    if(output>=0&&(fstat(output,&info)||!S_ISREG(info.st_mode))){close(output);return -1;}
    return output;
}
void android_main(struct android_app *app) {
    app_context *c=calloc(1,sizeof(*c));if(!c)return;c->app=app;c->x=c->y=0.2f;
    pthread_mutex_init(&c->mutex,NULL);pthread_condattr_t attr;pthread_condattr_init(&attr);pthread_condattr_setclock(&attr,CLOCK_MONOTONIC);pthread_cond_init(&c->changed,&attr);pthread_condattr_destroy(&attr);
    pthread_mutex_lock(&current_mutex);current=c;pthread_mutex_unlock(&current_mutex);
    snprintf(c->private_path,sizeof(c->private_path),"%s",app->activity->internalDataPath);
    snprintf(c->profile,sizeof(c->profile),"%s/profile",c->private_path);mkdir(c->profile,0700);
    const char *game=getenv("PORT_PART_GAME");
    if(game && game[0]=='/')snprintf(c->game,sizeof(c->game),"%s",game);
    else snprintf(c->game,sizeof(c->game),"%s/game",app->activity->externalDataPath?app->activity->externalDataPath:c->private_path);
    snprintf(c->driver,sizeof(c->driver),"%s/driver/",c->private_path);
    crash_journal_install(c->private_path);
    int output=-1;
#ifdef FP_GAME_RUNTIME
    const char *log_directory=getenv("FROSTPUNK_NATIVE_LOG_DIR");
    if(log_directory&&log_directory[0]=='/')output=open_native_log(log_directory);
#endif
    if(output<0)output=open_native_log(c->private_path);
    if(output>=0){dup2(output,STDOUT_FILENO);dup2(output,STDERR_FILENO);close(output);setvbuf(stderr,NULL,_IOLBF,0);}
    library_path(c);
    c->api=(FrostpunkAndroidHost){.abi_version=1,.struct_size=sizeof(FrostpunkAndroidHost),.context=c,.acquire_window=acquire_window,.release_window=release_window,
        .poll_event=poll_event,.wait=wait_event,.wake=wake,.is_stopping=stopping,.set_status=set_status,.game_path=c->game,.profile_path=c->profile,.private_path=c->private_path,
        .native_library_path=c->libraries,.gpu_driver_path=c->driver,.vulkan_provider=provider,.set_pointer_visible=set_pointer_visible,.event_sequence=event_sequence};
    app->userData=c;app->onAppCmd=command;app->onInputEvent=input;log_text("APK_NATIVE_MAIN host_api=1 original_game_loaded=0 internet_permission=0");
    const prop_info *fingerprint=__system_property_find("ro.build.fingerprint");
    if(fingerprint)__system_property_read_callback(fingerprint,log_build_fingerprint,NULL);
    while(!app->destroyRequested&&!stopping(c)) {
        struct android_poll_source *source=NULL;int events=0;
        int ident=ALooper_pollOnce(c->probe&&c->focused?0:-1,NULL,&events,(void **)&source);
        if(ident==ALOOPER_POLL_ERROR){log_text("FROSTPUNK_HOST_FAIL ALooper_pollOnce");break;}if(source)source->process(app,source);
#ifdef FP_GAME_RUNTIME
        /* The translated USER32 message pump is the sole event consumer. */
        report(c);
#else
        FrostpunkAndroidEvent event;int changed=0;
        while(poll_event(c,&event,0)==1){++c->consumed;if(event.type==FP_ANDROID_MOTION){++c->touches;changed=1;
            int width=c->window?ANativeWindow_getWidth(c->window):1,height=c->window?ANativeWindow_getHeight(c->window):1;c->x=width>0?event.x/width:0;c->y=height>0?event.y/height:0;}
            if(event.type==FP_ANDROID_KEY){++c->keys;changed=1;}}
        if(c->probe&&c->focused){int result=window_probe_frame(c->probe,c->x,c->y);
            if(result==2){stop_probe(c);start_probe(c);}else if(result<0){snprintf(c->error,sizeof(c->error),"Vulkan present failed VkResult=%d",result);log_text(c->error);stop_probe(c);report(c);}
            else if(result==1&&window_probe_frames(c->probe)%60==0)changed=1;}
        if(changed)report(c);
#endif
    }
#ifdef FP_GAME_RUNTIME
    stop_game_worker(c);
#endif
    stop_probe(c);set_window(c,NULL);report(c);
    pthread_mutex_lock(&current_mutex);current=NULL;pthread_mutex_unlock(&current_mutex);
    pthread_mutex_destroy(&c->mutex);pthread_cond_destroy(&c->changed);free(c);
}
