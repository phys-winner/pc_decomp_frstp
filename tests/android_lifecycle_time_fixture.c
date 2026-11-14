/* Real time adapter with deterministic clocks and unavailable API35 zone symbols. */
#define _GNU_SOURCE
#define clock_gettime fixture_clock_gettime
#define dlsym fixture_dlsym
#include "../android/host/win64_time.c"
#undef clock_gettime
#undef dlsym
#include <assert.h>

static uint64_t mono_ns=UINT64_C(10000000000),boot_ns=UINT64_C(100000000000);
static const char *guest_tz;
static int32_t system_offset,system_dst;
static int crt_error;
int fixture_clock_gettime(clockid_t clock,struct timespec *out){uint64_t value=clock==CLOCK_BOOTTIME?boot_ns:mono_ns;out->tv_sec=value/1000000000;out->tv_nsec=value%1000000000;return 0;}
void *fixture_dlsym(void *handle,const char *name){(void)handle;(void)name;return NULL;}
int frostpunk_android_timezone_offset(int64_t seconds,int32_t *offset,int32_t *is_dst){(void)seconds;*offset=system_offset;*is_dst=system_dst;return 1;}
void win64_region_add(uint64_t address,size_t bytes,int write){(void)address;(void)bytes;(void)write;}
void *win64_pointer(uint64_t address,size_t bytes,int write){(void)bytes;(void)write;assert(address);return(void*)(uintptr_t)address;}
const char *win64_string_a(uint64_t address){return(const char*)(uintptr_t)address;}
void win64_stdio_set_errno(int value){crt_error=value;}
int win64_environment_call(CPU *cpu,const char *name){assert(!strcmp(name,"getenv"));assert(!strcmp((const char*)(uintptr_t)cpu->rcx,"TZ"));cpu->rax=(uintptr_t)guest_tz;return 1;}
static win_tm calendar(int64_t seconds){CPU cpu={0};win_tm out;cpu.rcx=(uintptr_t)&out;cpu.rdx=(uintptr_t)&seconds;errno=1927;crt_error=1234;assert(win64_time_call(&cpu,"_localtime64_s")&&cpu.rax==0&&errno==1927&&crt_error==1234);return out;}
static void reset_zone(const char *zone){guest_tz=zone;CPU cpu={0};assert(win64_time_call(&cpu,"_tzset"));}
int main(void){
    win64_time_self_test();assert(!setenv("TZ","Pacific/Honolulu",1));tzset();
    reset_zone("ABC-5:30:17");win_tm tm=calendar(0);assert(tm.hour==5&&tm.min==30&&tm.sec==17&&tm.mday==1&&tm.mon==0&&tm.year==70&&!tm.isdst);
    reset_zone("UTC8");tm=calendar(0);assert(tm.hour==16&&tm.mday==31&&tm.mon==11&&tm.year==69);
    system_offset=10800;system_dst=0;reset_zone(NULL);tm=calendar(0);assert(tm.hour==3&&!tm.isdst);
    system_offset=7200;system_dst=1;tm=calendar(INT64_C(1719792000));assert(tm.year==124&&tm.mon==6&&tm.mday==1&&tm.hour==2&&tm.isdst==1);
    assert(!strcmp(getenv("TZ"),"Pacific/Honolulu"));
    assert(win64_game_clock_ns(CLOCK_MONOTONIC)==mono_ns&&win64_game_clock_ns(CLOCK_BOOTTIME)==boot_ns);
    win64_time_pause(1);win64_time_pause(1);mono_ns+=3000000000;boot_ns+=9000000000;
    assert(win64_game_clock_ns(CLOCK_MONOTONIC)==10000000000&&win64_game_clock_ns(CLOCK_BOOTTIME)==100000000000);
    CPU cpu={0};errno=1927;assert(win64_time_winmm_call(&cpu,"timeGetTime")&&cpu.rax==100000&&errno==1927);
    win64_time_pause(0);win64_time_pause(0);assert(win64_game_clock_ns(CLOCK_MONOTONIC)==10000000000&&win64_game_clock_ns(CLOCK_BOOTTIME)==100000000000);
    mono_ns+=1000000000;boot_ns+=1000000000;assert(win64_game_clock_ns(CLOCK_MONOTONIC)==11000000000&&win64_game_clock_ns(CLOCK_BOOTTIME)==101000000000);
    win64_time_pause(1);mono_ns+=7000000000;boot_ns+=7000000000;win64_time_pause(0);assert(win64_game_clock_ns(CLOCK_MONOTONIC)==11000000000&&win64_game_clock_ns(CLOCK_BOOTTIME)==101000000000);
    puts("{\"success\":true,\"API30_timezone_fallback\":true,\"fixed_offset\":true,\"system_DST\":true,\"host_TZ_preserved\":true,\"paused_monotonic_and_boottime\":true,\"repeated_pause_resume\":true,\"timeGetTime\":true}");return 0;
}
