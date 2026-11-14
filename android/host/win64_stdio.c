/* Win64 narrow CRT formatting: guest va_list is an array of eight-byte slots. */
#define _GNU_SOURCE
#include "win64_stdio.h"
#include "win64.h"
#if !defined(_MSC_VER)
#include "win64_kernel.h"
#include <unistd.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <windows.h>
#else
#include <pthread.h>
#include <stdatomic.h>
#endif

#define OUTPUT_LIMIT (16u*1024u*1024u)
typedef struct { uint64_t magic; uint32_t index,reserved; uint64_t base,ptr; int32_t count; uint32_t padding; } guest_file;
static guest_file standard_files[3]={{UINT64_C(0x4650535444494f31),0,0,0,0,0,0},{UINT64_C(0x4650535444494f31),1,0,0,0,0,0},{UINT64_C(0x4650535444494f31),2,0,0,0,0,0}};
static _Noreturn void stdio_stop(const char *reason) {
    /* Fatal diagnostics must not acquire a second FILE lock while a stream or
     * registry lock is owned; another guest thread may own stderr's lock. */
    char message[512];int n=snprintf(message,sizeof(message),"STOP Win64 stdio %s\n",reason);
    size_t bytes=n>0?((size_t)n<sizeof(message)?(size_t)n:sizeof(message)-1):0;
#if defined(_MSC_VER)
    DWORD done;WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,(DWORD)bytes,&done,NULL);
#else
    size_t at=0;while(at<bytes){ssize_t done=write(STDERR_FILENO,message+at,bytes-at);if(done>0)at+=(size_t)done;else if(done<0&&errno==EINTR)continue;else break;}
#endif
    _Exit(73);
}
/* UCRT's default stream limit is 512. Slots may be reused after fclose; using
 * an already closed FILE pointer has no supported lifetime contract. */
typedef struct {
    guest_file *guest;FILE *native;void *token;unsigned refs,lock_depth;
    int state,registered,readable;
#if defined(_MSC_VER)
    DWORD owner;
#else
    pthread_t owner;
#endif
} file_state;
static guest_file dynamic_files[509];
static file_state file_states[512];
#if defined(_MSC_VER)
static SRWLOCK registry_mutex=SRWLOCK_INIT;
static CONDITION_VARIABLE registry_changed=CONDITION_VARIABLE_INIT;
static void registry_lock(void){AcquireSRWLockExclusive(&registry_mutex);}
static void registry_unlock(void){ReleaseSRWLockExclusive(&registry_mutex);}
static void registry_wait(void){SleepConditionVariableSRW(&registry_changed,&registry_mutex,INFINITE,0);}
static void registry_signal(void){WakeAllConditionVariable(&registry_changed);}
static int owns_stream(file_state *s){return s->lock_depth&&s->owner==GetCurrentThreadId();}
#else
static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t registry_changed=PTHREAD_COND_INITIALIZER;
static void registry_lock(void){pthread_mutex_lock(&registry_mutex);}
static void registry_unlock(void){pthread_mutex_unlock(&registry_mutex);}
static void registry_wait(void){pthread_cond_wait(&registry_changed,&registry_mutex);}
static void registry_signal(void){pthread_cond_broadcast(&registry_changed);}
static int owns_stream(file_state *s){return s->lock_depth&&pthread_equal(s->owner,pthread_self());}
#endif
static _Noreturn void registry_stop(const char *reason){registry_unlock();stdio_stop(reason);}
static void register_files(void) {
    win64_region_add((uintptr_t)standard_files,sizeof(standard_files),0);
    for (unsigned i=0;i<3;++i) win64_region_add((uintptr_t)&standard_files[i].base,24,1);
    for(unsigned i=0;i<512;++i){file_state *s=&file_states[i];s->guest=i<3?&standard_files[i]:&dynamic_files[i-3];
        if(i<3){s->state=s->registered=1;s->readable=i==0;s->native=i==0?stdin:i==1?stdout:stderr;}}
}
#if defined(_MSC_VER)
static INIT_ONCE files_once=INIT_ONCE_STATIC_INIT;
static volatile LONG count_output;
static BOOL CALLBACK initialize_files(PINIT_ONCE once,PVOID data,PVOID *context) {
    (void)once;(void)data;(void)context;register_files();
    _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS|=_CRT_INTERNAL_PRINTF_STANDARD_ROUNDING;
    return TRUE;
}
static void files_init(void) { InitOnceExecuteOnce(&files_once,initialize_files,NULL,NULL); }
static int count_exchange(int value) { return (int)InterlockedExchange(&count_output,value); }
static int count_enabled(void) { return (int)InterlockedCompareExchange(&count_output,0,0); }
#else
static pthread_once_t files_once=PTHREAD_ONCE_INIT;
static atomic_int count_output;
static void files_init(void) { pthread_once(&files_once,register_files); }
static int count_exchange(int value) { return atomic_exchange(&count_output,value); }
static int count_enabled(void) { return atomic_load(&count_output); }
#endif
static FILE *host_file(uint64_t guest) {
    files_init();
    for (unsigned i=0;i<3;++i) if (guest==(uintptr_t)&standard_files[i]) return i==0?stdin:i==1?stdout:stderr;
    stdio_stop("unrecognized opaque FILE handle");
}
static file_state *stream_find(uint64_t guest){
    for(unsigned i=0;i<512;++i)if(guest==(uintptr_t)file_states[i].guest)return &file_states[i];
    return NULL;
}
static file_state *stream_acquire(uint64_t guest){
    registry_lock();file_state *s=stream_find(guest);
    if(!s || !s->native || (s->state!=1&&!(s->state==3&&owns_stream(s))))registry_stop("invalid or closed FILE handle");
    ++s->refs;registry_unlock();return s;
}
static void stream_release(file_state *s){registry_lock();if(!s->refs)registry_stop("FILE reference underflow");--s->refs;registry_signal();registry_unlock();}
static void stream_lock(FILE *stream) {
#if defined(_MSC_VER)
    _lock_file(stream);
#else
    flockfile(stream);
#endif
}
static void stream_unlock(FILE *stream) {
#if defined(_MSC_VER)
    _unlock_file(stream);
#else
    funlockfile(stream);
#endif
}
static void unbuffered_check(guest_file *stream) {
    if (stream->base || stream->ptr || stream->count>0) stdio_stop("guest buffering is not implemented");
    stream->count=0;
}
static int stream_open(CPU *c){
    uint64_t output=win64_argument(c,0),path=win64_argument(c,1),mode=win64_argument(c,2),zero=0;
    if(!output){win64_stdio_set_errno(22);stdio_stop("NULL FILE output invalid-parameter callback unsupported");}
    memcpy(win64_pointer(output,8,1),&zero,8);
    if(!path||!mode){win64_stdio_set_errno(22);stdio_stop("NULL fopen argument invalid-parameter callback unsupported");}
    uint16_t first,kind,second,end;memcpy(&first,win64_pointer(path,2,0),2);
    if(!first){win64_stdio_set_errno(22);c->rax=22;return 1;}
    memcpy(&kind,win64_pointer(mode,2,0),2);
    if(!kind){win64_stdio_set_errno(22);stdio_stop("empty fopen mode invalid-parameter callback unsupported");}
    memcpy(&second,win64_pointer(mode+2,2,0),2);
    if(second!='b'||(kind!='r'&&kind!='w')){win64_stdio_set_errno(22);stdio_stop("only proven binary rb/wb open modes are implemented");}
    memcpy(&end,win64_pointer(mode+4,2,0),2);if(end){win64_stdio_set_errno(22);stdio_stop("extended fopen modes are unsupported");}
    registry_lock();file_state *s=NULL;
    for(unsigned i=3;i<512;++i)if(!file_states[i].state){s=&file_states[i];s->state=2;break;}
    registry_unlock();if(!s){win64_stdio_set_errno(24);c->rax=24;return 1;}
    int result;FILE *native=NULL;void *token=NULL;
#if defined(_MSC_VER)
    /* Native Windows builds are formatting differential fixtures only; the
     * production anchored provider is POSIX and never opens a raw guest path. */
    stdio_stop("anchored FILE opening requires Android kernel provider");result=22;
#else
    int fd=-1;result=win64_kernel_stream_open(path,1,kind=='w',&fd,&token);
    if(!result){native=win64_kernel_stream_file(fd,token,kind=='w'?"wb":"rb");if(!native){result=errno;close(fd);win64_kernel_stream_close(token);token=NULL;}
        else if(setvbuf(native,NULL,_IONBF,0)){fclose(native);win64_kernel_stream_close(token);token=NULL;native=NULL;result=12;}}
#endif
    registry_lock();
    if(result){s->state=0;registry_unlock();win64_stdio_set_errno(result);c->rax=(uint32_t)result;return 1;}
    if(!s->registered){win64_region_add((uintptr_t)s->guest,sizeof(*s->guest),0);win64_region_add((uintptr_t)&s->guest->base,24,1);s->registered=1;}
    *s->guest=(guest_file){UINT64_C(0x4650535444494f31),(uint32_t)(s-file_states),0,0,0,0,0};
    s->native=native;s->token=token;s->readable=kind=='r';s->state=1;uint64_t guest=(uintptr_t)s->guest;
    registry_unlock();memcpy(win64_pointer(output,8,1),&guest,8);c->rax=0;return 1;
}
static int stream_call(CPU *c,const char *name) {
    if(!strcmp(name,"_wfopen_s"))return stream_open(c);
    if(!strcmp(name,"fclose")){
        uint64_t guest=win64_argument(c,0);if(!guest){win64_stdio_set_errno(22);stdio_stop("NULL fclose invalid-parameter callback unsupported");}
        registry_lock();file_state *s=stream_find(guest);
        if(!s||s->state!=1)registry_stop("fclose invalid or closed FILE");
        if(s<file_states+3)registry_stop("closing standard FILE is unsupported");
        if(owns_stream(s))registry_stop("fclose while owning explicit FILE lock is unsupported");
        s->state=3;while(s->refs)registry_wait();FILE *native=s->native;void *token=s->token;registry_unlock();
        int result=fclose(native),error=errno;
#if !defined(_MSC_VER)
        win64_kernel_stream_close(token);
#else
        (void)token;
#endif
        registry_lock();s->native=NULL;s->token=NULL;s->state=0;registry_signal();registry_unlock();
        if(result)win64_stdio_set_errno(error);c->rax=(uint32_t)result;return 1;
    }
    if (!strcmp(name,"_get_stream_buffer_pointers")) {
        uint64_t handle=win64_argument(c,0);
        if (!handle) { win64_stdio_set_errno(EINVAL);c->rax=EINVAL;return 1; }
        file_state *s=stream_acquire(handle);guest_file *file=s->guest;
        uint64_t fields[]={(uintptr_t)&file->base,(uintptr_t)&file->ptr,(uintptr_t)&file->count};
        for (unsigned i=0;i<3;++i) { uint64_t out=win64_argument(c,i+1);if(out)memcpy(win64_pointer(out,8,1),&fields[i],8); }
        stream_release(s);c->rax=0;return 1;
    }
    if (!strcmp(name,"_lock_file") || !strcmp(name,"_unlock_file")) {
        uint64_t guest=win64_argument(c,0);
        if(!strcmp(name,"_lock_file")){file_state *s=stream_acquire(guest);stream_lock(s->native);registry_lock();
#if defined(_MSC_VER)
            s->owner=GetCurrentThreadId();
#else
            s->owner=pthread_self();
#endif
            ++s->lock_depth;registry_unlock(); /* reference belongs to this explicit lock */
        }else{registry_lock();file_state *s=stream_find(guest);if(!s||!owns_stream(s))registry_stop("unlock of unowned FILE lock");--s->lock_depth;registry_unlock();stream_unlock(s->native);stream_release(s);}
        return 1;
    }
    if (!strcmp(name,"fflush")) {
        uint64_t handle=win64_argument(c,0);int result;
        if (handle){file_state *s=stream_acquire(handle);result=fflush(s->native);stream_release(s);}
        else {result=0;for(unsigned i=1;i<512;++i){registry_lock();file_state *s=&file_states[i];int active=s->state==1&&!s->readable;if(active)++s->refs;registry_unlock();if(active){if(fflush(s->native))result=EOF;stream_release(s);}}}
        if (result) win64_stdio_set_errno(errno);c->rax=(uint32_t)result;return 1;
    }
    if (!strcmp(name,"fgetc") || !strcmp(name,"getc") || !strcmp(name,"ferror") || !strcmp(name,"feof") || !strcmp(name,"clearerr")) {
        uint64_t handle=win64_argument(c,0);file_state *s=stream_acquire(handle);FILE *stream=s->native;int result;
        stream_lock(stream);unbuffered_check(s->guest);
        if (!strcmp(name,"clearerr")) { clearerr(stream);result=0; }
        else if (!strcmp(name,"ferror")) result=!!ferror(stream);
        else if (!strcmp(name,"feof")) result=!!feof(stream);
        else { result=fgetc(stream);if(result==EOF && ferror(stream)&&s->readable)win64_stdio_set_errno(errno); }
        stream_unlock(stream);stream_release(s);c->rax=(uint32_t)result;return 1;
    }
    if (!strcmp(name,"fwrite") || !strcmp(name,"fread")) {
        uint64_t data=win64_argument(c,0),size=win64_argument(c,1),count=win64_argument(c,2),handle=win64_argument(c,3);
        int reading=!strcmp(name,"fread");
        if (size && count>SIZE_MAX/size) { win64_stdio_set_errno(22);stdio_stop("fread/fwrite size overflow invalid-parameter callback unsupported"); }
        size_t bytes=(size_t)(size*count);if (!bytes) { c->rax=0;return 1; }
        if(!data||!handle){win64_stdio_set_errno(22);stdio_stop("NULL fread/fwrite invalid-parameter callback unsupported");}
        file_state *s=stream_acquire(handle);FILE *stream=s->native;
        void *buffer=win64_pointer(data,bytes,reading);
        stream_lock(stream);unbuffered_check(s->guest);
        size_t result=reading?fread(buffer,(size_t)size,(size_t)count,stream):fwrite(buffer,(size_t)size,(size_t)count,stream);
        if (result<count && ferror(stream)&&(!reading||s->readable)) win64_stdio_set_errno(errno);
        stream_unlock(stream);stream_release(s);c->rax=result;return 1;
    }
    if (!strcmp(name,"ungetc") || !strcmp(name,"fputc") || !strcmp(name,"putc") || !strcmp(name,"fputs")) {
        uint64_t value=win64_argument(c,0),handle=win64_argument(c,1);file_state *s=stream_acquire(handle);FILE *stream=s->native;int result;
        const char *text=!strcmp(name,"fputs")?win64_string_a(value):NULL;
        stream_lock(stream);unbuffered_check(s->guest);
        if (text) result=fputs(text,stream);
        else if (!strcmp(name,"ungetc")) result=ungetc((int)value,stream);
        else result=fputc((int)value,stream);
        if (result==EOF && ferror(stream)) win64_stdio_set_errno(errno);
        stream_unlock(stream);stream_release(s);c->rax=(uint32_t)result;return 1;
    }
    if(!strcmp(name,"_ftelli64")||!strcmp(name,"ftell")||!strcmp(name,"_fseeki64")||!strcmp(name,"fseek")){
        uint64_t handle=win64_argument(c,0);if(!handle){win64_stdio_set_errno(22);stdio_stop("NULL seek/tell invalid-parameter callback unsupported");}
        file_state *s=stream_acquire(handle);FILE *stream=s->native;int tell=!strcmp(name,"ftell")||!strcmp(name,"_ftelli64");
        stream_lock(stream);unbuffered_check(s->guest);int64_t result;
        if(tell){
#if defined(_MSC_VER)
            result=_ftelli64(stream);
#else
            result=ftello(stream);
#endif
            if(result>=0&&!strcmp(name,"ftell")&&result>INT32_MAX){result=-1;errno=22;}
        }else{int origin=(int32_t)win64_argument(c,2);if(origin<0||origin>2){win64_stdio_set_errno(22);stdio_stop("seek origin invalid-parameter callback unsupported");}
            int64_t offset=!strcmp(name,"fseek")?(int32_t)win64_argument(c,1):(int64_t)win64_argument(c,1);
#if defined(_MSC_VER)
            result=_fseeki64(stream,offset,origin);
#else
            result=fseeko(stream,(off_t)offset,origin);
#endif
        }
        if(result<0)win64_stdio_set_errno(errno);stream_unlock(stream);stream_release(s);
        c->rax=!strcmp(name,"_ftelli64")?(uint64_t)result:(uint32_t)result;return 1;
    }
    return 0;
}
typedef struct { char *bytes; size_t length,capacity,limit; int truncated; } output;
static void append(output *o,const char *p,size_t bytes) {
    if (bytes>OUTPUT_LIMIT || o->length>OUTPUT_LIMIT-bytes) stdio_stop("format output exceeds 16 MiB diagnostic budget");
    if (bytes>o->limit-o->length) { bytes=o->limit-o->length; o->truncated=1; }
    if (o->length+bytes+1>o->capacity) {
        size_t n=o->length+bytes+1; char *q=(char*)realloc(o->bytes,n);
        if (!q) stdio_stop("format allocation failed"); o->bytes=q;o->capacity=n;
    }
    if (bytes) memcpy(o->bytes+o->length,p,bytes);
    o->length+=bytes;o->bytes[o->length]=0;
}
static void padding(output *o,char value,size_t bytes) {
    char block[128];memset(block,value,sizeof(block));
    while (bytes && !o->truncated) { size_t n=bytes<sizeof(block)?bytes:sizeof(block);append(o,block,n);bytes-=n; }
}
static uint64_t next_arg(uint64_t *va) {
    uint64_t result;memcpy(&result,win64_pointer(*va,8,0),8);
    if (*va>UINT64_MAX-8) stdio_stop("va_list pointer overflow");*va+=8;return result;
}
typedef enum { L_NONE,L_HH,L_H,L_L,L_LL,L_I32,L_I64 } length_type;
typedef struct { int left,plus,space,alternate,zero,width,precision; length_type length; char type; } spec;
static int decimal(const char **p) {
    int value=0;
    while (isdigit((unsigned char)**p)) {
        int d=*(*p)++-'0';if (value>(int)(OUTPUT_LIMIT-d)/10) stdio_stop("format width/precision exceeds diagnostic budget");value=value*10+d;
    }return value;
}
static spec parse_spec(const char **p,uint64_t *va) {
    spec s={0};s.precision=-1;
    for (;;) {
        char a=**p;
        if (a=='-')s.left=1;else if(a=='+')s.plus=1;else if(a==' ')s.space=1;
        else if(a=='#')s.alternate=1;else if(a=='0')s.zero=1;else break;++*p;
    }
    if (**p=='*') { ++*p;s.width=(int32_t)next_arg(va);if(s.width==INT_MIN)stdio_stop("INT_MIN field width");if(s.width<0){s.left=1;s.width=-s.width;} }
    else s.width=decimal(p);
    if (**p=='.') { ++*p;if(**p=='*'){++*p;s.precision=(int32_t)next_arg(va);if(s.precision<0)s.precision=-1;}else s.precision=decimal(p); }
    if ((unsigned)s.width>OUTPUT_LIMIT || s.precision>(int)OUTPUT_LIMIT)stdio_stop("format width/precision exceeds diagnostic budget");
    if (**p=='h') {++*p;s.length=L_H;if(**p=='h'){++*p;s.length=L_HH;}}
    else if(**p=='l'){++*p;s.length=L_L;if(**p=='l'){++*p;s.length=L_LL;}}
    else if(**p=='I'){++*p;if(!strncmp(*p,"64",2)){*p+=2;s.length=L_I64;}else if(!strncmp(*p,"32",2)){*p+=2;s.length=L_I32;}else s.length=L_I64;}
    else if(**p=='j'||**p=='z'||**p=='t'){++*p;s.length=L_I64;}
    else if(**p=='L'){++*p;s.length=L_L;}
    else if(**p=='w')stdio_stop("UTF-16 printf conversion is unsupported");
    s.type=*(*p)++;if(!s.type)stdio_stop("incomplete printf format");
    if (s.type=='S'||s.type=='C'||((s.type=='s'||s.type=='c')&&s.length==L_L))stdio_stop("UTF-16 printf conversion is unsupported");
    return s;
}
static void native_spec(char *b,spec s,int integer,int width) {
    char *p=b;*p++='%';
    if(width&&s.left)*p++='-';if(s.plus)*p++='+';if(s.space)*p++=' ';if(s.alternate)*p++='#';if(width&&s.zero)*p++='0';
    if(width&&s.width)p+=sprintf(p,"%d",s.width);
    if(s.precision>=0)p+=sprintf(p,".%d",s.precision);
    if(integer){*p++='l';*p++='l';}*p++=s.type;*p=0;
}
static void padded(output *o,const char *core,size_t n,spec s,int number) {
    size_t pad=(size_t)s.width>n?(size_t)s.width-n:0;
    if(!s.left&&s.zero&&number) {
        size_t sign=n&&(*core=='+'||*core=='-'||*core==' ')?1:0;
        size_t prefix=n>=sign+2&&core[sign]=='0'&&(core[sign+1]=='x'||core[sign+1]=='X')?2:0;
        append(o,core,sign+prefix);padding(o,'0',pad);append(o,core+sign+prefix,n-sign-prefix);
    }else{if(!s.left)padding(o,' ',pad);append(o,core,n);if(s.left)padding(o,' ',pad);}
}
static unsigned trailing_zeros(uint64_t value) { unsigned n=0;while(!(value&1)){++n;value>>=1;}return n; }
/* A decimal half tie requires cancellation of every factor of five in the
 * denominator, then exactly one remaining factor of two in that denominator. */
static int decimal_round_up(double value,int shift) {
    uint64_t bits;memcpy(&bits,&value,8);unsigned exp=(unsigned)(bits>>52)&2047;
    uint64_t mantissa=bits&UINT64_C(0xfffffffffffff);int power=exp?(int)exp-1023-52:-1074;
    if(exp)mantissa|=UINT64_C(0x10000000000000);if(!mantissa||exp==2047)return 0;
    if(shift<0){if(shift<-23)return 0;for(int i=0;i<-shift;++i){if(mantissa%5)return 0;mantissa/=5;}}
    unsigned zeros=trailing_zeros(mantissa);
    return power+shift+(int)zeros==-1 && ((mantissa>>zeros)&3)==1;
}
static double legacy_hex_rounding(double value,spec s) {
    if(!isfinite(value)||value==0)return value;
    int precision=s.precision>=0?s.precision:13;
    if(s.type=='a'||s.type=='A') {
        if(precision>=13)return value;
        uint64_t bits;memcpy(&bits,&value,8);unsigned exponent=(unsigned)(bits>>52)&2047;
        uint64_t m=(bits&UINT64_C(0xfffffffffffff))|(exponent?UINT64_C(0x10000000000000):0);
        unsigned removed=52-4*(unsigned)precision;uint64_t mask=(UINT64_C(1)<<removed)-1;
        return (m&mask)==(UINT64_C(1)<<(removed-1))?nextafter(value,copysign(INFINITY,value)):value;
    }
    return value;
}
static char *float_text(double value,spec s,uint64_t options,uint32_t mxcsr,size_t *length) {
    int previous=fegetround();static const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    if(fesetround(options&32?modes[(mxcsr>>13)&3]:FE_TONEAREST))stdio_stop("native rounding mode unavailable");
    if((s.type=='a'||s.type=='A')&&s.precision<0)s.precision=13;
    int advance=0,trim=0;
    if(!(options&32)) {
        value=legacy_hex_rounding(value,s);
        if(isfinite(value)&&value!=0&&s.type!='a'&&s.type!='A') {
            int p=s.precision<0?6:s.precision,shift=p;
            if(s.type!='f'&&s.type!='F') {
                char ebuf[64];snprintf(ebuf,sizeof(ebuf),"%.17e",fabs(value));char *e=strchr(ebuf,'e');if(!e)stdio_stop("decimal exponent unavailable");
                shift=(s.type=='e'||s.type=='E')?p-atoi(e+1):(p?p:1)-1-atoi(e+1);
            }
            advance=decimal_round_up(value,shift);
            if(advance&&(s.type=='g'||s.type=='G')&&!s.alternate){trim=1;s.alternate=1;}
        }
    }
    char format[64];native_spec(format,s,0,0);
    int n=snprintf(NULL,0,format,value);if(n<0||n>(int)OUTPUT_LIMIT)stdio_stop("native float format failed");
    char *result=(char*)malloc((size_t)n+3);if(!result)stdio_stop("float allocation failed");
    snprintf(result,(size_t)n+1,format,value);fesetround(previous);
    if(advance) {
        char *e=strchr(result,'e');if(!e)e=strchr(result,'E');char *last=e?e-1:result+n-1;
        while(last>=result&&!isdigit((unsigned char)*last))--last;
        if(last<result||*last>'8'||((*last-'0')&1))stdio_stop("native decimal half-tie contract mismatch");++*last;
        if(trim){char *end=e?e:result+n,*dot=strchr(result,'.');if(dot&&dot<end){char *q=end;while(q>dot+1&&q[-1]=='0')--q;if(q==dot+1)--q;size_t removed=(size_t)(end-q);memmove(q,end,strlen(end)+1);n-=(int)removed;}}
    }
    /* UCRT names the negative canonical NaN generated by an invalid operation. */
    uint64_t bits;memcpy(&bits,&value,8);
    if(!isfinite(value)) {
        int upper=s.type=='A'||s.type=='E'||s.type=='F'||s.type=='G',negative=(int)(bits>>63);
        const char *word=isinf(value)?"inf":!(bits&UINT64_C(0x8000000000000))?"nan(snan)":bits==UINT64_C(0xfff8000000000000)?"nan(ind)":"nan";
        char special[32],*q=special;
        if(negative)*q++='-';
        if(s.alternate&&s.precision==0&&s.type!='g'&&s.type!='G'){
            if(negative){if(s.type=='a'||s.type=='A'){*q++='0';*q++='x';}*q++='.';while(*word)*q++=*word++;}
            else{*q++=*word++;*q++='.';while(*word)*q++=*word++;}
        }else while(*word)*q++=*word++;
        *q=0;if(upper)for(char *t=special;*t;++t)*t=(char)toupper((unsigned char)*t);
        char signed_special[40];snprintf(signed_special,sizeof(signed_special),"%s%s",negative?"":s.plus?"+":s.space?" ":"",special);
        size_t bytes=strlen(signed_special);char *replacement=(char*)realloc(result,bytes+1);if(!replacement)stdio_stop("NaN allocation failed");result=replacement;memcpy(result,signed_special,bytes+1);n=(int)bytes;
    }
    if(options&16) {
        char *e=strchr(result,'e');if(!e)e=strchr(result,'E');
        if(e&&(e[1]=='+'||e[1]=='-')&&strlen(e+2)==2){memmove(e+3,e+2,3);e[2]='0';++n;}
    }
    *length=(size_t)n;return result;
}
static const char *guest_string(uint64_t address,int precision) {
    if(!address)return "(null)";
    if(precision<0)return win64_string_a(address);
    for(int i=0;i<precision;++i) { if(address>UINT64_MAX-(unsigned)i)stdio_stop("string pointer overflow");if(!*(const char*)win64_pointer(address+(unsigned)i,1,0))break; }
    return precision?(const char*)win64_pointer(address,0,0):"";
}
static void count_write(uint64_t address,size_t n,length_type length) {
    size_t bytes=length==L_HH?1:length==L_H?2:(length==L_LL||length==L_I64)?8:4;
    uint64_t count=(uint64_t)n;memcpy(win64_pointer(address,bytes,1),&count,bytes);
}
static output render(const char *format,uint64_t va,uint64_t options,uint32_t mxcsr,size_t limit,int secure) {
    output o={0};o.limit=limit;append(&o,"",0);
    if(options&~UINT64_C(63))stdio_stop("unknown __stdio_common option bits");
    if(options&8)stdio_stop("legacy MSVCRT floating special-value formatting is unsupported");
    while(*format&&!o.truncated) {
        if(*format!='%'){const char *start=format;while(*format&&*format!='%')++format;append(&o,start,(size_t)(format-start));continue;}
        ++format;if(*format=='%'){append(&o,"%",1);++format;continue;}
        spec s=parse_spec(&format,&va);uint64_t bits=next_arg(&va);char native[64],*text=NULL;int n=0;
        if(strchr("diuoxX",s.type)) {
            int wide=s.length==L_LL||s.length==L_I64;
            if(s.length==L_HH)bits=(s.type=='d'||s.type=='i')?(uint64_t)(int64_t)(int8_t)bits:(uint8_t)bits;
            else if(s.length==L_H)bits=(s.type=='d'||s.type=='i')?(uint64_t)(int64_t)(int16_t)bits:(uint16_t)bits;
            else if(!wide)bits=(s.type=='d'||s.type=='i')?(uint64_t)(int64_t)(int32_t)bits:(uint32_t)bits;
            native_spec(native,s,1,1);
            if(s.type=='d'||s.type=='i')n=snprintf(NULL,0,native,(long long)(int64_t)bits);else n=snprintf(NULL,0,native,(unsigned long long)bits);
            if(n<0||n>(int)OUTPUT_LIMIT)stdio_stop("native integer format failed");text=(char*)malloc((size_t)n+1);if(!text)stdio_stop("integer allocation failed");
            if(s.type=='d'||s.type=='i')snprintf(text,(size_t)n+1,native,(long long)(int64_t)bits);else snprintf(text,(size_t)n+1,native,(unsigned long long)bits);
        }else if(s.type=='s'||s.type=='c') {
            if(s.length!=L_NONE&&s.length!=L_H)stdio_stop("unsupported narrow character length modifier");
            native_spec(native,s,0,1);
            if(s.type=='s') { if(secure&&!bits)stdio_stop("secure printf null string invalid-parameter callback unsupported");const char *str=guest_string(bits,s.precision);n=snprintf(NULL,0,native,str); }
            else n=snprintf(NULL,0,native,(int)(uint8_t)bits);
            if(n<0||n>(int)OUTPUT_LIMIT)stdio_stop("native string format failed");text=(char*)malloc((size_t)n+1);if(!text)stdio_stop("string allocation failed");
            if(s.type=='s')snprintf(text,(size_t)n+1,native,guest_string(bits,s.precision));else snprintf(text,(size_t)n+1,native,(int)(uint8_t)bits);
        }else if(s.type=='p') {
            char pointer[32];snprintf(pointer,sizeof(pointer),"%016llX",(unsigned long long)bits);s.zero=0;padded(&o,pointer,16,s,0);continue;
        }else if(strchr("aAeEfFgG",s.type)) {
            if(s.length!=L_NONE&&s.length!=L_L)stdio_stop("unsupported floating length modifier");
            double value;memcpy(&value,&bits,8);size_t bytes;text=float_text(value,s,options,mxcsr,&bytes);padded(&o,text,bytes,s,isfinite(value));free(text);continue;
        }else if(s.type=='n') {
            if(secure||!count_enabled())stdio_stop("printf %n disabled or secure invalid-parameter callback unsupported");
            count_write(bits,o.length,s.length);continue;
        }else stdio_stop("unsupported printf conversion");
        append(&o,text,(size_t)n);free(text);
    }
    return o;
}
static int format_buffer(CPU *c,uint64_t options,uint64_t buffer,uint64_t capacity,uint64_t max_count,uint64_t format,uint64_t locale,uint64_t va,int secure,int counted) {
    if(locale)stdio_stop("non-default printf locale unsupported");
    const char *f=win64_string_a(format);
    if(!secure&&!(options&2)&&!capacity){return -1;}
    if(secure&&(!buffer||!capacity)){win64_stdio_set_errno(EINVAL);stdio_stop("secure destination invalid-parameter callback unsupported");}
    if(!buffer&&capacity)stdio_stop("printf null destination with nonzero size");
    if(secure){if(buffer>UINT64_MAX-capacity)stdio_stop("secure destination range overflow");win64_pointer(buffer,(size_t)capacity,1);}
    size_t limit=OUTPUT_LIMIT;
    if(!secure&&(!(options&2)||(options&1))&&capacity<limit)limit=(size_t)capacity;
    output out=render(f,va,options,c->mxcsr,limit,secure);size_t n=out.length;
    int result=(int)n;
    if(secure) {
        uint64_t allowed=capacity-1;if(counted&&max_count<allowed)allowed=max_count;
        size_t written=n<(size_t)allowed?n:(size_t)allowed;
        char *dst=(char*)win64_pointer(buffer,written+1,1);if(written)memcpy(dst,out.bytes,written);dst[written]=0;
        if(counted&&max_count>=capacity)*(char*)win64_pointer(buffer+capacity-1,1,1)=0;
        if(n>allowed||out.truncated){result=-1;if(!counted||(max_count>=capacity&&max_count!=UINT64_MAX)){dst[0]=0;win64_stdio_set_errno(ERANGE);stdio_stop("secure range error invalid-parameter callback unsupported");}}
    }else if(options&1) {
        size_t written=n<(size_t)capacity?n:(size_t)capacity;
        char *dst=written?(char*)win64_pointer(buffer,written,1):NULL;if(written)memcpy(dst,out.bytes,written);
        if(n<capacity){*(char*)win64_pointer(buffer+n,1,1)=0;}if(out.truncated)result=-1;
    }else {
        if(capacity){size_t written=n<capacity-1?n:(size_t)(capacity-1);char *dst=(char*)win64_pointer(buffer,written+1,1);if(written)memcpy(dst,out.bytes,written);dst[written]=0;}
        if(!(options&2)&&(out.truncated||n>=capacity))result=-2;
    }
    free(out.bytes);return result;
}
int win64_stdio_call(CPU *c,const char *name) {
    files_init();
    /* These exact CRT names are declined after the required first-call FILE
     * registration, without walking unrelated stream/format APIs. */
    if(!strcmp(name,"memcpy")||!strcmp(name,"memmove")||!strcmp(name,"strcat_s")||
       !strcmp(name,"strcpy_s")||!strcmp(name,"strncmp")||!strcmp(name,"powf"))return 0;
    if(stream_call(c,name))return 1;
    if(!strcmp(name,"__acrt_iob_func")) {
        files_init();unsigned index=(unsigned)win64_argument(c,0);if(index>=3)stdio_stop("__acrt_iob_func index outside stdin/stdout/stderr");c->rax=(uintptr_t)&standard_files[index];return 1;
    }
    if(!strcmp(name,"_set_printf_count_output")){c->rax=(uint32_t)count_exchange(!!(int)win64_argument(c,0));return 1;}
    if(!strcmp(name,"_get_printf_count_output")){c->rax=(uint32_t)count_enabled();return 1;}
    int secure=0,counted=0,common=0;
    if(!strcmp(name,"__stdio_common_vsprintf"))common=1;
    else if(!strcmp(name,"__stdio_common_vsprintf_s")){common=1;secure=1;}
    else if(!strcmp(name,"__stdio_common_vsnprintf_s")){common=1;secure=1;counted=1;}
    else if(!strcmp(name,"__stdio_common_vfprintf")||!strcmp(name,"__stdio_common_vfprintf_s")) {
        uint64_t options=win64_argument(c,0),handle=win64_argument(c,1);file_state *s=stream_acquire(handle);FILE *stream=s->native;
        if(stream==stdin)stdio_stop("vfprintf to stdin is unsupported");
        if(win64_argument(c,3))stdio_stop("non-default printf locale unsupported");
        output o=render(win64_string_a(win64_argument(c,2)),win64_argument(c,4),options,c->mxcsr,OUTPUT_LIMIT,name[strlen(name)-1]=='s');
        stream_lock(stream);unbuffered_check(s->guest);
        int result=(int)o.length;if(fwrite(o.bytes,1,o.length,stream)!=o.length){win64_stdio_set_errno(errno);result=-1;}
        stream_unlock(stream);stream_release(s);free(o.bytes);c->rax=(uint32_t)result;return 1;
    }else if(strcmp(name,"vsprintf")&&strcmp(name,"vsprintf_s")&&strcmp(name,"vsnprintf")&&strcmp(name,"_vsnprintf")&&strcmp(name,"_vsnprintf_s"))return 0;
    uint64_t options=0,buffer,capacity,max_count=UINT64_MAX,format,locale=0,va;
    if(common){options=win64_argument(c,0);buffer=win64_argument(c,1);capacity=win64_argument(c,2);unsigned f=counted?4:3;if(counted)max_count=win64_argument(c,3);format=win64_argument(c,f);locale=win64_argument(c,f+1);va=win64_argument(c,f+2);}
    else{buffer=win64_argument(c,0);if(!strcmp(name,"vsprintf")){capacity=UINT64_MAX;format=win64_argument(c,1);va=win64_argument(c,2);}
        else{capacity=win64_argument(c,1);if(!strcmp(name,"_vsnprintf_s")){secure=1;counted=1;max_count=win64_argument(c,2);format=win64_argument(c,3);va=win64_argument(c,4);}
             else{secure=!strcmp(name,"vsprintf_s");options=!strcmp(name,"vsnprintf")?2:!strcmp(name,"_vsnprintf")?1:0;format=win64_argument(c,2);va=win64_argument(c,3);}}}
    c->rax=(uint32_t)format_buffer(c,options,buffer,capacity,max_count,format,locale,va,secure,counted);return 1;
}
void win64_stdio_self_test(void) {
    CPU c={0};c.rsp=0x12345678;
    for(unsigned i=0;i<3;++i){c.rcx=i;if(!win64_stdio_call(&c,"__acrt_iob_func")||c.rsp!=0x12345678||host_file(c.rax)!=(i==0?stdin:i==1?stdout:stderr))stdio_stop("opaque stream self-test");win64_pointer(c.rax,sizeof(guest_file),0);}
    uint64_t stack=win64_alloc(256,1),va=win64_alloc(64,1),buffer=win64_alloc(256,1);unsigned cases=0;
    for (unsigned i=0;i<3;++i) {
        uint64_t handle=(uintptr_t)&standard_files[i];c=(CPU){0};c.rsp=stack;c.rcx=handle;c.rdx=buffer;c.r8=buffer+8;c.r9=buffer+16;
        if(!win64_stdio_call(&c,"_get_stream_buffer_pointers")||c.rax||c.rsp!=stack)stdio_stop("stream buffer pointer return self-test");
        uint64_t *aliases=(uint64_t*)win64_pointer(buffer,24,0);
        if(aliases[0]!=(uintptr_t)&standard_files[i].base || aliases[1]!=(uintptr_t)&standard_files[i].ptr ||
           aliases[2]!=(uintptr_t)&standard_files[i].count || *(uint64_t*)win64_pointer(aliases[0],8,0) ||
           *(uint64_t*)win64_pointer(aliases[1],8,0) || *(int32_t*)win64_pointer(aliases[2],4,0))stdio_stop("unbuffered field alias self-test");
        *(int32_t*)win64_pointer(aliases[2],4,1)=-7;
        c.rcx=handle;c.rdx=buffer+24;c.r8=c.r9=0;win64_stdio_call(&c,"_get_stream_buffer_pointers");
        if(*(uint64_t*)win64_pointer(buffer+24,8,0)!=aliases[0] || standard_files[i].count!=-7)stdio_stop("persistent writable field self-test");
        standard_files[i].count=0;++cases;
    }
    c=(CPU){0};c.rsp=stack;c.rcx=(uintptr_t)&standard_files[2];
    win64_stdio_call(&c,"_lock_file");win64_stdio_call(&c,"_lock_file");
    win64_stdio_call(&c,"_unlock_file");win64_stdio_call(&c,"_unlock_file");if(c.rsp!=stack)stdio_stop("FILE lock return self-test");++cases;
    const struct {const char *format,*expected;uint64_t options,values[4];} tests[]={
        {"%ld|%lu|%I64d|%Iu","305419896|305419896|9223372036854775807|18446744073709551615",0,{UINT64_C(0xffffffff12345678),UINT64_C(0xffffffff12345678),INT64_MAX,UINT64_MAX}},
        {"%*.*f","    1.25",32,{8,2,UINT64_C(0x3ff4000000000000)}},
        {"%.1f|%.1f|%.1f","1.3|2.3|-1.3",0,{UINT64_C(0x3ff4000000000000),UINT64_C(0x4002000000000000),UINT64_C(0xbff4000000000000)}},
        {"%.1f|%.1f|%.1f","1.2|2.2|-1.2",32,{UINT64_C(0x3ff4000000000000),UINT64_C(0x4002000000000000),UINT64_C(0xbff4000000000000)}},
        {"%a|%.1e","0x1.4000000000000p+0|1.2e+000",48,{UINT64_C(0x3ff4000000000000),UINT64_C(0x3ff4000000000000)}},
        {"%020p|%-20p","    0000000000012345|0000000000012345    ",0,{0x12345,0x12345}},
        {"%hhd|%hu|%#08x|%%","-1|65535|0x000abc|%",0,{255,UINT64_MAX,0xabc}},
        {"%.0s|%c|%+.0d","|Q|+",0,{UINT64_MAX,'Q',0}},
    };
    for(size_t i=0;i<sizeof(tests)/sizeof(tests[0]);++i){
        const char *f=tests[i].format;win64_region_add((uintptr_t)f,strlen(f)+1,0);memcpy(win64_pointer(va,32,1),tests[i].values,32);
        uint64_t *s=(uint64_t*)win64_pointer(stack,256,1);s[5]=0;s[6]=va;
        c=(CPU){0};c.rsp=stack;c.rcx=tests[i].options;c.rdx=buffer;c.r8=256;c.r9=(uintptr_t)f;c.mxcsr=0x1f80;
        if(!win64_stdio_call(&c,"__stdio_common_vsprintf")||c.rsp!=stack||(int32_t)c.rax!=(int)strlen(tests[i].expected)||strcmp((const char*)win64_pointer(buffer,strlen(tests[i].expected)+1,0),tests[i].expected))stdio_stop("format ABI self-test");
        win64_region_remove((uintptr_t)f);++cases;
    }
    const char *format="%d";win64_region_add((uintptr_t)format,3,0);*(uint64_t*)win64_pointer(va,8,1)=123;
    for(unsigned flags=0;flags<=2;++flags)for(unsigned cap=0;cap<=4;++cap){
        memset(win64_pointer(buffer,256,1),'X',256);c=(CPU){0};c.rsp=stack;c.rcx=flags;c.rdx=buffer;c.r8=cap;c.r9=(uintptr_t)format;
        int result=flags==2?3:!cap?-1:flags==1?(cap<3?-1:3):(cap<4?-2:3);
        if(!win64_stdio_call(&c,"__stdio_common_vsprintf")||(int32_t)c.rax!=result||c.rsp!=stack)stdio_stop("format truncation return self-test");
        const char *p=(const char*)win64_pointer(buffer,256,0);if(p[cap]!='X')stdio_stop("format output canary self-test");
        if(cap&&flags!=1&&p[(cap<4?cap-1:3)]!=0)stdio_stop("format termination self-test");++cases;
    }
    /* %n consumes another eight-byte va slot even for its four-byte target. */
    const char *count_format="%d%n";uint64_t count=win64_alloc(8,1);int old_count=count_exchange(1);win64_region_add((uintptr_t)count_format,strlen(count_format)+1,0);
    for(unsigned flags=0;flags<=2;++flags)for(unsigned cap=0;cap<=4;++cap){
        *(uint64_t*)win64_pointer(count,8,1)=777;uint64_t *v=(uint64_t*)win64_pointer(va,16,1);v[0]=123;v[1]=count;
        uint64_t *s=(uint64_t*)win64_pointer(stack,256,1);s[5]=0;s[6]=va;
        c=(CPU){0};c.rsp=stack;c.rcx=flags;c.rdx=buffer;c.r8=cap;c.r9=(uintptr_t)count_format;
        if(!win64_stdio_call(&c,"__stdio_common_vsprintf")||c.rsp!=stack||*(uint32_t*)win64_pointer(count,4,0)!=(flags==2||cap>=3?3u:777u))stdio_stop("printf count/truncation self-test");++cases;
    }
    count_exchange(old_count);win64_region_remove((uintptr_t)count_format);win64_free(count);
    uint64_t *s=(uint64_t*)win64_pointer(stack,256,1);s[5]=(uintptr_t)format;s[6]=0;s[7]=va;
    c=(CPU){0};c.rsp=stack;c.rdx=buffer;c.r8=3;c.r9=UINT64_MAX;
    if(!win64_stdio_call(&c,"__stdio_common_vsnprintf_s")||(int32_t)c.rax!=-1||c.rsp!=stack||memcmp(win64_pointer(buffer,3,0),"12",3))stdio_stop("secure _TRUNCATE self-test");++cases;
    const char *print_format="WIN64_STDIO_VFPRINTF value=%ld\n";win64_region_add((uintptr_t)print_format,strlen(print_format)+1,0);*(uint64_t*)win64_pointer(va,8,1)=17;
    c=(CPU){0};c.rsp=stack;c.rcx=2;win64_stdio_call(&c,"__acrt_iob_func");uint64_t stream=c.rax;
    s[5]=va;c=(CPU){0};c.rsp=stack;c.rcx=32;c.rdx=stream;c.r8=(uintptr_t)print_format;
    if(!win64_stdio_call(&c,"__stdio_common_vfprintf")||(int32_t)c.rax!=30||c.rsp!=stack)stdio_stop("real console vfprintf self-test");++cases;
    win64_region_remove((uintptr_t)print_format);win64_region_remove((uintptr_t)format);win64_free(buffer);win64_free(va);win64_free(stack);
    fprintf(stderr,"WIN64_STDIO_SELF_TEST opaque_standard_streams=3 unbuffered_field_sets=3 cases=%u Win64_va_slots=8 LLP64=1 passed\n",cases);
}
