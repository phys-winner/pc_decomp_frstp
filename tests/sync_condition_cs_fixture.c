/* SleepConditionVariableCS of android/host/win64_sync.c on ARM64: timeout, wake, reacquired lock
 * and a ping-pong that hangs (and is killed by the alarm) if a single wake is ever lost.
 * The synchronization source is the real one; only the host services it calls are stand-ins. */
#include "../android/host/win64_sync.c"
#include <signal.h>
#include <stdlib.h>

static _Thread_local uint32_t last_error;
void *win64_pointer(uint64_t address,size_t bytes,int writable){(void)bytes;(void)writable;return (void *)(uintptr_t)address;}
void win64_set_error(uint32_t value){last_error=value;}
uint32_t win64_get_error(void){return last_error;}
uint64_t win64_alloc(size_t bytes,int zero){void *p=zero?calloc(1,bytes):malloc(bytes);return (uintptr_t)p;}
void win64_free(uint64_t address){free((void *)(uintptr_t)address);}
void win64_region_add(uint64_t address,size_t bytes,int writable){(void)address;(void)bytes;(void)writable;}
void win64_region_remove(uint64_t address){(void)address;}
void dispatch(CPU *cpu,uint64_t address){(void)cpu;(void)address;abort();}

/* A critical section with the contract of win64_threads.c: recursive, owned, fail closed. */
typedef struct {pthread_mutex_t mutex;pid_t owner;unsigned recursion;} fixture_critical;
static fixture_critical *critical_at(uint64_t address){return (fixture_critical *)(uintptr_t)address;}
static void critical_create(fixture_critical *cs){
    pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    if(pthread_mutex_init(&cs->mutex,&attr))abort();pthread_mutexattr_destroy(&attr);cs->owner=0;cs->recursion=0;
}
int win64_threads_call(CPU *c,const char *name){
    fixture_critical *cs=critical_at(c->rcx);
    if(!strcmp(name,"EnterCriticalSection")){if(pthread_mutex_lock(&cs->mutex))abort();cs->owner=current_tid();++cs->recursion;c->rax=1;return 1;}
    if(!strcmp(name,"TryEnterCriticalSection")){
        if(pthread_mutex_trylock(&cs->mutex)){c->rax=0;return 1;}
        cs->owner=current_tid();++cs->recursion;c->rax=1;return 1;
    }
    if(!strcmp(name,"LeaveCriticalSection")){
        if(!cs->recursion||cs->owner!=current_tid())abort();
        if(!--cs->recursion)cs->owner=0;
        if(pthread_mutex_unlock(&cs->mutex))abort();return 1;
    }
    return 0;
}

static uint64_t api(const char *name,uint64_t a,uint64_t b,uint64_t d){
    CPU cpu={0};cpu.rcx=a;cpu.rdx=b;cpu.r8=d;
    if(!win64_sync_call(&cpu,name)&&!win64_threads_call(&cpu,name))abort();
    return cpu.rax;
}
#define REQUIRE(condition) do{if(!(condition)){fprintf(stderr,"FAILED line %d: %s\n",__LINE__,#condition);exit(1);}}while(0)

typedef struct {fixture_critical cs;uint64_t cv;unsigned arrived,go,counter;int token;unsigned long rounds;} fixture;
static uint64_t CS(fixture *f){return (uintptr_t)&f->cs;}
static uint64_t CV(fixture *f){return (uintptr_t)&f->cv;}

static void *arrival_worker(void *argument){
    fixture *f=argument;api("EnterCriticalSection",CS(f),0,0);
    ++f->arrived;api("WakeAllConditionVariable",CV(f),0,0);
    while(!f->go)REQUIRE(api("SleepConditionVariableCS",CV(f),CS(f),10000));
    /* Returned owning the lock: the unprotected increments below would be lost otherwise. */
    for(unsigned i=0;i<20000;++i)++f->counter;
    api("LeaveCriticalSection",CS(f),0,0);return NULL;
}
static void *try_enter(void *argument){
    fixture *f=argument;return (void *)(uintptr_t)api("TryEnterCriticalSection",CS(f),0,0);
}
/* Two threads hand one token back and forth with infinite waits only. */
static void *ping(void *argument){
    fixture *f=argument;
    for(unsigned long i=0;i<f->rounds;++i){
        api("EnterCriticalSection",CS(f),0,0);
        while(f->token!=0)REQUIRE(api("SleepConditionVariableCS",CV(f),CS(f),UINT32_MAX));
        f->token=1;api("WakeAllConditionVariable",CV(f),0,0);
        api("LeaveCriticalSection",CS(f),0,0);
    }
    return NULL;
}
static void *pong(void *argument){
    fixture *f=argument;
    for(unsigned long i=0;i<f->rounds;++i){
        api("EnterCriticalSection",CS(f),0,0);
        while(f->token!=1)REQUIRE(api("SleepConditionVariableCS",CV(f),CS(f),UINT32_MAX));
        f->token=0;api("WakeConditionVariable",CV(f),0,0);
        api("LeaveCriticalSection",CS(f),0,0);
    }
    return NULL;
}
static uint64_t stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;}

int main(void){
    alarm(120); /* a lost wake leaves the ping-pong asleep for ever: fail instead of hanging */
    fixture f={0};critical_create(&f.cs);
    api("InitializeConditionVariable",CV(&f),0,0);

    /* 1. Timeout: 0 with ERROR_TIMEOUT, after at least the requested time, lock owned again. */
    api("EnterCriticalSection",CS(&f),0,0);
    uint64_t begin=stamp();win64_set_error(0);
    REQUIRE(!api("SleepConditionVariableCS",CV(&f),CS(&f),30)&&win64_get_error()==1460);
    uint64_t waited=stamp()-begin;REQUIRE(waited>=30000000&&waited<5000000000ull);
    REQUIRE(f.cs.recursion==1&&f.cs.owner==current_tid());
    pthread_t other;void *entered;
    REQUIRE(!pthread_create(&other,NULL,try_enter,&f)&&!pthread_join(other,&entered)&&!entered);
    /* A zero timeout polls: still releases and reacquires. */
    REQUIRE(!api("SleepConditionVariableCS",CV(&f),CS(&f),0)&&win64_get_error()==1460&&f.cs.recursion==1);

    /* 2. Wake: four workers announce themselves and sleep; all of them are released together. */
    pthread_t workers[4];
    for(unsigned i=0;i<4;++i)REQUIRE(!pthread_create(&workers[i],NULL,arrival_worker,&f));
    while(f.arrived<4)REQUIRE(api("SleepConditionVariableCS",CV(&f),CS(&f),10000));
    REQUIRE(f.cs.recursion==1&&f.cs.owner==current_tid());
    f.go=1;api("WakeAllConditionVariable",CV(&f),0,0);api("LeaveCriticalSection",CS(&f),0,0);
    for(unsigned i=0;i<4;++i)REQUIRE(!pthread_join(workers[i],NULL));
    REQUIRE(f.counter==80000);

    /* 3. No lost wake: 200000 hand-offs with infinite waits. */
    f.rounds=200000;f.token=0;pthread_t a,b;
    REQUIRE(!pthread_create(&a,NULL,ping,&f)&&!pthread_create(&b,NULL,pong,&f));
    REQUIRE(!pthread_join(a,NULL)&&!pthread_join(b,NULL)&&f.token==0);

    /* 4. Nothing is left registered on the condition variable. */
    pthread_mutex_lock(&sync_mutex);cv_state *cv=get_cv(CV(&f));REQUIRE(!cv->waiters);pthread_mutex_unlock(&sync_mutex);
    printf("{\"success\":true,\"timeout_ms\":%.1f,\"workers\":4,\"protected_increments\":%u,\"handoffs\":%lu}\n",waited/1e6,f.counter,2*f.rounds);
    return 0;
}
