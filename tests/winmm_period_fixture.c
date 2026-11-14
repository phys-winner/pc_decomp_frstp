/* Real core CPU/TEB/errno plus production timer provider; no original PE runs. */
#include "../android/host/win64.c"
#include "win64_time.h"
#include <stdatomic.h>
#include "winmm_period_vectors.h"
static _Thread_local unsigned char timer_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(timer_teb))abort();memcpy(&v,timer_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(timer_teb))abort();memcpy(timer_teb+offset,&value,bytes);}
static atomic_uint clock_mode,checked_calls;
int __real_clock_getres(clockid_t clock,struct timespec *out);
int __wrap_clock_getres(clockid_t clock,struct timespec *out){
    unsigned mode=atomic_load(&clock_mode);
    if(mode==1){*out=(struct timespec){0,2000000};return 0;}
    if(mode==2){errno=EINVAL;return -1;}
    return __real_clock_getres(clock,out);
}
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"WINMM_PERIOD_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(const char *name,uint64_t period){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=period;c.rsp=UINT64_C(0x12345678);before=c;
    crt_errno=1927;win64_set_error(0x5a17);errno=2718;
    if(!win64_time_winmm_call(&c,name))abort();uint64_t result=c.rax;c.rax=before.rax;
    if(memcmp(&c,&before,sizeof(c))||crt_errno!=1927||win64_get_error()!=0x5a17||errno!=2718){fputs("WINMM_PERIOD_CPU_ERRORS_FAIL\n",stderr);abort();}
    atomic_fetch_add(&checked_calls,1);return result;
}
static void *one_native_thread(void *opaque){
    const period_vector *v=opaque;if(invoke(v->name,UINT64_C(0x1234567800000000)|v->period)!=v->result)abort();return NULL;
}
static void *paired_worker(void *opaque){
    (void)opaque;for(unsigned i=0;i<2048;++i)if(invoke("timeBeginPeriod",1)||invoke("timeEndPeriod",1))abort();return NULL;
}
static uint64_t monotonic_ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return(uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;}
int main(int argc,char **argv){
    if(argc==2){
        if(!strcmp(argv[1],"--other-begin"))invoke("timeBeginPeriod",2);
        else if(!strcmp(argv[1],"--other-end"))invoke("timeEndPeriod",2);
        else if(!strcmp(argv[1],"--large-begin"))invoke("timeBeginPeriod",UINT64_MAX);
        else if(!strcmp(argv[1],"--large-end"))invoke("timeEndPeriod",UINT64_MAX);
        else return 3;return 99;
    }
    CHECK(argc==1);struct timespec resolution;CHECK(!__real_clock_getres(CLOCK_MONOTONIC,&resolution));
    CHECK(!resolution.tv_sec&&resolution.tv_nsec>0&&resolution.tv_nsec<=1000000);
    for(unsigned i=0;i<sizeof(period_vectors)/sizeof(period_vectors[0]);++i){const period_vector *v=&period_vectors[i];
        if(v->worker){pthread_t t;CHECK(!pthread_create(&t,NULL,one_native_thread,(void*)v));CHECK(!pthread_join(t,NULL));}
        else CHECK(invoke(v->name,UINT64_C(0xfedcba9800000000)|v->period)==v->result);
    }
    CHECK(invoke("timeEndPeriod",1)==97);
    atomic_store(&clock_mode,1);CHECK(invoke("timeBeginPeriod",1)==97);CHECK(invoke("timeEndPeriod",1)==97);
    atomic_store(&clock_mode,2);CHECK(invoke("timeBeginPeriod",1)==97);CHECK(invoke("timeEndPeriod",1)==97);
    atomic_store(&clock_mode,0);CHECK(!invoke("timeBeginPeriod",1));
    uint64_t sleep_samples[8],cond_samples[2];
    for(unsigned i=0;i<8;++i){uint64_t start=monotonic_ns();struct timespec delay={0,1000000};while(nanosleep(&delay,&delay)){CHECK(errno==EINTR);}sleep_samples[i]=monotonic_ns()-start;CHECK(sleep_samples[i]>=1000000);}
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;pthread_condattr_t attr;pthread_cond_t condition;
    CHECK(!pthread_condattr_init(&attr));CHECK(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC));CHECK(!pthread_cond_init(&condition,&attr));CHECK(!pthread_condattr_destroy(&attr));CHECK(!pthread_mutex_lock(&mutex));
    for(unsigned i=0;i<2;++i){uint64_t duration=(i+1)*UINT64_C(1000000),start=monotonic_ns(),end=start+duration;struct timespec deadline={(time_t)(end/UINT64_C(1000000000)),(long)(end%UINT64_C(1000000000))};
        int status;do{status=pthread_cond_timedwait(&condition,&mutex,&deadline);}while(status==0);CHECK(status==ETIMEDOUT);cond_samples[i]=monotonic_ns()-start;CHECK(cond_samples[i]>=duration);
    }
    CHECK(!pthread_mutex_unlock(&mutex));CHECK(!pthread_cond_destroy(&condition));CHECK(!pthread_mutex_destroy(&mutex));CHECK(!invoke("timeEndPeriod",1));CHECK(invoke("timeEndPeriod",1)==97);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,paired_worker,NULL));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));CHECK(invoke("timeEndPeriod",1)==97);
    CPU c,before;memset(&c,0xa5,sizeof(c));before=c;errno=2718;CHECK(!win64_time_winmm_call(&c,"timeGetTime")&&!memcmp(&c,&before,sizeof(c))&&errno==2718);
    printf("{\"success\":true,\"native_selected_rows\":%u,\"checks\":%u,\"checked_calls\":%u,\"parallel_pairs\":8192,\"parallel_calls\":16384,\"threads\":4,\"monotonic_clock_resolution_ns\":%ld,\"fullCPU_no_pop\":true,\"low32\":true,\"all_errors_preserved\":true,\"cross_thread_pair\":true,\"capability_failure_injection\":true,\"scheduler_latency_guarantee\":false,\"nanosleep_elapsed_ns\":[",(unsigned)(sizeof(period_vectors)/sizeof(period_vectors[0])),checks,atomic_load(&checked_calls),resolution.tv_nsec);
    for(unsigned i=0;i<8;++i)printf("%s%llu",i?",":"",(unsigned long long)sleep_samples[i]);printf("],\"condition_elapsed_ns\":[%llu,%llu]}\n",(unsigned long long)cond_samples[0],(unsigned long long)cond_samples[1]);return 0;
}
