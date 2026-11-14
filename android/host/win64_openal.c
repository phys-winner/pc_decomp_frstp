/* Typed Win64->nativeOpenAL boundary. Original AL entryVAs are data, never
 * patched/executed. Only the30observedC APIs are supported; all other public
 * originalAL/ALC entries stop, so native handles cannot enter the old mixer. */
#define _GNU_SOURCE
#include "win64_openal.h"
#include "win64.h"
#include "openal_original_entries.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
/* ALC_EXT_disconnect and AL_SOFTX_hold_on_disconnect of the pinned OpenAL Soft (AL/alext.h, alc/inprogext.h). */
#define FP_ALC_CONNECTED 0x313
#define FP_AL_STOP_SOURCES_ON_DISCONNECT_SOFT 0x19AB
_Static_assert(sizeof(ALint)==4&&sizeof(ALuint)==4&&sizeof(ALsizei)==4&&sizeof(ALenum)==4,"OpenAL32bit scalarABI");
_Static_assert(sizeof(ALboolean)==1&&sizeof(ALCboolean)==1&&sizeof(ALfloat)==4&&sizeof(ALdouble)==8&&sizeof(void*)==8,"OpenAL scalar/pointerABI");
typedef struct {
#define FIELD(name,type,va) type name;
FROSTPUNK_OPENAL_ROUTES(FIELD)
#undef FIELD
} native_functions;
enum {
#define OP(name,type,va) OP_##name,
FROSTPUNK_OPENAL_ROUTES(OP)
#undef OP
    OP_COUNT
};
typedef struct {const char*name;uint64_t va;} route;
static const route routes[]={
#define ROW(name,type,va) {#name,va},
FROSTPUNK_OPENAL_ROUTES(ROW)
#undef ROW
};
/* attributes: the list of the device's latest context (with its terminating zero), for a reopen. */
typedef struct device_record {ALCdevice*native;struct device_record*next;ALCint attributes[258];unsigned attribute_count,reconnects;uint64_t reopened_ms;} device_record;
typedef struct context_record {ALCcontext*native;ALCdevice*device;struct context_record*next;int held;} context_record;
typedef struct string_record {void*address;size_t bytes,mapping;struct string_record*next;} string_record;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static native_functions A;
static device_record*devices;static context_record*contexts;static string_record*strings;
static void*library;static int active;static char library_path[PATH_MAX],profile_path[PATH_MAX];
static void (*pause_device)(ALCdevice*),(*resume_device)(ALCdevice*);
static int audio_paused;
static void (*get_integer)(ALCdevice*,ALCenum,ALCsizei,ALCint*);
static ALCboolean (*reopen_device)(ALCdevice*,const ALCchar*,const ALCint*);
static void (*disable_capability)(ALenum);
static ALboolean (*extension_present)(const ALchar*);
static uint64_t checked_ms;
static void stop(const char*why){fprintf(stderr,"STOP Win64 OpenAL %s\n",why);fflush(stderr);_Exit(73);}
static uint64_t arg(CPU*c,unsigned n){return win64_argument(c,n);}
static const char*text(uint64_t p){return p?win64_string_a(p):NULL;}
static void*pointer(uint64_t p,size_t bytes,int write){return p?win64_pointer(p,bytes,write):NULL;}
static device_record**find_device(uint64_t p){device_record**s=&devices;while(*s&&(uint64_t)(uintptr_t)(*s)->native!=p)s=&(*s)->next;return s;}
static context_record**find_context(uint64_t p){context_record**s=&contexts;while(*s&&(uint64_t)(uintptr_t)(*s)->native!=p)s=&(*s)->next;return s;}
static ALCdevice*device(uint64_t p){if(!p)return NULL;device_record*r=*find_device(p);if(!r)stop("foreign/released opaque device");return r->native;}
static ALCcontext*context(uint64_t p){if(!p)return NULL;context_record*r=*find_context(p);if(!r)stop("foreign/released opaque context");return r->native;}
static uint64_t intern(const char*s,int list){
    if(!s)return 0;size_t bytes=0;const size_t limit=65536;
    if(list){if(!s[0]){static const char empty[2]={0,0};s=empty;bytes=2;}else{int ended=0;while(bytes+1<limit){if(!s[bytes]&&!s[bytes+1]){bytes+=2;ended=1;break;}++bytes;}if(!ended)stop("native device-list length exceeds bounded bridge");}}
    else{bytes=strnlen(s,limit);if(bytes==limit)stop("native string lacks bounded NUL");++bytes;}
    for(string_record*r=strings;r;r=r->next)if(r->bytes==bytes&&!memcmp(r->address,s,bytes))return(uintptr_t)r->address;
    long page=sysconf(_SC_PAGESIZE);if(page<=0||bytes>SIZE_MAX-(size_t)page)stop("native string mapping geometry");size_t mapping=(bytes+(size_t)page-1)/(size_t)page*(size_t)page;
    void*p=mmap(NULL,mapping,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p==MAP_FAILED)stop("native string allocation");memcpy(p,s,bytes);if(mprotect(p,mapping,PROT_READ))stop("native string readonly mapping");
    string_record*r=malloc(sizeof(*r));if(!r)stop("native string registry allocation");*r=(string_record){p,bytes,mapping,strings};strings=r;win64_region_add((uintptr_t)p,bytes,0);return(uintptr_t)p;
}
static int has_extension(const char*list,const char*name){
    if(!list)return 0;size_t n=strlen(name);
    for(const char*s=list;*s;){while(*s==' ')++s;const char*e=strchr(s,' ');if(!e)e=s+strlen(s);if((size_t)(e-s)==n&&!memcmp(s,name,n))return 1;s=e;}
    return 0;
}
static uint64_t extensions(const char*actual,int alc){
    if(!actual)return 0;static const char*const al_names[]={"AL_EXT_FLOAT32","AL_EXT_MCFORMATS"};static const char*const alc_names[]={"ALC_ENUMERATION_EXT","ALC_ENUMERATE_ALL_EXT"};char result[128]={0};const char*const*names=alc?alc_names:al_names;
    for(unsigned i=0;i<2;++i)if(has_extension(actual,names[i])){if(*result)strcat(result," ");strcat(result,names[i]);}
    return intern(result,0);
}
static unsigned vector_count(ALenum p,int listener){
    if(p==AL_POSITION||p==AL_VELOCITY||(!listener&&p==AL_DIRECTION))return 3;
    if(listener&&p==AL_ORIENTATION)return 6;
    if(p==AL_GAIN)return 1;
    if(!listener)switch(p){case AL_PITCH:case AL_MIN_GAIN:case AL_MAX_GAIN:case AL_REFERENCE_DISTANCE:case AL_MAX_DISTANCE:case AL_ROLLOFF_FACTOR:case AL_CONE_INNER_ANGLE:case AL_CONE_OUTER_ANGLE:case AL_CONE_OUTER_GAIN:case AL_SEC_OFFSET:case AL_SAMPLE_OFFSET:case AL_BYTE_OFFSET:return 1;}
    stop("unknown vector parameter requires explicit element-count contract");return 0;
}
static void*array(uint64_t p,ALsizei count,int write){
    if(count<=0)return NULL;
    if(!p)stop("NULL array with positive element count");
    if((uint64_t)(uint32_t)count>SIZE_MAX/sizeof(ALuint))stop("array byte-count overflow");
    return pointer(p,(size_t)(uint32_t)count*sizeof(ALuint),write);
}
static int write_config(const char*root,char*full){
    static const char contents[]="# Frostpunk static-recompilation native audio\n[general]\ndrivers = oboe\n";
    const char*name="frostpunk-staticrecomp-openal-1.conf";int dir=open(root,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return 0;
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd>=0){size_t done=0;while(done<sizeof(contents)-1){ssize_t n=write(fd,contents+done,sizeof(contents)-1-done);if(n<=0){close(fd);close(dir);return 0;}done+=(size_t)n;}if(close(fd)){close(dir);return 0;}}
    else if(errno==EEXIST){fd=openat(dir,name,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC);if(fd<0){close(dir);return 0;}struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size!=sizeof(contents)-1){close(fd);close(dir);return 0;}char buffer[sizeof(contents)];ssize_t n=read(fd,buffer,sizeof(buffer));int good=n==sizeof(contents)-1&&!memcmp(buffer,contents,sizeof(contents)-1);close(fd);if(!good){close(dir);return 0;}}
    else{close(dir);return 0;}
    close(dir);int n=snprintf(full,PATH_MAX,"%s/%s",root,name);return n>0&&n<PATH_MAX;
}
int win64_openal_configure(const char*dso,const char*root){
    char path[PATH_MAX],profile[PATH_MAX],config[PATH_MAX];if(!dso||!root||!realpath(dso,path)||!realpath(root,profile))return 0;
    pthread_mutex_lock(&mutex);
    if(library){int same=!strcmp(path,library_path)&&!strcmp(profile,profile_path);if(same)active=1;pthread_mutex_unlock(&mutex);return same;}
    if(!write_config(profile,config)){pthread_mutex_unlock(&mutex);return 0;}
    if(setenv("ALSOFT_CONF",config,1)){pthread_mutex_unlock(&mutex);return 0;}
    void*loaded=dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_NODELETE);if(!loaded){fprintf(stderr,"OPENAL_CONFIGURE dlopen: %s\n",dlerror());pthread_mutex_unlock(&mutex);return 0;}
    native_functions functions={0};int missing=0;
#define LOAD(name,type,va) do{void*p=dlsym(loaded,#name);if(!p)missing=1;_Static_assert(sizeof(functions.name)==sizeof(p),"native function-pointer ABI");memcpy(&functions.name,&p,sizeof(p));}while(0);
FROSTPUNK_OPENAL_ROUTES(LOAD)
#undef LOAD
    if(missing){dlclose(loaded);pthread_mutex_unlock(&mutex);return 0;}
    pause_device=(void(*)(ALCdevice*))dlsym(loaded,"alcDevicePauseSOFT");
    resume_device=(void(*)(ALCdevice*))dlsym(loaded,"alcDeviceResumeSOFT");
    if(!pause_device||!resume_device){dlclose(loaded);pthread_mutex_unlock(&mutex);return 0;}
    /* Optional: without them a disconnected output stays silent until the game restarts.
     * alcReopenDeviceSOFT is not an exported symbol; the first opened device resolves it. */
    get_integer=(void(*)(ALCdevice*,ALCenum,ALCsizei,ALCint*))dlsym(loaded,"alcGetIntegerv");
    reopen_device=NULL;
    disable_capability=(void(*)(ALenum))dlsym(loaded,"alDisable");
    extension_present=(ALboolean(*)(const ALchar*))dlsym(loaded,"alIsExtensionPresent");
    A=functions;library=loaded;strcpy(library_path,path);strcpy(profile_path,profile);active=1;pthread_mutex_unlock(&mutex);return 1;
}
static uint32_t output_list_count(const char*list){
    if(!list)return 0;size_t offset=0;uint32_t count=0;
    while(offset<65536){size_t n=strnlen(list+offset,65536-offset);if(n==65536-offset)stop("native output list exceeds64KiB");if(!n)return count;++count;offset+=n+1;}
    stop("native output list lacks final empty entry");return 0;
}
static uint32_t native_output_count(void){
    if(!A.alcIsExtensionPresent(NULL,"ALC_ENUMERATE_ALL_EXT"))stop("native all-output enumeration unavailable");
    return output_list_count(A.alcGetString(NULL,0x1013));
}
int win64_openal_winmm_call(CPU*c,const char*name){
    int count_query=!strcmp(name,"waveOutGetNumDevs"),caps_query=!strcmp(name,"waveOutGetDevCapsW");
    if(!count_query&&!caps_query)return 0;
    /* shortcut: native endpoint inventory only; no Windows wave playback
     * driver exists, so capabilities return NODRIVER without fabricated data. */
    int saved_errno=errno;
    if(caps_query&&(uint32_t)arg(c,2)==0){c->rax=0;errno=saved_errno;return 1;}
    pthread_mutex_lock(&mutex);
    if(!active){pthread_mutex_unlock(&mutex);errno=saved_errno;return 0;}
    uint32_t count=native_output_count();
    uint64_t id=count_query?0:arg(c,0);
    c->rax=count_query?count:(id==UINT32_MAX||id==UINT64_MAX)?6:id>=count?2:6;
    pthread_mutex_unlock(&mutex);errno=saved_errno;return 1;
}
/* An output route change (a Bluetooth headset arriving or leaving, a plug, an audio server
 * restart) disconnects the AAudio stream, and OpenAL Soft then keeps the device disconnected:
 * no sound until the game restarts, and every source stops the moment it is played, which the
 * guest sound thread answers by decoding its streams again without a pause (83 % of a core
 * measured on the test tablet).  The 2018 game predates ALC_SOFT_reopen_device, so the bridge
 * does what a current OpenAL application does itself: sources are held instead of stopped while
 * the output is gone (hold_sources), and the device is reopened on the default output with the
 * attributes of its context.  Looked at from guest audio calls, at most five times a second; a
 * failed reopen is repeated once a second.  Both run under the bridge mutex. */
static uint64_t monotonic_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return(uint64_t)t.tv_sec*1000u+(uint64_t)(t.tv_nsec/1000000);}
static void hold_sources(context_record*r){
    if(!r||r->held)return;r->held=1;
    if(disable_capability&&extension_present&&extension_present("AL_SOFTX_hold_on_disconnect"))disable_capability(FP_AL_STOP_SOURCES_ON_DISCONNECT_SOFT);
}
static void reconnect_outputs(void){
    if(audio_paused||!devices||!get_integer||!reopen_device)return;
    uint64_t now=monotonic_ms();if(!now||now-checked_ms<200)return;checked_ms=now;
    int saved_errno=errno;
    for(device_record*r=devices;r;r=r->next){
        ALCint connected=ALC_TRUE;get_integer(r->native,FP_ALC_CONNECTED,1,&connected);
        if(connected||(r->reopened_ms&&now-r->reopened_ms<1000))continue;
        r->reopened_ms=now;
        ALCboolean reopened=reopen_device(r->native,NULL,r->attribute_count?r->attributes:NULL);
        if(!reopened)(void)A.alcGetError(r->native); /* the bridge's own attempt is not a guest error */
        fprintf(stderr,"OPENAL_RECONNECT reopened=%d attempt=%u\n",(int)reopened,++r->reconnects);
    }
    errno=saved_errno;
}
int win64_openal_dispatch(CPU*c,uint64_t target){
    /* Ordinary guest dispatch must pay only this range check. */
    if(target<FROSTPUNK_OPENAL_MIN_VA||target>FROSTPUNK_OPENAL_MAX_VA)return 0;
    pthread_mutex_lock(&mutex);
    unsigned low=0,high=FROSTPUNK_OPENAL_ENTRY_COUNT;while(low<high){unsigned mid=low+(high-low)/2;if(original_openal_entries[mid]<target)low=mid+1;else high=mid;}
    if(low==FROSTPUNK_OPENAL_ENTRY_COUNT||original_openal_entries[low]!=target){pthread_mutex_unlock(&mutex);return 0;}
    if(!active){if(library)stop("public entry after native audio shutdown");pthread_mutex_unlock(&mutex);return 0;}
    unsigned which;for(which=0;which<OP_COUNT;++which)if(routes[which].va==target)break;
    if(which==OP_COUNT)stop("unimplemented original public entry; refusing native/AOT context mixing");
    reconnect_outputs();
    uint64_t a=arg(c,0),b=arg(c,1),d=arg(c,2);ALsizei n;
    switch(which){
    case OP_alBufferData:{ALsizei size=(ALsizei)arg(c,3);A.alBufferData((ALuint)a,(ALenum)b,size<0?NULL:pointer(d,(size_t)size,0),size,(ALsizei)arg(c,4));break;}
    case OP_alDeleteBuffers:n=(ALsizei)a;A.alDeleteBuffers(n,array(b,n,0));break;
    case OP_alDeleteSources:n=(ALsizei)a;A.alDeleteSources(n,array(b,n,0));break;
    case OP_alDistanceModel:A.alDistanceModel((ALenum)a);break;
    case OP_alGenBuffers:n=(ALsizei)a;A.alGenBuffers(n,array(b,n,1));break;
    case OP_alGenSources:n=(ALsizei)a;A.alGenSources(n,array(b,n,1));break;
    case OP_alGetError:c->rax=(uint32_t)A.alGetError();break;
    case OP_alGetListener3f:A.alGetListener3f((ALenum)a,pointer(b,4,1),pointer(d,4,1),pointer(arg(c,3),4,1));break;
    case OP_alGetListenerfv:A.alGetListenerfv((ALenum)a,pointer(b,vector_count((ALenum)a,1)*4,1));break;
    case OP_alGetSourcei:A.alGetSourcei((ALuint)a,(ALenum)b,pointer(d,4,1));break;
    case OP_alGetString:{const char*s=A.alGetString((ALenum)a);c->rax=a==AL_EXTENSIONS?extensions(s,0):intern(s,0);break;}
    case OP_alListenerfv:A.alListenerfv((ALenum)a,pointer(b,vector_count((ALenum)a,1)*4,0));break;
    case OP_alSourcePlay:A.alSourcePlay((ALuint)a);break;
    case OP_alSourceQueueBuffers:n=(ALsizei)b;A.alSourceQueueBuffers((ALuint)a,n,array(d,n,0));break;
    case OP_alSourceStop:A.alSourceStop((ALuint)a);break;
    case OP_alSourceUnqueueBuffers:n=(ALsizei)b;A.alSourceUnqueueBuffers((ALuint)a,n,array(d,n,1));break;
    case OP_alSourcef:A.alSourcef((ALuint)a,(ALenum)b,c->xmm[2].f32[0]);break;
    case OP_alSourcefv:A.alSourcefv((ALuint)a,(ALenum)b,pointer(d,vector_count((ALenum)b,0)*4,0));break;
    case OP_alSourcei:A.alSourcei((ALuint)a,(ALenum)b,(ALint)d);break;
    case OP_alcOpenDevice:{ALCdevice*value=A.alcOpenDevice(text(a));if(value){if(*find_device((uintptr_t)value))stop("duplicate native device identity");device_record*r=malloc(sizeof(*r));if(!r)stop("native device registry allocation");*r=(device_record){.native=value,.next=devices};devices=r;if(audio_paused)pause_device(value);if(!reopen_device){void*found=A.alcGetProcAddress(value,"alcReopenDeviceSOFT");memcpy(&reopen_device,&found,sizeof(found));}}c->rax=(uintptr_t)value;break;}
    case OP_alcCloseDevice:{ALCdevice*value=device(a);ALCboolean ok=A.alcCloseDevice(value);if(ok&&value){context_record**slot=&contexts;while(*slot){context_record*r=*slot;if(r->device==value){*slot=r->next;free(r);}else slot=&r->next;}device_record**device_slot=find_device(a),*old=*device_slot;*device_slot=old->next;free(old);}c->rax=(uint8_t)ok;break;}
    case OP_alcCreateContext:{ALCdevice*value=device(a);const ALCint*attrs=NULL;unsigned count=0;if(b){if(b>UINT64_MAX-256*4)stop("context attribute pointer overflow");unsigned words;for(words=0;words<256;words+=2){ALCint key;memcpy(&key,win64_pointer(b+words*4,4,0),4);if(!key)break;win64_pointer(b+(words+1)*4,4,0);}if(words==256)stop("unterminated context attribute list");attrs=win64_pointer(b,(words+1)*4,0);count=words+1;}ALCcontext*made=A.alcCreateContext(value,attrs);if(made){if(*find_context((uintptr_t)made))stop("duplicate native context identity");context_record*r=malloc(sizeof(*r));if(!r)stop("native context registry allocation");*r=(context_record){.native=made,.device=value,.next=contexts};contexts=r;device_record*owner=*find_device(a);if(owner){owner->attribute_count=count;if(count)memcpy(owner->attributes,attrs,count*sizeof(ALCint));}}c->rax=(uintptr_t)made;break;}
    case OP_alcDestroyContext:{ALCcontext*value=context(a);A.alcDestroyContext(value);if(value){context_record**slot=find_context(a),*old=*slot;*slot=old->next;free(old);}break;}
    case OP_alcMakeContextCurrent:{ALCcontext*value=context(a);ALCboolean ok=A.alcMakeContextCurrent(value);if(ok&&value)hold_sources(*find_context(a));c->rax=(uint8_t)ok;break;}
    case OP_alcProcessContext:A.alcProcessContext(context(a));break;
    case OP_alcSuspendContext:A.alcSuspendContext(context(a));break;
    case OP_alcGetError:c->rax=(uint32_t)A.alcGetError(device(a));break;
    case OP_alcGetProcAddress:{ALCdevice*value=device(a);const char*name=text(b);void*found=A.alcGetProcAddress(value,name);c->rax=0;if(name&&found&&strcmp(name,"alBufferDataStatic")){for(unsigned i=0;i<OP_COUNT;++i)if(!strcmp(name,routes[i].name)){c->rax=routes[i].va;break;}}break;}
    case OP_alcGetString:{ALCdevice*value=device(a);const char*s=A.alcGetString(value,(ALCenum)b);int list=!a&&(b==ALC_DEVICE_SPECIFIER||b==0x1013||b==ALC_CAPTURE_DEVICE_SPECIFIER);c->rax=b==ALC_EXTENSIONS?extensions(s,1):intern(s,list);break;}
    case OP_alcIsExtensionPresent:{ALCdevice*value=device(a);const char*name=text(b);ALCboolean native=A.alcIsExtensionPresent(value,name);c->rax=name&&(!strcasecmp(name,"ALC_ENUMERATION_EXT")||!strcasecmp(name,"ALC_ENUMERATE_ALL_EXT"))?(uint8_t)native:0;break;}
    default:stop("typed route opcode mismatch");
    }
    pthread_mutex_unlock(&mutex);return 1;
}
void win64_openal_shutdown(void){
    pthread_mutex_lock(&mutex);if(active){A.alcMakeContextCurrent(NULL);while(contexts){context_record*r=contexts;contexts=r->next;A.alcDestroyContext(r->native);free(r);}while(devices){device_record*r=devices;devices=r->next;if(!A.alcCloseDevice(r->native))stop("native device close failed at shutdown");free(r);}while(strings){string_record*r=strings;strings=r->next;win64_region_remove((uintptr_t)r->address);munmap(r->address,r->mapping);free(r);}active=0;}pthread_mutex_unlock(&mutex);
}
void win64_openal_pause(int paused){
    pthread_mutex_lock(&mutex);
    if(audio_paused!=!!paused){
        audio_paused=!!paused;
        if(active)for(device_record*r=devices;r;r=r->next)(paused?pause_device:resume_device)(r->native);
        fprintf(stderr,"OPENAL_LIFECYCLE paused=%d\n",audio_paused);
    }
    pthread_mutex_unlock(&mutex);
}
void win64_openal_self_test(void){
    CPU c,before;memset(&c,0xa5,sizeof(c));before=c;if(win64_openal_dispatch(&c,0)||win64_openal_dispatch(&c,UINT64_MAX)||memcmp(&c,&before,sizeof(c)))stop("unrelated target/CPU preservation self-test");
    if(FROSTPUNK_OPENAL_ENTRY_COUNT!=157||OP_COUNT!=30)stop("sealed original entry/typed route count");
    for(unsigned i=1;i<FROSTPUNK_OPENAL_ENTRY_COUNT;++i)if(original_openal_entries[i]<=original_openal_entries[i-1])stop("original entry guard sort/duplicate");
    static const char two[]="Native endpoint A\0Native endpoint B\0";
    if(output_list_count(NULL)||output_list_count("")||output_list_count(two)!=2)stop("native output list count self-test");
    c=before;if(win64_openal_winmm_call(&c,"waveOutOpen")||memcmp(&c,&before,sizeof(c)))stop("unavailable WinMM API self-test");
    c.r8=0;before=c;int saved_errno=errno;errno=1927;
    if(!win64_openal_winmm_call(&c,"waveOutGetDevCapsW")||c.rax||errno!=1927)stop("zero-capacity WinMM query self-test");
    c.rax=before.rax;if(memcmp(&c,&before,sizeof(c)))stop("WinMM CPU/no-pop self-test");errno=saved_errno;
    fprintf(stderr,"OPENAL self-test original_entries=157 typed_routes=30 unrelated_CPU_preserved=1 winmm_inventory=1 winmm_capabilities_unavailable=1 PASS\n");
}
