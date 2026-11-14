/* Windows time ABI over private Android timezone objects; no host TZ mutation. */
#include "win64_time.h"
#include "win64.h"
#include "win64_environment.h"
#include "win64_stdio.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {int32_t sec,min,hour,mday,mon,year,wday,yday,isdst;} win_tm;
_Static_assert(sizeof(win_tm)==36,"Win64 tm ABI");
_Static_assert(sizeof(time_t)==8,"64-bit Android time_t");
typedef timezone_t (*allocate_zone_fn)(const char *);
typedef struct tm *(*local_zone_fn)(timezone_t,const time_t *,struct tm *);
typedef void (*free_zone_fn)(timezone_t);
static pthread_mutex_t time_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t period_one_requests;
static allocate_zone_fn allocate_zone;
static local_zone_fn local_zone;
static free_zone_fn free_zone;
static timezone_t current_zone;
static int32_t seconds_west;
static int query_registered;
static int zone_initialized, explicit_zone, symbols_checked;
extern int frostpunk_android_timezone_offset(int64_t seconds,int32_t *offset,int32_t *is_dst) __attribute__((weak));
static const char tz_query[]="TZ";
static const char *const formats[]={"%d-%m-%Y %H-%M-%S.png","%F %T","%d/%m/%Y %H:%M","%d-%m-%Y_%H-%M-%S",""};

static _Noreturn void time_stop(const char *reason) {
    fprintf(stderr,"STOP Win64 time %s\n",reason);fflush(stderr);_Exit(73);
}
static _Noreturn void parameter_stop(const char *reason) {
    win64_stdio_set_errno(22);time_stop(reason);
}
static int letter(unsigned c){return(c>='A'&&c<='Z')||(c>='a'&&c<='z');}
/* shortcut: only measured Windows fixed-offset TZ syntax, not DST/rule strings. */
static int fixed_offset(const char *text,int32_t *out) {
    for(unsigned i=0;i<3;++i)if(!text[i]||!letter((unsigned char)text[i]))return 0;
    text+=3;int sign=1;if(*text=='-'||*text=='+'){if(*text=='-')sign=-1;++text;}
    if(*text<'0'||*text>'9')return 0;
    unsigned fields[3]={0},count=0;
    for(;;){unsigned digits=0;
        while(*text>='0'&&*text<='9'){if(++digits>2)return 0;fields[count]=fields[count]*10+(unsigned)(*text++-'0');}
        if(!digits)return 0;
        if(!*text)break;
        if(*text++!=':'||++count==3)return 0;
    }
    if(fields[0]>23||fields[1]>59||fields[2]>59)return 0;
    *out=sign*(int32_t)(fields[0]*3600+fields[1]*60+fields[2]);return 1;
}
static void symbols(void) {
    if(symbols_checked)return;
    symbols_checked=1;
    allocate_zone=(allocate_zone_fn)dlsym(RTLD_DEFAULT,"tzalloc");
    local_zone=(local_zone_fn)dlsym(RTLD_DEFAULT,"localtime_rz");
    free_zone=(free_zone_fn)dlsym(RTLD_DEFAULT,"tzfree");
    if(!allocate_zone||!local_zone||!free_zone){allocate_zone=NULL;local_zone=NULL;free_zone=NULL;}
}
/* Caller holds time_mutex; environment values become immutable at first query. */
static void refresh_zone(CPU *cpu) {
    int saved_errno=errno;symbols();
    if(!query_registered){win64_region_add((uintptr_t)tz_query,sizeof(tz_query),0);query_registered=1;}
    CPU query=*cpu;query.rcx=(uintptr_t)tz_query;
    if(!win64_environment_call(&query,"getenv"))time_stop("guest environment provider unavailable");
    const char *tz=query.rax?win64_string_a(query.rax):NULL;
    int32_t offset=0;
    if(tz&&*tz&&!fixed_offset(tz,&offset))time_stop("unsupported explicit guest TZ (fixed offsets only)");
    explicit_zone=tz&&*tz;
    if(allocate_zone){
        /* tzalloc(NULL) reads the system zone without changing the process TZ. */
        timezone_t next=allocate_zone(explicit_zone?"UTC":NULL);
        if(!next)time_stop("cannot allocate actual platform timezone");
        if(current_zone)free_zone(current_zone);
        current_zone=next;
    }
    seconds_west=offset;zone_initialized=1;errno=saved_errno;
}
static int convert_time(CPU *cpu,int64_t seconds,win_tm *out) {
    int saved_errno=errno;struct tm native;time_t adjusted;
    pthread_mutex_lock(&time_mutex);
    if(!zone_initialized)refresh_zone(cpu);
    adjusted=(time_t)(seconds-seconds_west);
    struct tm *converted;
    if(local_zone)converted=local_zone(current_zone,&adjusted,&native);
    else if(explicit_zone)converted=gmtime_r(&adjusted,&native);
    else {
        int32_t offset,is_dst;
        if(!frostpunk_android_timezone_offset||!frostpunk_android_timezone_offset(seconds,&offset,&is_dst))
            time_stop("Android system timezone provider unavailable");
        adjusted=(time_t)(seconds+offset);converted=gmtime_r(&adjusted,&native);
        if(converted)native.tm_isdst=is_dst;
    }
    pthread_mutex_unlock(&time_mutex);errno=saved_errno;
    if(!converted)return 0;
    *out=(win_tm){native.tm_sec,native.tm_min,native.tm_hour,native.tm_mday,native.tm_mon,native.tm_year,native.tm_wday,native.tm_yday,native.tm_isdst};return 1;
}

static pthread_mutex_t game_clock_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned game_clock_epoch,game_paused;
static _Atomic uint64_t pause_start[2],pause_total[2];
static uint64_t raw_clock_ns(clockid_t clock) {
    struct timespec now;if(clock_gettime(clock,&now))time_stop("game clock unavailable");
    return (uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
}
uint64_t win64_game_clock_ns(clockid_t clock) {
    unsigned index=clock==CLOCK_BOOTTIME;
    for(;;){
        unsigned epoch=atomic_load_explicit(&game_clock_epoch,memory_order_acquire);
        if(epoch&1){sched_yield();continue;}
        uint64_t now=raw_clock_ns(clock),total=atomic_load_explicit(&pause_total[index],memory_order_relaxed);
        unsigned paused=atomic_load_explicit(&game_paused,memory_order_relaxed);
        uint64_t start=atomic_load_explicit(&pause_start[index],memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if(epoch==atomic_load_explicit(&game_clock_epoch,memory_order_relaxed))return (paused?start:now)-total;
    }
}
void win64_time_pause(int paused) {
    pthread_mutex_lock(&game_clock_mutex);
    if(!!paused!=atomic_load_explicit(&game_paused,memory_order_relaxed)){
        atomic_fetch_add_explicit(&game_clock_epoch,1,memory_order_acq_rel);
        const clockid_t clocks[]={CLOCK_MONOTONIC,CLOCK_BOOTTIME};
        for(unsigned i=0;i<2;++i){
            uint64_t now=raw_clock_ns(clocks[i]);
            if(paused)atomic_store_explicit(&pause_start[i],now,memory_order_relaxed);
            else atomic_fetch_add_explicit(&pause_total[i],now-atomic_load_explicit(&pause_start[i],memory_order_relaxed),memory_order_relaxed);
        }
        atomic_store_explicit(&game_paused,!!paused,memory_order_relaxed);
        atomic_fetch_add_explicit(&game_clock_epoch,1,memory_order_release);
    }
    pthread_mutex_unlock(&game_clock_mutex);
}
typedef struct {char text[64];size_t used,left;} output;
static void emit(output *out,char c){if(out->left){if(out->used==sizeof(out->text))time_stop("numeric format buffer overflow");out->text[out->used++]=c;--out->left;}}
static int number(output *out,int64_t value,int64_t low,int64_t high,unsigned width) {
    if(value<low||value>high)return 0;
    unsigned divisor=width==4?1000:10;
    do{emit(out,(char)('0'+((unsigned)value/divisor)%10));divisor/=10;}while(divisor);return 1;
}
static int expand(output *out,char code,const win_tm *tm) {
    switch(code){
    case 'd':return number(out,tm->mday,1,31,2);
    case 'm':return number(out,(int64_t)tm->mon+1,1,12,2);
    case 'Y':return number(out,(int64_t)tm->year+1900,0,9999,4);
    case 'H':return number(out,tm->hour,0,23,2);
    case 'M':return number(out,tm->min,0,59,2);
    case 'S':return number(out,tm->sec,0,60,2);
    case 'F':
        if(!expand(out,'Y',tm))return 0;emit(out,'-');
        if(!expand(out,'m',tm))return 0;emit(out,'-');return expand(out,'d',tm);
    case 'T':
        if(!expand(out,'H',tm))return 0;emit(out,':');
        if(!expand(out,'M',tm))return 0;emit(out,':');return expand(out,'S',tm);
    default:time_stop("unproved numeric format directive");
    }
}
/* Returns -1 for an invalid-parameter callback, 0 for ERANGE, 1 for success. */
static int render(output *out,const char *format,const win_tm *tm,size_t capacity) {
    memset(out,0,sizeof(*out));out->left=capacity;
    while(*format&&out->left){
        if(*format=='%'){++format;if(!expand(out,*format++,tm))return -1;}
        else emit(out,*format++);
    }
    return *format||!out->left?0:1;
}
int win64_time_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"_tzset")){
        pthread_mutex_lock(&time_mutex);refresh_zone(cpu);pthread_mutex_unlock(&time_mutex);return 1;
    }
    if(!strcmp(name,"_localtime64_s")){
        if(!cpu->rcx)parameter_stop("invalid-parameter callback required for NULL tm");
        win_tm *out=win64_pointer(cpu->rcx,sizeof(*out),1);memset(out,0xff,sizeof(*out));
        if(!cpu->rdx)parameter_stop("invalid-parameter callback required for NULL time64");
        int64_t value;memcpy(&value,win64_pointer(cpu->rdx,sizeof(value),0),sizeof(value));
        /* Actual pinned Windows UCRT, also its SDK traits, uses this inclusive maximum. */
        if(value<0||value>INT64_C(0x793582aff)||!convert_time(cpu,value,out)){
            win64_stdio_set_errno(22);cpu->rax=22;return 1;
        }
        cpu->rax=0;return 1;
    }
    if(!strcmp(name,"strftime")){
        if(!cpu->rcx||!cpu->rdx)parameter_stop("invalid-parameter callback required for strftime destination/capacity");
        char *destination=win64_pointer(cpu->rcx,1,1);*destination=0;
        if(!cpu->r8||!cpu->r9)parameter_stop("invalid-parameter callback required for strftime format/tm");
        const char *format=win64_string_a(cpu->r8);unsigned i;
        for(i=0;i<sizeof(formats)/sizeof(*formats);++i)if(!strcmp(format,formats[i]))break;
        if(i==sizeof(formats)/sizeof(*formats))time_stop("unsupported strftime format (only four original game formats)");
        win_tm tm;memcpy(&tm,win64_pointer(cpu->r9,sizeof(tm),0),sizeof(tm));output out;
        int result=render(&out,format,&tm,(size_t)cpu->rdx);
        if(result<0)parameter_stop("invalid-parameter callback required for strftime tm field");
        if(!result){win64_stdio_set_errno(34);cpu->rax=0;return 1;}
        memcpy(win64_pointer(cpu->rcx,out.used+1,1),out.text,out.used);destination[out.used]=0;cpu->rax=out.used;return 1;
    }
    return 0;
}
void win64_time_self_test(void) {
    int32_t offset;output out;win_tm tm={59,58,23,31,11,124,2,365,0};
    if(!fixed_offset("UTC0",&offset)||offset||!fixed_offset("ABC-5:30:17",&offset)||offset!=-19817||fixed_offset("UTC0DST",&offset)||fixed_offset("UTC24",&offset))time_stop("offset parser self-test");
    if(render(&out,formats[1],&tm,20)!=1||out.used!=19||memcmp(out.text,"2024-12-31 23:58:59",19)||render(&out,formats[1],&tm,19)!=0||render(&out,formats[4],&tm,1)!=1)time_stop("numeric formatter self-test");
    tm.mon=12;if(render(&out,formats[1],&tm,20)!=-1)time_stop("invalid tm self-test");
}

int win64_time_winmm_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"timeGetTime")){int saved_errno=errno;cpu->rax=(uint32_t)(win64_game_clock_ns(CLOCK_BOOTTIME)/1000000);errno=saved_errno;return 1;}
    int begin=!strcmp(name,"timeBeginPeriod");
    if(!begin&&strcmp(name,"timeEndPeriod"))return 0;
    int saved_errno=errno;uint32_t period=(uint32_t)cpu->rcx,result=97;
    if(!period){cpu->rax=97;errno=saved_errno;return 1;}
    /* shortcut: only SDL's proven 1ms request is implemented. Android's
     * existing nanosleep/monotonic-condition backend already uses ns deadlines;
     * acknowledge it only with an actual <=1ms monotonic clock resolution.
     * This is neither a scheduler latency guarantee nor an OS-wide timer change. */
    if(period!=1)time_stop("WinMM period other than 1ms is unsupported");
    pthread_mutex_lock(&time_mutex);
    if(begin){
        struct timespec resolution;
        if(!clock_getres(CLOCK_MONOTONIC,&resolution)&&resolution.tv_sec==0&&resolution.tv_nsec>0&&resolution.tv_nsec<=1000000L){
            if(period_one_requests==UINT64_MAX){pthread_mutex_unlock(&time_mutex);time_stop("WinMM period request counter overflow");}
            ++period_one_requests;result=0;
        }
    }else if(period_one_requests){--period_one_requests;result=0;}
    pthread_mutex_unlock(&time_mutex);cpu->rax=result;errno=saved_errno;return 1;
}
