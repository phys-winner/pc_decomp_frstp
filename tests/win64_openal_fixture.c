/* Synthetic Win64 argument/CPU fixture invoking a real native AAudio device. */
#include "win64_openal.h"
#include "win64.h"
#include "openal_original_entries.h"
#include "metrics.h"
#include <AL/al.h>
#include <AL/alc.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef struct {uint64_t p;size_t n;int write;} region;
static region regions[256];static unsigned nregions,calls,checks,seen[30];
typedef struct {const char*name;uint64_t va;} route;
static const route routes[]={
#define ROW(name,type,va) {#name,va},
FROSTPUNK_OPENAL_ROUTES(ROW)
#undef ROW
};
static void check(int condition,const char*what){++checks;if(!condition){fprintf(stderr,"OPENAL_FIXTURE_FAIL %s calls%u\n",what,calls);fflush(stderr);_Exit(1);}}
static int fifo_input=-1,fifo_opened=-1;
static unsigned fifo_opens,fifo_stats,named_fifo_case,pipe_fifo_case;
int __real_openat(int directory,const char *path,int flags,...);
int __real_fstat(int fd,struct stat *info);
ssize_t __real_read(int fd,void *buffer,size_t bytes);
int __wrap_openat(int directory,const char *path,int flags,...){
    mode_t mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=(mode_t)va_arg(args,int);va_end(args);}
    if(fifo_input>=0&&!strcmp(path,"frostpunk-staticrecomp-openal-1.conf")){
        ++fifo_opens;
        if(flags&O_CREAT){check(flags==(O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC),"FIFO exclusive-create flags");errno=EEXIST;return -1;}
        check(flags==(O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC),"FIFO read must be nonblocking and no-follow");
        fifo_opened=fcntl(fifo_input,F_DUPFD_CLOEXEC,0);check(fifo_opened>=0,"duplicate real pipe read end");
        int status=fcntl(fifo_opened,F_GETFL);check(status>=0&&!fcntl(fifo_opened,F_SETFL,status|O_NONBLOCK),"real pipe is nonblocking");return fifo_opened;
    }
    return __real_openat(directory,path,flags,mode);
}
int __wrap_fstat(int fd,struct stat *info){int result=__real_fstat(fd,info);if(fifo_opened>=0&&fd==fifo_opened){check(!result&&S_ISFIFO(info->st_mode),"production fstat sees a real FIFO");++fifo_stats;}return result;}
ssize_t __wrap_read(int fd,void *buffer,size_t bytes){check(fifo_opened<0||fd!=fifo_opened,"FIFO rejected before reading");return __real_read(fd,buffer,bytes);}
void win64_region_add(uint64_t p,size_t n,int write){check(nregions<256,"region capacity");regions[nregions++]=(region){p,n,write};}
void win64_region_remove(uint64_t p){for(unsigned i=0;i<nregions;++i)if(regions[i].p==p){regions[i]=regions[--nregions];return;}check(0,"remove registered region");}
void*win64_pointer(uint64_t p,size_t n,int write){for(unsigned i=0;i<nregions;++i)if(p>=regions[i].p&&p-regions[i].p<=regions[i].n&&n<=regions[i].n-(p-regions[i].p)&&(!write||regions[i].write))return(void*)(uintptr_t)p;fprintf(stderr,"OPENAL_FIXTURE_INVALID_RANGE %llx+%zu write%d\n",(unsigned long long)p,n,write);fflush(stderr);_Exit(74);}
const char*win64_string_a(uint64_t p){for(size_t i=0;i<65536;++i){const char*s=win64_pointer(p+i,1,0);if(!*s)return win64_pointer(p,i+1,0);}check(0,"bounded guest string");return NULL;}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t r[]={c->rcx,c->rdx,c->r8,c->r9};return r[i];}uint64_t v;memcpy(&v,win64_pointer(c->rsp+40+(i-4)*8,8,0),8);return v;}
static unsigned index_for(const char*name){for(unsigned i=0;i<30;++i)if(!strcmp(routes[i].name,name))return i;check(0,"typed route name");return 0;}
static uint64_t call(const char*name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t fifth,float value){
    CPU c,before;uint64_t stack[16];memset(stack,0xcc,sizeof(stack));stack[5]=fifth;win64_region_add((uintptr_t)stack,sizeof(stack),1);
    memset(&c,0xa5,sizeof(c));c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;c.rsp=(uintptr_t)stack;c.xmm[0].f32[0]=-987.0f;c.xmm[2].f32[0]=value;before=c;
    unsigned i=index_for(name);check(win64_openal_dispatch(&c,routes[i].va),"exact original entry handled");uint64_t result=c.rax;
    int returns=!strcmp(name,"alGetError")||!strcmp(name,"alGetString")||(!strncmp(name,"alc",3)&&strcmp(name,"alcDestroyContext")&&strcmp(name,"alcSuspendContext")&&strcmp(name,"alcProcessContext"));
    if(returns)c.rax=before.rax;check(!memcmp(&c,&before,sizeof(c)),"all CPU state and guest RSP preserved except ABI result");
    win64_region_remove((uintptr_t)stack);++calls;++seen[i];return result;
}
#define C(name,a,b,d,e) call(#name,(uint64_t)(uintptr_t)(a),(uint64_t)(uintptr_t)(b),(uint64_t)(uintptr_t)(d),(uint64_t)(uintptr_t)(e),0,0)
static const char*guest_string(const char*s){win64_region_add((uintptr_t)s,strlen(s)+1,0);return s;}
static void delay(unsigned ms){struct timespec t={ms/1000,(long)(ms%1000)*1000000};while(nanosleep(&t,&t));}
static void*symbol(void*handle,const char*name){void*p=dlsym(handle,name);check(p!=NULL,name);return p;}
#define LOAD(type,name) type name; do{void*p=symbol(handle,#name);memcpy(&name,&p,sizeof(p));}while(0)
typedef struct{uint32_t count;unsigned calls,failed;} winmm_worker;
static int winmm_call(const char*name,uint64_t id,uint64_t output,uint64_t bytes,uint64_t expected){
    CPU c,before;uint64_t stack[8],saved_stack[8];memset(&c,0xa5,sizeof(c));memset(stack,0xcc,sizeof(stack));memcpy(saved_stack,stack,sizeof(stack));
    c.rcx=id;c.rdx=output;c.r8=bytes;c.rsp=(uintptr_t)stack;before=c;before.rax=expected;
    errno=1927;int handled=win64_openal_winmm_call(&c,name);
    return handled&&errno==1927&&!memcmp(&c,&before,sizeof(c))&&!memcmp(stack,saved_stack,sizeof(stack));
}
static void*winmm_queries(void*opaque){
    winmm_worker*w=opaque;
    for(unsigned i=0;i<256;++i){
        if(!winmm_call("waveOutGetNumDevs",UINT64_MAX,UINT64_MAX,UINT64_MAX,w->count)||
           !winmm_call("waveOutGetDevCapsW",0,UINT64_MAX,84,6)||
           !winmm_call("waveOutGetDevCapsW",(uint64_t)w->count+19,1,84,2)||
           !winmm_call("waveOutGetDevCapsW",UINT64_MAX,UINT64_MAX,UINT64_C(0x1234567800000000),0))w->failed=1;
        w->calls+=4;
    }
    return NULL;
}
static unsigned winmm_inventory_test(uint64_t ctx,LPALCGETSTRING get_string,LPALCGETERROR get_error,LPALCGETCURRENTCONTEXT current,uint32_t*inventory){
    const char*list=get_string(NULL,0x1013);check(list!=NULL,"real native all-output inventory");uint32_t count=0;size_t length=0;
    for(const char*s=list;*s;s+=strlen(s)+1){length+=strlen(s)+1;check(length<65536,"bounded oracle inventory");++count;}
    check(count>0,"AAudio fixture endpoint exists");*inventory=count;unsigned char caps[116],saved[116];memset(caps,0xa5,sizeof(caps));memcpy(saved,caps,sizeof(caps));
    for(unsigned repeat=0;repeat<2;++repeat){
        check(winmm_call("waveOutGetNumDevs",UINT64_MAX,1,UINT64_MAX,count),"WinMM count is actual inventory/full CPU");
        for(unsigned cb=1;cb<=85;++cb)check(winmm_call("waveOutGetDevCapsW",0,(uintptr_t)caps+16,cb,6)&&!memcmp(caps,saved,sizeof(caps)),"no Windows caps driver/no writes at every size");
        const uint64_t ids[]={0,UINT32_MAX,UINT64_MAX,UINT64_C(0x100000000),(uint64_t)count+19};
        for(unsigned n=0;n<sizeof(ids)/sizeof(*ids);++n){
            check(winmm_call("waveOutGetDevCapsW",ids[n],UINT64_MAX,UINT64_C(0x1234567800000000),0),"cb0 precedes pointer/ID validation including both mapper widths");
            if(n)check(winmm_call("waveOutGetDevCapsW",ids[n],(uintptr_t)caps+16,84,n<3?6:2),"mapper has no driver/unknown device has no guest handle");
        }
    }
    CPU c,before;memset(&c,0xa5,sizeof(c));before=c;errno=1927;
    check(!win64_openal_winmm_call(&c,"waveOutOpen")&&errno==1927&&!memcmp(&c,&before,sizeof(c)),"unknown WinMM APIs remain unhandled and unchanged");
    check(!get_error(NULL),"native global ALC error initially clear");(void)get_string(NULL,-12345);
    check(winmm_call("waveOutGetNumDevs",0,0,0,count)&&get_error(NULL)==ALC_INVALID_ENUM,"inventory does not clear native ALC error");
    pthread_t threads[4];winmm_worker workers[4]={0};unsigned total=0;
    for(unsigned i=0;i<4;++i){workers[i].count=count;check(!pthread_create(&threads[i],NULL,winmm_queries,&workers[i]),"WinMM query thread");}
    for(unsigned i=0;i<4;++i){check(!pthread_join(threads[i],NULL)&&!workers[i].failed,"concurrent real inventory/caps queries");total+=workers[i].calls;}
    check((uintptr_t)current()==ctx&&!get_error(NULL)&&!memcmp(caps,saved,sizeof(caps)),"native context identity/errors and all output canaries survive queries");
    return total;
}
static void config_negative(const char*dso,const char*root){
    char directory[4096],name[4096],target[4096];check(snprintf(directory,sizeof(directory),"%s/config-negative-XXXXXX",root)>0,"temporary config path");check(mkdtemp(directory)!=NULL,"exclusive config regression directory");
    check(snprintf(name,sizeof(name),"%s/frostpunk-staticrecomp-openal-1.conf",directory)>0,"config leaf path");check(snprintf(target,sizeof(target),"%s/target",directory)>0,"config symlink target path");
    static const char canary[]="Fixture user config must survive unchanged\n";char buffer[sizeof(canary)];int fd=open(name,O_WRONLY|O_CREAT|O_EXCL,0600);check(fd>=0&&write(fd,canary,sizeof(canary))==sizeof(canary)&&!close(fd),"exclusive conflicting config creation");
    check(!win64_openal_configure(dso,directory),"conflicting existing config rejected");fd=open(name,O_RDONLY);check(fd>=0&&read(fd,buffer,sizeof(buffer))==sizeof(buffer)&&!memcmp(buffer,canary,sizeof(buffer))&&!close(fd),"conflicting config bytes preserved");check(!rename(name,target)&&!symlink("target",name),"symlink config setup");
    check(!win64_openal_configure(dso,directory),"config symlink rejected");fd=open(target,O_RDONLY);check(fd>=0&&read(fd,buffer,sizeof(buffer))==sizeof(buffer)&&!memcmp(buffer,canary,sizeof(buffer))&&!close(fd),"symlink target bytes preserved");
    check(!unlink(name)&&!unlink(target),"own symlink regression cleanup");
    if(!mkfifo(name,0600)){
        named_fifo_case=1;check(!win64_openal_configure(dso,directory),"named config FIFO rejected without blocking");check(!unlink(name),"own FIFO cleanup");
    }else{
        check(errno==EPERM||errno==EACCES||errno==EOPNOTSUPP||errno==ENOSYS,"unexpected named FIFO setup failure");
        // Android shell SELinux may forbid named FIFOs; inject a real pipe only at openat.
        int descriptors[2];check(!pipe(descriptors),"real FIFO pipe creation");fifo_input=descriptors[0];
        check(!win64_openal_configure(dso,directory),"real pipe FIFO rejected without reading");
        check(fifo_opens==2&&fifo_stats==1,"production opens and fstats FIFO");
        fifo_input=fifo_opened=-1;check(!close(descriptors[0])&&!close(descriptors[1]),"own pipe cleanup");pipe_fifo_case=1;
    }
    check(!rmdir(directory),"own config regression cleanup");
}
int main(int argc,char**argv){
    check(argc>=3,"DSO/profile arguments");win64_openal_self_test();
    const char*home=getenv("HOME"),*tz=getenv("TZ");char*oldhome=home?strdup(home):NULL,*oldtz=tz?strdup(tz):NULL;
    config_negative(argv[1],argv[2]);
    check(win64_openal_configure(argv[1],argv[2]),"native backend configure");check(win64_openal_configure(argv[1],argv[2]),"idempotent pinned configure");
    check((oldhome&&getenv("HOME")&&!strcmp(oldhome,getenv("HOME")))||(!oldhome&&!getenv("HOME")),"host HOME unchanged");check((oldtz&&getenv("TZ")&&!strcmp(oldtz,getenv("TZ")))||(!oldtz&&!getenv("TZ")),"host TZ unchanged");free(oldhome);free(oldtz);
    check(!win64_openal_configure("/does/not/exist",argv[2]),"missing DSO not accepted");
    if(argc>3){CPU c={0};const char*mode=argv[3];
        if(!strcmp(mode,"unsupported"))win64_openal_dispatch(&c,UINT64_C(0x141bbf490));
        if(!strcmp(mode,"alias"))win64_openal_dispatch(&c,UINT64_C(0x141bb5880));
        if(!strcmp(mode,"foreign"))C(alcCloseDevice,123,0,0,0);
        if(!strcmp(mode,"attr-overflow"))C(alcCreateContext,0,UINT64_MAX-8,0,0);
        if(!strcmp(mode,"attr-unterminated")){int attrs[256];for(unsigned i=0;i<256;++i)attrs[i]=1;win64_region_add((uintptr_t)attrs,sizeof(attrs),0);C(alcCreateContext,0,attrs,0,0);}
        if(!strcmp(mode,"readonly")){uint64_t p=C(alcGetString,0,ALC_EXTENSIONS,0,0);win64_pointer(p,1,1);}
        if(!strcmp(mode,"array-range")){uint32_t id=0;win64_region_add((uintptr_t)&id,4,1);C(alGenSources,INT32_MAX,&id,0,0);}
        if(!strcmp(mode,"null-sources"))C(alGenSources,1,0,0,0);
        if(!strcmp(mode,"null-buffers"))C(alGenBuffers,1,0,0,0);
        if(!strcmp(mode,"null-queue"))C(alSourceQueueBuffers,1,1,0,0);
        if(!strcmp(mode,"shutdown")){win64_openal_shutdown();C(alGetError,0,0,0,0);}
        if(!strcmp(mode,"released")){uint64_t dev=C(alcOpenDevice,0,0,0,0);check(dev!=0,"negative real device");check(C(alcCloseDevice,dev,0,0,0),"negative close");C(alcGetError,dev,0,0,0);}
        if(!strcmp(mode,"closed-context")){uint64_t dev=C(alcOpenDevice,0,0,0,0);check(dev!=0,"live-context real device");uint64_t ctx=C(alcCreateContext,dev,0,0,0);check(ctx&&C(alcCloseDevice,dev,0,0,0),"native CloseDevice destroys live context");C(alcDestroyContext,ctx,0,0,0);}
        check(0,"negative child must stop");
    }
    void*handle=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);check(handle!=NULL,"oracle native dlopen");
    LOAD(LPALGETSTRING,alGetString);LOAD(LPALGETERROR,alGetError);LOAD(LPALCGETERROR,alcGetError);LOAD(LPALCGETSTRING,alcGetString);LOAD(LPALCGETPROCADDRESS,alcGetProcAddress);
    LOAD(LPALGETSOURCEF,alGetSourcef);LOAD(LPALGETBUFFERI,alGetBufferi);LOAD(LPALCGETCURRENTCONTEXT,alcGetCurrentContext);
    typedef void(*enable_fn)(int);typedef int(*metrics_fn)(FrostpunkAudioMetrics*,uint32_t);LOAD(enable_fn,FrostpunkAudio_ProbeEnable);LOAD(metrics_fn,FrostpunkAudio_GetMetrics);
    FrostpunkAudio_ProbeEnable(1);
    /* Preserve real native no-context query/error behavior before any device. */
    const char*raw=alGetString(AL_EXTENSIONS);ALenum raw_error=alGetError();uint64_t filtered=C(alGetString,AL_EXTENSIONS,0,0,0);ALenum guest_error=(ALenum)C(alGetError,0,0,0,0);
    check((!raw)==(!filtered)&&raw_error==guest_error,"no-context extensions NULL/error matches native");
    if(filtered)check(!strstr(win64_string_a(filtered),"callback")&&!strstr(win64_string_a(filtered),"EFX"),"no-context extension filter");
    const char*unknown=guest_string("Frostpunk_NoSuchAudioProcedure_323190");const char*static_data=guest_string("alBufferDataStatic");const char*known=guest_string("alBufferData");
    (void)alcGetError(NULL);check(alcGetProcAddress(NULL,unknown)==NULL,"native unknown GPA");ALCenum native_unknown=alcGetError(NULL);
    check(!C(alcGetProcAddress,0,unknown,0,0)&&(ALCenum)C(alcGetError,0,0,0,0)==native_unknown,"unknown GPA/error matches native and original NULL");
    (void)alcGetError(NULL);check(alcGetProcAddress(NULL,NULL)==NULL,"native NULL GPA");ALCenum native_null=alcGetError(NULL);
    check(!C(alcGetProcAddress,0,0,0,0)&&(ALCenum)C(alcGetError,0,0,0,0)==native_null&&native_null==ALC_INVALID_VALUE,"NULL GPA error original INVALID_VALUE");
    check(!C(alcGetProcAddress,0,static_data,0,0),"original alBufferDataStatic NULL fallback");
    check(C(alcGetProcAddress,0,known,0,0)==routes[index_for("alBufferData")].va,"GPA exact original callable VA");
    uint64_t enumeration=C(alcGetString,0,ALC_DEVICE_SPECIFIER,0,0);check(enumeration!=0,"owned double-NUL device enumeration");size_t listbytes=0;for(;listbytes<65535;++listbytes){const char*s=win64_pointer(enumeration+listbytes,1,0);if(!*s&&listbytes&&*(const char*)win64_pointer(enumeration+listbytes-1,1,0)==0)break;}check(listbytes<65535,"device list second NUL validated");
    uint64_t capture=C(alcGetString,0,ALC_CAPTURE_DEVICE_SPECIFIER,0,0);check(capture&& !memcmp(win64_pointer(capture,2,0),"\0",2),"disabled capture returns owned read-only double-NUL empty list");
    uint64_t dev=C(alcOpenDevice,0,0,0,0);check(dev!=0,"guest ABI opens actual physical backend");
    int attrs[]={ALC_SYNC,ALC_TRUE,0};win64_region_add((uintptr_t)attrs,sizeof(attrs),0);uint64_t ctx=C(alcCreateContext,dev,attrs,0,0);check(ctx&&C(alcMakeContextCurrent,ctx,0,0,0),"guest original sync attrs/current");
    check(!C(alcGetError,dev,0,0,0),"real ALC error clear");
    uint32_t winmm_count;unsigned winmm_calls=winmm_inventory_test(ctx,alcGetString,alcGetError,alcGetCurrentContext,&winmm_count);
    uint64_t version=C(alGetString,AL_VERSION,0,0,0);check(version&&strstr(win64_string_a(version),"ALSOFT 1.25.2"),"owned real native version");
    uint64_t extensions=C(alGetString,AL_EXTENSIONS,0,0,0);const char*exts=win64_string_a(extensions);check(!strstr(exts,"callback")&&!strstr(exts,"EFX")&&!strstr(exts,"thread_local"),"unsupported AL extensions hidden");
    uint64_t alc_extensions=C(alcGetString,dev,ALC_EXTENSIONS,0,0);const char*alc_exts=win64_string_a(alc_extensions);check(!strstr(alc_exts,"capture")&&!strstr(alc_exts,"EFX")&&!strstr(alc_exts,"thread_local"),"unsupported ALC extensions hidden");
    const char*enumerate_ext=guest_string("ALC_ENUMERATION_EXT"),*capture_ext=guest_string("ALC_EXT_CAPTURE");check(C(alcIsExtensionPresent,dev,enumerate_ext,0,0)==ALC_TRUE,"supported enumeration extension");check(!C(alcIsExtensionPresent,dev,capture_ext,0,0),"unbridged capture not advertised");
    const char*raw_invalid=alcGetString((ALCdevice*)(uintptr_t)dev,-12345);ALCenum error_invalid=alcGetError((ALCdevice*)(uintptr_t)dev);
    check(raw_invalid==NULL&&!C(alcGetString,dev,(uint32_t)-12345,0,0)&&(ALCenum)C(alcGetError,dev,0,0,0)==error_invalid,"invalid string native NULL/error");
    float position[]={0.25f,-0.5f,0.75f},orientation[]={0,0,-1,0,1,0},returned[6]={0},x=0,y=0,z=0;
    win64_region_add((uintptr_t)position,sizeof(position),0);win64_region_add((uintptr_t)orientation,sizeof(orientation),0);win64_region_add((uintptr_t)returned,sizeof(returned),1);win64_region_add((uintptr_t)&x,4,1);win64_region_add((uintptr_t)&y,4,1);win64_region_add((uintptr_t)&z,4,1);
    C(alListenerfv,AL_POSITION,position,0,0);C(alListenerfv,AL_ORIENTATION,orientation,0,0);C(alGetListenerfv,AL_ORIENTATION,returned,0,0);check(!memcmp(returned,orientation,sizeof(orientation)),"six-float listener vector");C(alGetListener3f,AL_POSITION,&x,&y,&z);check(x==position[0]&&y==position[1]&&z==position[2],"three distinct output pointers");C(alDistanceModel,AL_INVERSE_DISTANCE_CLAMPED,0,0,0);
    ALuint buffers[4]={0},source=0;win64_region_add((uintptr_t)buffers,sizeof(buffers),1);win64_region_add((uintptr_t)&source,4,1);C(alGenBuffers,4,buffers,0,0);C(alGenSources,1,&source,0,0);check(source&&buffers[0]&&!C(alGetError,0,0,0,0),"native buffer/source IDs");
    int16_t pcm[9600];for(unsigned i=0;i<4800;++i)pcm[i*2]=pcm[i*2+1]=(int16_t)(sin((double)i*6.283185307179586*440/48000)*6000);win64_region_add((uintptr_t)pcm,sizeof(pcm),0);
    for(unsigned i=0;i<4;++i)call("alBufferData",buffers[i],AL_FORMAT_STEREO16,(uintptr_t)pcm,sizeof(pcm),48000,0);
    ALint frequency=0,size=0;alGetBufferi(buffers[0],AL_FREQUENCY,&frequency);alGetBufferi(buffers[0],AL_SIZE,&size);check(frequency==48000&&size==sizeof(pcm),"fifth Win64 stack argument and full PCM bytecount");
    call("alSourcef",source,AL_GAIN,0,0,0,0.08f);ALfloat gain=0;alGetSourcef(source,AL_GAIN,&gain);check(gain==0.08f,"third positional float from XMM2 not XMM0");
    C(alSourcefv,source,AL_POSITION,position,0);C(alSourcei,source,AL_SOURCE_RELATIVE,AL_TRUE,0);C(alSourceQueueBuffers,source,4,buffers,0);C(alSourcePlay,source,0,0,0);check(!C(alGetError,0,0,0,0),"queued PCM through bridge");
    win64_openal_pause(1);win64_openal_pause(1);
    ALint paused_offset=0,still_offset=0;win64_region_add((uintptr_t)&paused_offset,4,1);win64_region_add((uintptr_t)&still_offset,4,1);
    C(alGetSourcei,source,AL_SAMPLE_OFFSET,&paused_offset,0);delay(80);C(alGetSourcei,source,AL_SAMPLE_OFFSET,&still_offset,0);
    check(paused_offset==still_offset,"background freezes source playback");
    win64_openal_pause(0);win64_openal_pause(0);delay(40);C(alGetSourcei,source,AL_SAMPLE_OFFSET,&still_offset,0);
    check(still_offset!=paused_offset&&!C(alGetError,0,0,0,0),"foreground resumes source playback");
    delay(250);ALint state=0,processed=0;win64_region_add((uintptr_t)&state,4,1);win64_region_add((uintptr_t)&processed,4,1);C(alGetSourcei,source,AL_SOURCE_STATE,&state,0);C(alGetSourcei,source,AL_BUFFERS_PROCESSED,&processed,0);check(state==AL_PLAYING&&processed>0,"actual queued source playback/progress");
    ALuint recycled[4];win64_region_add((uintptr_t)recycled,sizeof(recycled),1);C(alSourceUnqueueBuffers,source,processed,recycled,0);C(alSourceQueueBuffers,source,processed,recycled,0);C(alcSuspendContext,ctx,0,0,0);C(alcProcessContext,ctx,0,0,0);delay(100);
    FrostpunkAudioMetrics m={0};check(FrostpunkAudio_GetMetrics(&m,sizeof(m))&&m.audio_api==2&&m.stream_state==4&&m.callbacks&&m.nonzero_samples&&m.peak_abs>0.001&&!m.nonfinite_samples,"real AAudio started/rendered finite nonzero PCM");
    C(alSourceStop,source,0,0,0);C(alGetSourcei,source,AL_BUFFERS_PROCESSED,&processed,0);check(processed==4,"stop completes all buffers");C(alSourceUnqueueBuffers,source,4,recycled,0);C(alDeleteSources,1,&source,0,0);C(alDeleteBuffers,4,buffers,0,0);check(!C(alGetError,0,0,0,0),"real native queue cleanup");
    C(alGenBuffers,(uint32_t)-1,0,0,0);check(C(alGetError,0,0,0,0)==AL_INVALID_VALUE,"negative count native error");C(alGenSources,0,0,0,0);check(!C(alGetError,0,0,0,0),"zero count NULL native success");
    check(C(alcMakeContextCurrent,0,0,0,0),"clear native current");C(alcDestroyContext,ctx,0,0,0);check(C(alcCloseDevice,dev,0,0,0),"close real native device");
    check(strstr(win64_string_a(version),"ALSOFT")!=NULL,"copied string survives native device/context destruction");
    uint64_t live_device=C(alcOpenDevice,0,0,0,0);check(live_device!=0,"live-context close device open");check(C(alcCreateContext,live_device,0,0,0)!=0&&C(alcCloseDevice,live_device,0,0,0),"CloseDevice destroys live context and bridge retires its registry");
    unsigned routes_seen=0;for(unsigned i=0;i<30;++i)routes_seen+=seen[i]!=0;check(routes_seen==30,"all30typed routes tested");
    unsigned before=nregions;win64_openal_shutdown();check(nregions<before,"shutdown removes owned read-only regions");CPU c={0};check(!win64_openal_dispatch(&c,0),"unrelated target still passes after shutdown");
    printf("{\"success\":true,\"fifo_named_case\":%u,\"fifo_pipe_boundary_case\":%u,\"typed_routes\":%u,\"calls\":%u,\"checks\":%u,\"winmm_endpoint_count\":%u,\"winmm_parallel_queries\":%u,\"winmm_real_inventory\":true,\"winmm_no_caps_driver\":true,\"winmm_context_survives\":true,\"CPU_RSP_canaries\":true,\"audio_api\":%d,\"stream_state\":%d,\"callbacks\":%llu,\"frames\":%llu,\"nonzero_samples\":%llu,\"nonfinite_samples\":%llu,\"peak_abs\":%.9g,\"xruns\":%d,\"original_x64_executed\":false,\"game_boot_proven\":false}\n",named_fifo_case,pipe_fifo_case,routes_seen,calls,checks,winmm_count,winmm_calls,m.audio_api,m.stream_state,(unsigned long long)m.callbacks,(unsigned long long)m.frames,(unsigned long long)m.nonzero_samples,(unsigned long long)m.nonfinite_samples,m.peak_abs,m.xruns);
    return 0;
}
