/* Process-local Win64 SRW locks, condition variables and synchronous INIT_ONCE.
 * Guest callback addresses are sent only to the static CPU dispatcher. All
 * waits release the native metadata lock; no original x64 bytes are executable.
 */
#define _GNU_SOURCE
#include "win64_sync.h"
#include "win64_threads.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The spinlock of cpu64.h's locked read-modify-write helpers for a misaligned guest address. */
__attribute__((visibility("hidden"))) int guest_lock_unaligned;

typedef struct shared_owner {pid_t tid;unsigned count;struct shared_owner *next;} shared_owner;
typedef struct srw_state {
    uint64_t address;unsigned readers,waiting_writers;pid_t writer;
    shared_owner *owners;pthread_cond_t changed;struct srw_state *next;
} srw_state;
typedef struct cv_waiter {int done;pthread_cond_t changed;struct cv_waiter *next;} cv_waiter;
typedef struct cv_state {uint64_t address;cv_waiter *waiters;struct cv_state *next;} cv_state;
typedef struct once_state {
    uint64_t address,context;int busy,complete;pid_t owner;
    pthread_cond_t changed;struct once_state *next;
} once_state;
static pthread_mutex_t sync_mutex=PTHREAD_MUTEX_INITIALIZER;
static srw_state *srw_states;
static cv_state *cv_states;
static once_state *once_states;
static _Thread_local unsigned once_callback_depth;
// shortcut: synchronization metadata is retained by address for startup. Add
// allocation-lifetime reclamation if a profile shows growth from unique lock
// addresses; re-used zero-initialized addresses must already get fresh state.

static _Noreturn void sync_stop(const char *detail,int error){fprintf(stderr,"STOP Win64 sync %s error=%d\n",detail,error);exit(73);}
static uint64_t sync_error(uint32_t code){win64_set_error(code);return 0;}
static pid_t current_tid(void){return (pid_t)syscall(SYS_gettid);}
static void store_word(uint64_t address,uint64_t value){memcpy(win64_pointer(address,8,1),&value,8);}
static uint64_t load_word(uint64_t address){uint64_t value;memcpy(&value,win64_pointer(address,8,1),8);return value;}
static void condition_init(pthread_cond_t *condition) {
    pthread_condattr_t attr;
    if(pthread_condattr_init(&attr)||pthread_condattr_setclock(&attr,CLOCK_MONOTONIC)||pthread_cond_init(condition,&attr))sync_stop("native condition initialization",0);
    pthread_condattr_destroy(&attr);
}
static srw_state *get_srw(uint64_t address) {
    uint64_t word=load_word(address);
    for(srw_state *s=srw_states;s;s=s->next)if(s->address==address)return s;
    if(word)sync_stop("unregistered nonzero SRW lock",87);
    srw_state *s=calloc(1,sizeof(*s));if(!s)sync_stop("SRW metadata allocation",8);
    s->address=address;condition_init(&s->changed);s->next=srw_states;srw_states=s;return s;
}
static cv_state *get_cv(uint64_t address) {
    if(load_word(address))sync_stop("unregistered condition representation",87);
    for(cv_state *s=cv_states;s;s=s->next)if(s->address==address)return s;
    cv_state *s=calloc(1,sizeof(*s));if(!s)sync_stop("condition metadata allocation",8);
    s->address=address;s->next=cv_states;cv_states=s;return s;
}
static once_state *get_once(uint64_t address) {
    uint64_t word=load_word(address);
    for(once_state *s=once_states;s;s=s->next)if(s->address==address){
        if(!word&&!s->busy){s->complete=0;s->context=0;}
        else if(word!=(s->busy?1:s->context|2))sync_stop("mutated active INIT_ONCE representation",87);
        return s;
    }
    if(word)sync_stop("unregistered nonzero INIT_ONCE",87);
    once_state *s=calloc(1,sizeof(*s));if(!s)sync_stop("INIT_ONCE metadata allocation",8);
    s->address=address;condition_init(&s->changed);s->next=once_states;once_states=s;return s;
}
static shared_owner **owner_slot(srw_state *s,pid_t tid) {
    shared_owner **p=&s->owners;while(*p&&(*p)->tid!=tid)p=&(*p)->next;return p;
}
static void srw_fields(srw_state *s){store_word(s->address,s->writer?1:(uint64_t)s->readers<<4);}
/* sync_mutex is held while checking predicates and sleeping. Exclusive and
 * shared ownership live in the same state, enabling atomic CV unlock+wait. */
static int srw_acquire(srw_state *s,int shared,int attempt) {
    pid_t tid=current_tid();int ready;
    if(shared)ready=!s->writer&&!s->waiting_writers;
    else ready=!s->writer&&!s->readers;
    if(attempt&&!ready)return 0;
    if(!shared)++s->waiting_writers;
    while(shared?(s->writer||s->waiting_writers):(s->writer||s->readers))
        if(pthread_cond_wait(&s->changed,&sync_mutex))sync_stop("SRW wait",0);
    if(shared) {
        shared_owner **p=owner_slot(s,tid);
        if(!*p){*p=calloc(1,sizeof(**p));if(!*p)sync_stop("SRW reader allocation",8);(*p)->tid=tid;}
        ++(*p)->count;++s->readers;
    }else{--s->waiting_writers;s->writer=tid;}
    srw_fields(s);return 1;
}
static void srw_release(srw_state *s,int shared) {
    pid_t tid=current_tid();
    if(shared){shared_owner **p=owner_slot(s,tid);if(!*p||!s->readers)sync_stop("shared SRW release by non-owner",87);
        --s->readers;if(!--(*p)->count){shared_owner *old=*p;*p=old->next;free(old);}}
    else{if(s->writer!=tid)sync_stop("exclusive SRW release by non-owner",87);s->writer=0;}
    srw_fields(s);pthread_cond_broadcast(&s->changed);
}
static uint64_t sleep_srw(uint64_t cv_address,uint64_t srw_address,uint32_t milliseconds,uint32_t flags) {
    if(flags&~1u)return sync_error(87);
    pthread_mutex_lock(&sync_mutex);cv_state *cv=get_cv(cv_address);srw_state *srw=get_srw(srw_address);
    if(flags){shared_owner **p=owner_slot(srw,current_tid());if(!*p)sync_stop("condition sleep without shared SRW ownership",87);}
    else if(srw->writer!=current_tid())sync_stop("condition sleep without exclusive SRW ownership",87);
    cv_waiter waiter={0};condition_init(&waiter.changed);waiter.next=cv->waiters;cv->waiters=&waiter;
    srw_release(srw,flags!=0);
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    deadline.tv_sec+=milliseconds/1000;deadline.tv_nsec+=(long)(milliseconds%1000)*1000000;
    if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
    int status=0;
    while(!waiter.done&&!status)status=milliseconds==UINT32_MAX?pthread_cond_wait(&waiter.changed,&sync_mutex):pthread_cond_timedwait(&waiter.changed,&sync_mutex,&deadline);
    int woke=waiter.done;cv_waiter **p=&cv->waiters;while(*p!=&waiter)p=&(*p)->next;*p=waiter.next;
    pthread_cond_destroy(&waiter.changed);
    srw_acquire(srw,flags!=0,0);pthread_mutex_unlock(&sync_mutex);
    if(woke)return 1;return sync_error(status==ETIMEDOUT?1460:87);
}
/* Critical sections live in win64_threads.c; its import entry points take the lock in rcx. */
static void critical_call(CPU *cpu,const char *name,uint64_t address) {
    CPU call=*cpu;call.rcx=address;
    if(!win64_threads_call(&call,name))sync_stop("critical section entry point",50);
}
/* SleepConditionVariableCS. The Visual C++ runtime looks it up with GetProcAddress and calls it
 * only when two threads reach the same function-local static at once (_Init_thread_header waits
 * for the other thread's initializer), so the game needs it once in many runs.
 * The waiter is registered before the critical section is released: a wake that arrives between
 * the release and the sleep finds it, so no wake is lost. The lock is owned again on every return. */
static uint64_t sleep_critical(CPU *cpu,uint64_t cv_address,uint64_t critical,uint32_t milliseconds) {
    pthread_mutex_lock(&sync_mutex);cv_state *cv=get_cv(cv_address);
    cv_waiter waiter={0};condition_init(&waiter.changed);waiter.next=cv->waiters;cv->waiters=&waiter;
    pthread_mutex_unlock(&sync_mutex);
    critical_call(cpu,"LeaveCriticalSection",critical);
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    deadline.tv_sec+=milliseconds/1000;deadline.tv_nsec+=(long)(milliseconds%1000)*1000000;
    if(deadline.tv_nsec>=1000000000L){++deadline.tv_sec;deadline.tv_nsec-=1000000000L;}
    pthread_mutex_lock(&sync_mutex);int status=0;
    while(!waiter.done&&!status)status=milliseconds==UINT32_MAX?pthread_cond_wait(&waiter.changed,&sync_mutex):pthread_cond_timedwait(&waiter.changed,&sync_mutex,&deadline);
    int woke=waiter.done;cv_waiter **p=&cv->waiters;while(*p!=&waiter)p=&(*p)->next;*p=waiter.next;
    pthread_mutex_unlock(&sync_mutex);pthread_cond_destroy(&waiter.changed);
    critical_call(cpu,"EnterCriticalSection",critical);
    if(woke)return 1;return sync_error(status==ETIMEDOUT?1460:87);
}
typedef uint64_t (*once_callback)(CPU *,uint64_t,uint64_t,uint64_t,uint64_t);
void win64_sync_thread_exit(void) {
    if(once_callback_depth) {
        /* A non-local ExitThread would strand both pending initialization and
         * its callback context. Reject before any longjmp; never return success
         * or leave other threads asleep on an abandoned busy state. write and
         * _exit also make the negative self-test safe in a forked child. */
        static const char message[]="STOP Win64 sync ExitThread inside INIT_ONCE callback unsupported error=50\n";
        (void)write(STDERR_FILENO,message,sizeof(message)-1);_exit(73);
    }
}
static uint64_t static_once_callback(CPU *cpu,uint64_t target,uint64_t once,uint64_t parameter,uint64_t context) {
    CPU call=*cpu;
    if(call.rsp<128)sync_stop("INIT_ONCE callback stack underflow",87);
    call.rsp=(call.rsp-128)&~UINT64_C(15);win64_pointer(call.rsp-8,72,1);uint64_t before=call.rsp;
    call.rcx=once;call.rdx=parameter;call.r8=context;push64(&call,UINT64_C(0xe5ef00c0));dispatch(&call,target);
    if(call.rsp!=before)sync_stop("INIT_ONCE callback stack imbalance",87);return call.rax;
}
static uint64_t execute_once(CPU *cpu,uint64_t address,uint64_t function,uint64_t parameter,uint64_t output,once_callback invoke) {
    if(!function)return sync_error(87);
    if(output)win64_pointer(output,8,1);
    pthread_mutex_lock(&sync_mutex);once_state *s=get_once(address);
    while(s->busy){if(s->owner==current_tid())sync_stop("recursive INIT_ONCE callback",87);pthread_cond_wait(&s->changed,&sync_mutex);}
    if(s->complete){uint64_t context=s->context;pthread_mutex_unlock(&sync_mutex);if(output)store_word(output,context);return 1;}
    s->busy=1;s->owner=current_tid();store_word(address,1);pthread_mutex_unlock(&sync_mutex);
    uint64_t scratch=win64_alloc(8,1);if(!scratch) {
        pthread_mutex_lock(&sync_mutex);s->busy=0;s->owner=0;store_word(address,0);pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&sync_mutex);return 0;
    }
    ++once_callback_depth;
    uint32_t success=(uint32_t)invoke(cpu,function,address,parameter,scratch);
    --once_callback_depth;
    uint64_t context=load_word(scratch);win64_free(scratch);
    if(success&&(context&3))sync_stop("unaligned INIT_ONCE context",87);
    pthread_mutex_lock(&sync_mutex);s->busy=0;s->owner=0;
    if(success){s->complete=1;s->context=context;store_word(address,context|2);}else store_word(address,0);
    pthread_cond_broadcast(&s->changed);pthread_mutex_unlock(&sync_mutex);
    if(success&&output)store_word(output,context);return success!=0;
}
int win64_sync_call(CPU *cpu,const char *name) {
    if(!strcmp(name,"SleepConditionVariableSRW")){cpu->rax=sleep_srw(cpu->rcx,cpu->rdx,(uint32_t)cpu->r8,(uint32_t)cpu->r9);return 1;}
    if(!strcmp(name,"SleepConditionVariableCS")){cpu->rax=sleep_critical(cpu,cpu->rcx,cpu->rdx,(uint32_t)cpu->r8);return 1;}
    if(!strcmp(name,"InitOnceExecuteOnce")){cpu->rax=execute_once(cpu,cpu->rcx,cpu->rdx,cpu->r8,cpu->r9,static_once_callback);return 1;}
    if(!strcmp(name,"WakeConditionVariable")||!strcmp(name,"WakeAllConditionVariable")) {
        pthread_mutex_lock(&sync_mutex);cv_state *cv=get_cv(cpu->rcx);int all=!strcmp(name,"WakeAllConditionVariable");
        for(cv_waiter *w=cv->waiters;w;w=w->next)if(!w->done){w->done=1;pthread_cond_signal(&w->changed);if(!all)break;}
        pthread_mutex_unlock(&sync_mutex);return 1;
    }
    if(!strcmp(name,"InitializeConditionVariable")) {
        pthread_mutex_lock(&sync_mutex);cv_state *cv=cv_states;while(cv&&cv->address!=cpu->rcx)cv=cv->next;
        if(cv&&cv->waiters)sync_stop("reinitialize busy condition",87);
        store_word(cpu->rcx,0);if(!cv)get_cv(cpu->rcx);pthread_mutex_unlock(&sync_mutex);return 1;
    }
    if(!strcmp(name,"InitOnceInitialize")) {
        pthread_mutex_lock(&sync_mutex);once_state *s=once_states;while(s&&s->address!=cpu->rcx)s=s->next;
        if(s&&s->busy)sync_stop("reinitialize busy INIT_ONCE",87);
        store_word(cpu->rcx,0);if(!s)s=get_once(cpu->rcx);s->complete=0;s->context=0;
        pthread_mutex_unlock(&sync_mutex);return 1;
    }
    int initialize=!strcmp(name,"InitializeSRWLock");
    int acquire=!strcmp(name,"AcquireSRWLockExclusive")||!strcmp(name,"AcquireSRWLockShared");
    int attempt=!strcmp(name,"TryAcquireSRWLockExclusive")||!strcmp(name,"TryAcquireSRWLockShared");
    int release=!strcmp(name,"ReleaseSRWLockExclusive")||!strcmp(name,"ReleaseSRWLockShared");
    if(initialize||acquire||attempt||release) {
        int shared=strstr(name,"Shared")!=NULL;pthread_mutex_lock(&sync_mutex);srw_state *s;
        if(initialize){
            s=srw_states;while(s&&s->address!=cpu->rcx)s=s->next;
            if(s&&(s->writer||s->readers||s->waiting_writers))sync_stop("reinitialize busy SRW lock",87);
            store_word(cpu->rcx,0);if(!s)get_srw(cpu->rcx);
        }
        else s=get_srw(cpu->rcx);
        if(!initialize){if(release)srw_release(s,shared);else cpu->rax=(uint64_t)srw_acquire(s,shared,attempt);}
        pthread_mutex_unlock(&sync_mutex);return 1;
    }
    return 0;
}

static void test_require(int condition,const char *detail){if(!condition)sync_stop(detail,0);}
static uint64_t test_api(const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    CPU cpu={0};cpu.rcx=a;cpu.rdx=b;cpu.r8=d;cpu.r9=e;
    test_require(win64_sync_call(&cpu,name),"self-test API lookup");return cpu.rax;
}
typedef struct {uint64_t srw,cv,once;unsigned arrived,go,counter,callbacks;} sync_fixture;
static uint64_t test_once_callback(CPU *cpu,uint64_t target,uint64_t once,uint64_t parameter,uint64_t context) {
    (void)cpu;sync_fixture *f=(void *)(uintptr_t)parameter;
    test_require(target==0xe5fe0200&&once==(uintptr_t)&f->once,"self-test INIT_ONCE callback ABI");
    unsigned attempt=++f->callbacks;if(attempt==1)return 0;
    store_word(context,0x123400);return 1;
}
static uint64_t test_exit_callback(CPU *cpu,uint64_t target,uint64_t once,uint64_t parameter,uint64_t context) {
    (void)cpu;(void)target;(void)once;(void)parameter;(void)context;
    win64_sync_thread_exit();return 0;
}
static void test_negative_exit(void) {
    int descriptors[2];test_require(!pipe(descriptors),"self-test negative pipe");pid_t child=fork();
    test_require(child>=0,"self-test negative fork");
    if(!child) {
        close(descriptors[0]);if(dup2(descriptors[1],STDERR_FILENO)<0)_exit(99);close(descriptors[1]);
        uint64_t once=0;win64_region_add((uintptr_t)&once,8,1);CPU cpu={0};
        execute_once(&cpu,(uintptr_t)&once,1,0,0,test_exit_callback);_exit(99);
    }
    close(descriptors[1]);int status;pid_t waited;do{waited=waitpid(child,&status,0);}while(waited<0&&errno==EINTR);
    char output[256]={0};ssize_t count=read(descriptors[0],output,sizeof(output)-1);close(descriptors[0]);
    test_require(waited==child&&WIFEXITED(status)&&WEXITSTATUS(status)==73&&count>0&&
        strstr(output,"ExitThread inside INIT_ONCE callback unsupported error=50"),"self-test negative ExitThread fail-closed gate");
}
static void *test_worker(void *argument) {
    sync_fixture *f=argument;test_api("AcquireSRWLockExclusive",(uintptr_t)&f->srw,0,0,0);
    ++f->arrived;test_api("WakeAllConditionVariable",(uintptr_t)&f->cv,0,0,0);
    while(!f->go)test_require(test_api("SleepConditionVariableSRW",(uintptr_t)&f->cv,(uintptr_t)&f->srw,10000,0),"self-test condition wake");
    test_api("ReleaseSRWLockExclusive",(uintptr_t)&f->srw,0,0,0);
    uint64_t result=0;win64_region_add((uintptr_t)&result,8,1);CPU cpu={0};
    while(!execute_once(&cpu,(uintptr_t)&f->once,0xe5fe0200,(uintptr_t)f,(uintptr_t)&result,test_once_callback)){}
    test_require(result==0x123400,"self-test INIT_ONCE retained context");win64_region_remove((uintptr_t)&result);
    for(unsigned i=0;i<256;++i) {
        test_api("AcquireSRWLockExclusive",(uintptr_t)&f->srw,0,0,0);++f->counter;
        test_api("ReleaseSRWLockExclusive",(uintptr_t)&f->srw,0,0,0);
        test_api("AcquireSRWLockShared",(uintptr_t)&f->srw,0,0,0);
        test_require(!test_api("TryAcquireSRWLockExclusive",(uintptr_t)&f->srw,0,0,0),"self-test shared excludes writer");
        test_api("ReleaseSRWLockShared",(uintptr_t)&f->srw,0,0,0);
    }
    return NULL;
}
void win64_sync_self_test(void) {
    uint32_t saved_error=win64_get_error();test_negative_exit();sync_fixture f={0};win64_region_add((uintptr_t)&f,sizeof(f),1);
    f.srw=f.cv=f.once=UINT64_C(0xa5a5a5a5a5a5a5a5);
    test_api("InitializeSRWLock",(uintptr_t)&f.srw,0,0,0);
    test_api("InitializeConditionVariable",(uintptr_t)&f.cv,0,0,0);
    test_api("InitOnceInitialize",(uintptr_t)&f.once,0,0,0);
    test_require(!f.srw&&!f.cv&&!f.once,"self-test initialize arbitrary A5 storage");
    pthread_t workers[4];for(unsigned i=0;i<4;++i)test_require(!pthread_create(&workers[i],NULL,test_worker,&f),"self-test pthread creation");
    test_api("AcquireSRWLockExclusive",(uintptr_t)&f.srw,0,0,0);
    while(f.arrived<4)test_require(test_api("SleepConditionVariableSRW",(uintptr_t)&f.cv,(uintptr_t)&f.srw,10000,0),"self-test arrival condition");
    f.go=1;test_api("WakeAllConditionVariable",(uintptr_t)&f.cv,0,0,0);test_api("ReleaseSRWLockExclusive",(uintptr_t)&f.srw,0,0,0);
    for(unsigned i=0;i<4;++i)test_require(!pthread_join(workers[i],NULL),"self-test join");
    test_require(f.counter==1024&&f.callbacks==2,"self-test SRW count and INIT_ONCE failure retry");
    store_word((uintptr_t)&f.once,0);uint64_t renewed=0;win64_region_add((uintptr_t)&renewed,8,1);CPU cpu={0};
    test_require(execute_once(&cpu,(uintptr_t)&f.once,0xe5fe0200,(uintptr_t)&f,(uintptr_t)&renewed,test_once_callback)&&
        f.callbacks==3&&renewed==0x123400,"self-test fresh zeroed INIT_ONCE at reused address");
    win64_region_remove((uintptr_t)&renewed);
    test_api("AcquireSRWLockExclusive",(uintptr_t)&f.srw,0,0,0);
    test_require(!test_api("SleepConditionVariableSRW",(uintptr_t)&f.cv,(uintptr_t)&f.srw,0,0)&&win64_get_error()==1460,"self-test condition timeout");
    test_require(!test_api("TryAcquireSRWLockExclusive",(uintptr_t)&f.srw,0,0,0),"self-test timeout reacquired exclusive lock");
    test_api("ReleaseSRWLockExclusive",(uintptr_t)&f.srw,0,0,0);
    test_api("AcquireSRWLockShared",(uintptr_t)&f.srw,0,0,0);
    test_require(!test_api("SleepConditionVariableSRW",(uintptr_t)&f.cv,(uintptr_t)&f.srw,0,1)&&win64_get_error()==1460,"self-test shared condition timeout");
    test_api("ReleaseSRWLockShared",(uintptr_t)&f.srw,0,0,0);
    pthread_mutex_lock(&sync_mutex);
    srw_state **sp=&srw_states;while((*sp)->address!=(uintptr_t)&f.srw)sp=&(*sp)->next;srw_state *srw=*sp;*sp=srw->next;pthread_cond_destroy(&srw->changed);free(srw);
    cv_state **cp=&cv_states;while((*cp)->address!=(uintptr_t)&f.cv)cp=&(*cp)->next;cv_state *cv=*cp;*cp=cv->next;free(cv);
    once_state **op=&once_states;while((*op)->address!=(uintptr_t)&f.once)op=&(*op)->next;once_state *once=*op;*op=once->next;pthread_cond_destroy(&once->changed);free(once);
    pthread_mutex_unlock(&sync_mutex);win64_region_remove((uintptr_t)&f);win64_set_error(saved_error);
    fprintf(stderr,"WIN64_SYNC_SELF_TEST passed workers=4 initialize_arbitrary_A5_bytes=3 SRW_exclusive_increments=1024 shared_exclusion=1024 CV_atomic_unlock_wait=1 timeout_reacquire_exclusive_and_shared=1 INIT_ONCE_hook_fixture=1 callback_failure_retry=1 initial_callbacks=2 context_retained=1 reused_zero_once_reinitialized=1 INIT_ONCE_ExitThread_negative_exit=73\n");
}
