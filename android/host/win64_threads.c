/* Native pthread lifecycle for statically dispatched Win64 guest functions.
 * Suspended creation is a real start latch. Asynchronous suspension of running
 * threads is deliberately unsupported; no signal-based fake suspension.
 */
#define _GNU_SOURCE
#include "win64_threads.h"
#include "win64_kernel.h"
#include "win64_sync.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TLS_SLOTS 1088u
#define STILL_ACTIVE 259u
#define CURRENT_THREAD (UINT64_MAX-1)
typedef struct thread_state thread_state;
struct thread_state {
    thread_state *next;CPU cpu;pthread_t native;pid_t tid;
    void *token;uint64_t initial_handle,start,parameter;
    void *stack_mapping;size_t mapping_bytes,stack_bytes;uint64_t stack_low;
    uint64_t *tls_first;uint64_t tls_expansion[TLS_SLOTS-64];
    unsigned suspend_count;int published,started,attached,boundary,hidden_handle,registered,finishing;
    char guest_name[256];
    uint32_t exit_code;jmp_buf exit_boundary;
};
typedef struct critical_state {
    uint64_t address;pthread_mutex_t mutex;atomic_uint active;unsigned recursion;_Atomic pid_t owner;
    uint32_t spin;struct critical_state *next;
} critical_state;
/* Address index of live critical sections: open addressing, mutated under
 * threads_mutex, read without it. A deleted slot keeps a tombstone. */
#define CRITICAL_SLOTS 65536u
#define CRITICAL_TOMBSTONE ((critical_state *)(uintptr_t)1)
static _Atomic(critical_state *) critical_index[CRITICAL_SLOTS];
static unsigned critical_used;
static pthread_mutex_t threads_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t threads_changed=PTHREAD_COND_INITIALIZER;
static pthread_once_t threads_once=PTHREAD_ONCE_INIT;
static pthread_key_t current_key;
static thread_state *states;
static critical_state *criticals;
static unsigned char tls_allocated[TLS_SLOTS];
static win64_thread_hooks host;
static size_t default_stack_bytes=2*1024*1024;
static int configured,process_ready;
static _Thread_local thread_state *current;

static _Noreturn void stop_thread(const char *what,int error) {
    fprintf(stderr,"STOP Win64 thread %s error=%d\n",what,error);fflush(stderr);_Exit(73);
}
static uint64_t failure(uint32_t error,uint64_t value){win64_set_error(error);return value;}
static void close_id(uint64_t id){CPU c={0};c.rcx=id;if(!win64_kernel_call(&c,"CloseHandle")||!c.rax)stop_thread("internal handle close",0);}
static void destroy_state(void *pointer){free(pointer);}
static void remove_state(thread_state *state) {
    pthread_mutex_lock(&threads_mutex);
    thread_state **p=&states;while(*p&&*p!=state)p=&(*p)->next;if(*p)*p=state->next;
    pthread_mutex_unlock(&threads_mutex);
}
static void finish_state(thread_state *state) {
    win64_sync_thread_exit();
    pthread_mutex_lock(&threads_mutex);
    if(state->finishing)stop_thread("recursive exit during thread detach",50);
    state->finishing=1;
    pthread_mutex_unlock(&threads_mutex);
    if(state->attached){host.detach(&state->cpu);state->attached=0;}
    remove_state(state);
    if(state->registered)win64_region_remove((uintptr_t)state->tls_expansion);
    if(state->stack_mapping){win64_region_remove(state->stack_low);munmap(state->stack_mapping,state->mapping_bytes);}
    uint32_t code=state->exit_code;void *token=state->token;uint64_t hidden=state->hidden_handle?state->initial_handle:0;
    current=NULL;pthread_setspecific(current_key,NULL);
    if(hidden)close_id(hidden);
    /* This may free state; it is the final access in the native thread. */
    win64_kernel_thread_complete(token,code);
}
static void key_cleanup(void *pointer){if(pointer)finish_state(pointer);}
static void initialize(void){if(pthread_key_create(&current_key,key_cleanup))stop_thread("pthread key",0);}
void win64_threads_configure(const win64_thread_hooks *hooks,size_t stack) {
    pthread_once(&threads_once,initialize);pthread_mutex_lock(&threads_mutex);
    if(configured){pthread_mutex_unlock(&threads_mutex);stop_thread("configured twice",0);}
    host=*hooks;if(!host.teb_current||!host.attach||!host.detach||!host.call){pthread_mutex_unlock(&threads_mutex);stop_thread("incomplete host hooks",0);}
    default_stack_bytes=stack?stack:default_stack_bytes;configured=1;pthread_mutex_unlock(&threads_mutex);
}
void win64_threads_process_ready(void){pthread_mutex_lock(&threads_mutex);process_ready=1;pthread_cond_broadcast(&threads_changed);pthread_mutex_unlock(&threads_mutex);}
static void prepare_tls(thread_state *state) {
    state->tls_first=(uint64_t *)((unsigned char *)host.teb_current()+0x1480);
    for(unsigned i=0;i<64;++i)frostpunk_teb_write(0x1480+i*8,0,8);
    frostpunk_teb_write(0x1780,(uintptr_t)state->tls_expansion,8);
    win64_region_add((uintptr_t)state->tls_expansion,sizeof(state->tls_expansion),1);state->registered=1;
    pthread_mutex_lock(&threads_mutex);state->next=states;states=state;pthread_mutex_unlock(&threads_mutex);
}
static thread_state *get_current(void) {
    pthread_once(&threads_once,initialize);if(current)return current;
    if(!configured){win64_set_error(50);return NULL;}
    thread_state *state=calloc(1,sizeof(*state));if(!state){win64_set_error(8);return NULL;}
    state->tid=(pid_t)syscall(SYS_gettid);state->native=pthread_self();state->started=state->published=1;state->hidden_handle=1;
    state->initial_handle=win64_kernel_thread_create(state,destroy_state,0,&state->token);
    if(!state->initial_handle){free(state);return NULL;}
    current=state;pthread_setspecific(current_key,state);prepare_tls(state);return state;
}
static size_t thread_label(const char *name,char native[16]) {
    size_t bytes=strlen(name),at=0,prefix=0;
    /* shortcut: retain at most255 UTF8 bytes; extend if a real longer name arrives. */
    if(bytes>255)stop_thread("thread name exceeds registry capacity",50);
    while(at<bytes){size_t begin=at;uint32_t cp;unsigned more;unsigned char first=(unsigned char)name[at++];
        if(first<0x80){cp=first;more=0;}
        else if(first>=0xc2&&first<=0xdf){cp=first&31u;more=1;}
        else if(first>=0xe0&&first<=0xef){cp=first&15u;more=2;}
        else if(first>=0xf0&&first<=0xf4){cp=first&7u;more=3;}
        else stop_thread("invalid UTF8 thread name",1113);
        if(more>bytes-at)stop_thread("truncated UTF8 thread name",1113);
        for(unsigned i=0;i<more;++i){unsigned char c=(unsigned char)name[at++];if((c&0xc0)!=0x80)stop_thread("invalid UTF8 thread name continuation",1113);cp=(cp<<6)|(c&63u);}
        if((more==1&&cp<0x80)||(more==2&&cp<0x800)||(more==3&&cp<0x10000)||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))stop_thread("non-scalar UTF8 thread name",1113);
        if(begin==prefix&&at<=15)prefix=at;
    }
    memcpy(native,name,prefix);native[prefix]=0;return bytes;
}
int win64_threads_raise_exception(CPU *cpu) {
    if((uint32_t)cpu->rcx!=UINT32_C(0x406d1388))return 0;
    uint64_t caller;memcpy(&caller,win64_pointer(cpu->rsp,8,0),8);uint32_t count=(uint32_t)cpu->r8;
    unsigned expected=caller==UINT64_C(0x140e2793b)||caller==UINT64_C(0x141b1aa8c)?6:caller==UINT64_C(0x141bd2a83)?3:0;
    if(!expected)return 0;
    if((uint32_t)cpu->rdx||count!=expected)stop_thread("unsupported MSVC thread-name exception flags/count",50);
    const unsigned char *record=win64_pointer(cpu->r9,(size_t)count*8,0);uint32_t type,tid,flags;uint64_t name;
    memcpy(&type,record,4);memcpy(&name,record+8,8);memcpy(&tid,record+16,4);memcpy(&flags,record+20,4);
    if(type!=0x1000||flags||!name)stop_thread("invalid MSVC thread-name record",87);
    const char *text=win64_string_a(name);char native[16];size_t bytes=thread_label(text,native);uint32_t saved_error=win64_get_error();int saved_errno=errno;
    thread_state *self=tid==UINT32_MAX?get_current():NULL;if(tid==UINT32_MAX&&!self)stop_thread("thread-name current context unavailable",50);
    pthread_mutex_lock(&threads_mutex);thread_state *target=self;
    if(!target)for(thread_state *state=states;state;state=state->next)if((uint32_t)state->tid==tid){target=state;break;}
    if(!target||!target->published||target->finishing){pthread_mutex_unlock(&threads_mutex);stop_thread("thread-name target is not a live guest thread",6);}
    int status=pthread_setname_np(target->native,native);
    if(!status)memcpy(target->guest_name,text,bytes+1);
    pthread_mutex_unlock(&threads_mutex);errno=saved_errno;
    if(status)stop_thread("native pthread thread-name operation failed",status);
    win64_set_error(saved_error);return 1;
}
void win64_threads_adopt_current(CPU *cpu) {
    thread_state *state=get_current();if(!state)stop_thread("adopt main thread context",0);
    state->cpu=*cpu;state->attached=1;
}
static void *native_start(void *argument) {
    thread_state *state=argument;current=state;pthread_setspecific(current_key,state);
    state->tid=(pid_t)syscall(SYS_gettid);prepare_tls(state);
    pthread_mutex_lock(&threads_mutex);state->published=1;pthread_cond_broadcast(&threads_changed);
    while(!process_ready||state->suspend_count)pthread_cond_wait(&threads_changed,&threads_mutex);
    state->started=1;pthread_mutex_unlock(&threads_mutex);
    /* The exit boundary also covers DLL/TLS callbacks. A guest ExitThread has
     * one cleanup path rather than falling through generated C call frames. */
    state->boundary=1;
    if(!setjmp(state->exit_boundary)) {
        state->attached=1;
        if(!host.attach(&state->cpu,state->stack_low,state->stack_bytes))stop_thread("host attach",0);
        state->exit_code=(uint32_t)host.call(&state->cpu,state->start,state->parameter);
    }
    state->boundary=0;finish_state(state);return NULL;
}
static uint64_t create_thread(CPU *c,int crt) {
    if(!configured)return failure(50,0);
    uint64_t security=c->rcx;int inherit=0;
    if(security){const unsigned char *p=win64_pointer(security,24,0);uint32_t n,flag;uint64_t descriptor;
        memcpy(&n,p,4);memcpy(&descriptor,p+8,8);memcpy(&flag,p+16,4);
        if(n!=24)return failure(87,0);if(descriptor)return failure(50,0);inherit=flag!=0;}
    uint32_t flags=(uint32_t)win64_argument(c,4);if(flags&~0x10004u)return failure(87,0);
    uint64_t wanted=crt?(uint32_t)c->rdx:c->rdx;
    if(wanted>64*1024*1024)return failure(8,0);
    size_t reserve=(flags&0x10000)?(wanted?wanted:default_stack_bytes):default_stack_bytes;
    if(reserve<wanted)reserve=(size_t)wanted;
    if(reserve<65536)reserve=65536;
    reserve=(reserve+65535)&~(size_t)65535;
    thread_state *state=calloc(1,sizeof(*state));if(!state)return failure(8,0);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);state->mapping_bytes=reserve+page*2;state->stack_bytes=reserve;
    state->stack_mapping=mmap(NULL,state->mapping_bytes,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(state->stack_mapping==MAP_FAILED){free(state);return failure(8,0);}
    state->stack_low=(uintptr_t)state->stack_mapping+page;
    if(mprotect((void *)(uintptr_t)state->stack_low,reserve,PROT_READ|PROT_WRITE)){munmap(state->stack_mapping,state->mapping_bytes);free(state);return failure(8,0);}
    win64_region_add(state->stack_low,reserve,1);
    state->start=c->r8;state->parameter=c->r9;state->suspend_count=(flags&4)?1:0;
    state->initial_handle=win64_kernel_thread_create(state,destroy_state,inherit,&state->token);
    if(!state->initial_handle){win64_region_remove(state->stack_low);munmap(state->stack_mapping,state->mapping_bytes);free(state);return 0;}
    pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    int result=pthread_create(&state->native,&attr,native_start,state);pthread_attr_destroy(&attr);
    if(result){close_id(state->initial_handle);win64_region_remove(state->stack_low);munmap(state->stack_mapping,state->mapping_bytes);win64_kernel_thread_complete(state->token,(uint32_t)result);return failure(8,0);}
    pthread_mutex_lock(&threads_mutex);while(!state->published)pthread_cond_wait(&threads_changed,&threads_mutex);
    uint32_t tid=(uint32_t)state->tid;uint64_t id=state->initial_handle;pthread_mutex_unlock(&threads_mutex);
    uint64_t output=win64_argument(c,5);if(output)memcpy(win64_pointer(output,4,1),&tid,4);
#ifdef FP_WINDOW_TRACE
    fprintf(stderr,"THREAD_CREATE start=0x%llx parameter=0x%llx tid=%u suspended=%u\n",(unsigned long long)state->start,(unsigned long long)state->parameter,tid,(flags&4)!=0);
#endif
    return id;
}
static thread_state *acquire_thread(uint64_t id,uint32_t access,void **token) {
    if(id==CURRENT_THREAD){thread_state *state=get_current();if(!state)return NULL;*token=state->token;
        uint64_t temporary=win64_kernel_thread_duplicate(state->token,0x1fffff,0);
        if(!temporary)return NULL;thread_state *result=win64_kernel_thread_acquire(temporary,access,token);close_id(temporary);return result;}
    return win64_kernel_thread_acquire(id,access,token);
}
static _Noreturn void exit_current(uint32_t code) {
    win64_sync_thread_exit();
    thread_state *state=get_current();if(!state)stop_thread("ExitThread without context",0);
    state->exit_code=code;if(state->boundary)longjmp(state->exit_boundary,1);
    finish_state(state);pthread_exit(NULL);
}
static size_t critical_hash(uint64_t address) {
    return (size_t)(((address>>3)*UINT64_C(0x9E3779B97F4A7C15))>>48)&(CRITICAL_SLOTS-1);
}
static critical_state *critical_lookup(uint64_t address) {
    for(size_t i=critical_hash(address),probe=0;probe<CRITICAL_SLOTS;++probe,i=(i+1)&(CRITICAL_SLOTS-1)) {
        critical_state *cs=atomic_load_explicit(&critical_index[i],memory_order_acquire);
        if(!cs)return NULL;
        if(cs!=CRITICAL_TOMBSTONE&&cs->address==address)return cs;
    }
    return NULL;
}
/* Caller holds threads_mutex. */
static void critical_index_insert(critical_state *cs) {
    if(++critical_used>CRITICAL_SLOTS*3/4)stop_thread("critical section index full",8);
    for(size_t i=critical_hash(cs->address);;i=(i+1)&(CRITICAL_SLOTS-1)) {
        critical_state *seen=atomic_load_explicit(&critical_index[i],memory_order_relaxed);
        if(!seen||seen==CRITICAL_TOMBSTONE){atomic_store_explicit(&critical_index[i],cs,memory_order_release);return;}
    }
}
static void critical_index_remove(critical_state *cs) {
    for(size_t i=critical_hash(cs->address),probe=0;probe<CRITICAL_SLOTS;++probe,i=(i+1)&(CRITICAL_SLOTS-1))
        if(atomic_load_explicit(&critical_index[i],memory_order_relaxed)==cs){atomic_store_explicit(&critical_index[i],CRITICAL_TOMBSTONE,memory_order_release);return;}
}
/* LockCount/RecursionCount/OwningThread mirror the host lock for guest
 * readers; each field is one aligned store. */
static void critical_fields(critical_state *cs) {
    unsigned char *p=(unsigned char *)(uintptr_t)cs->address;
    int32_t lock=(int32_t)atomic_load_explicit(&cs->active,memory_order_relaxed)-1;
    uint64_t owner=(uint32_t)atomic_load_explicit(&cs->owner,memory_order_relaxed);
    __atomic_store_n((int32_t *)(p+8),lock,__ATOMIC_RELAXED);
    __atomic_store_n((uint32_t *)(p+12),cs->recursion,__ATOMIC_RELAXED);
    __atomic_store_n((uint64_t *)(p+16),owner,__ATOMIC_RELAXED);
}
static int critical_initialize(uint64_t address,uint32_t spin) {
    void *guest=win64_pointer(address,40,1);pthread_mutex_lock(&threads_mutex);
    if(critical_lookup(address)){pthread_mutex_unlock(&threads_mutex);win64_set_error(87);return 0;}
    critical_state *cs=calloc(1,sizeof(*cs));if(!cs){pthread_mutex_unlock(&threads_mutex);win64_set_error(8);return 0;}
    pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    int status=pthread_mutex_init(&cs->mutex,&attr);pthread_mutexattr_destroy(&attr);
    if(status){free(cs);pthread_mutex_unlock(&threads_mutex);win64_set_error(8);return 0;}
    cs->address=address;cs->spin=spin;cs->next=criticals;criticals=cs;critical_index_insert(cs);
    memset(guest,0,40);uint64_t count=spin;memcpy((unsigned char *)guest+32,&count,8);critical_fields(cs);
    pthread_mutex_unlock(&threads_mutex);return 1;
}
static int critical_enter(uint64_t address,int try_only) {
    critical_state *cs=critical_lookup(address);
    if(!cs)stop_thread("uninitialized critical section",0);
    atomic_fetch_add_explicit(&cs->active,1,memory_order_relaxed);
    int result=try_only?pthread_mutex_trylock(&cs->mutex):pthread_mutex_lock(&cs->mutex);
    if(result){atomic_fetch_sub_explicit(&cs->active,1,memory_order_relaxed);if(try_only&&result==EBUSY)return 0;stop_thread("critical mutex lock",result);}
    atomic_store_explicit(&cs->owner,gettid(),memory_order_relaxed);++cs->recursion;critical_fields(cs);return 1;
}
static void critical_leave(uint64_t address) {
    critical_state *cs=critical_lookup(address);
    if(!cs||!cs->recursion||atomic_load_explicit(&cs->owner,memory_order_relaxed)!=gettid())stop_thread("critical section not owned",0);
    --cs->recursion;atomic_fetch_sub_explicit(&cs->active,1,memory_order_relaxed);
    if(!cs->recursion)atomic_store_explicit(&cs->owner,0,memory_order_relaxed);
    critical_fields(cs);
    int status=pthread_mutex_unlock(&cs->mutex);if(status)stop_thread("critical mutex unlock",status);
}
static int fast_enter_critical(CPU *c){c->rax=(uint64_t)critical_enter(c->rcx,0);return 1;}
static int fast_try_enter_critical(CPU *c){c->rax=(uint64_t)critical_enter(c->rcx,1);return 1;}
static int fast_leave_critical(CPU *c){critical_leave(c->rcx);return 1;}
void *win64_threads_fast_import(const char *name) {
    if(!strcmp(name,"EnterCriticalSection"))return (void *)fast_enter_critical;
    if(!strcmp(name,"TryEnterCriticalSection"))return (void *)fast_try_enter_critical;
    if(!strcmp(name,"LeaveCriticalSection"))return (void *)fast_leave_critical;
    return NULL;
}
/* Android profile shortcut: this exact legacy startup request pins the
 * current thread immediately before QueryPerformanceCounter -> srand. Our
 * CLOCK_MONOTONIC counter is migration-safe; keep the inherited OS mask so
 * subsequently created guest and native workers can use the allowed CPUs.
 * All other handles, masks and callsites retain the generic affinity API. */
static int preserve_qpc_seed_affinity(CPU *c,const thread_state *state,pid_t tid) {
    if(c->rcx!=CURRENT_THREAD||c->rdx!=1||state!=current||state->tid!=tid||tid!=(pid_t)syscall(SYS_gettid))return 0;
    uint64_t caller;memcpy(&caller,win64_pointer(c->rsp,8,0),8);
    if(caller!=UINT64_C(0x140e1bd60))return 0;
    static const unsigned char original[20]={0xff,0x15,0x16,0xb7,0xea,0x00,0x48,0x8b,0xc8,0xba,0x01,0x00,0x00,0x00,0xff,0x15,0xa8,0xb6,0xea,0x00};
    if(memcmp(win64_pointer(GVA(0x140e1bd4c),sizeof(original),0),original,sizeof(original)))stop_thread("QPC seed affinity source bytes differ",50);
    return 1;
}
static int apply_thread_affinity(CPU *c,const thread_state *state,pid_t tid,uint64_t *old) {
    cpu_set_t before,after;CPU_ZERO(&before);CPU_ZERO(&after);
    uint64_t mask=c->rdx;*old=0;int status=mask?sched_getaffinity(tid,sizeof(before),&before):-1;
    if(!status){
        for(unsigned i=0;i<64;++i){if(CPU_ISSET(i,&before))*old|=UINT64_C(1)<<i;if(mask&(UINT64_C(1)<<i))CPU_SET(i,&after);}
        if(!preserve_qpc_seed_affinity(c,state,tid))status=sched_setaffinity(tid,sizeof(after),&after);
    }
    return status;
}

int win64_threads_call(CPU *c,const char *name) {
    if(!strcmp(name,"CreateThread")||!strcmp(name,"_beginthreadex")){c->rax=create_thread(c,!strcmp(name,"_beginthreadex"));return 1;}
    if(!strcmp(name,"ExitThread")||!strcmp(name,"_endthreadex"))exit_current((uint32_t)c->rcx);
    if(!strcmp(name,"ResumeThread")||!strcmp(name,"SuspendThread")) {
        void *token;thread_state *state=acquire_thread(c->rcx,2,&token);if(!state){c->rax=UINT32_MAX;return 1;}
        pthread_mutex_lock(&threads_mutex);unsigned previous=state->suspend_count;int error=0;
        if(state->finishing)error=6;
        else if(!strcmp(name,"SuspendThread")){if(state->started||previous>=127)error=50;else ++state->suspend_count;}
        else if(state->suspend_count)--state->suspend_count;
        pthread_cond_broadcast(&threads_changed);pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);
        c->rax=error?failure((uint32_t)error,UINT32_MAX):previous;return 1;
    }
    if(!strcmp(name,"GetExitCodeThread")) {
        void *token;thread_state *state=acquire_thread(c->rcx,0x840,&token);if(!state){c->rax=0;return 1;}
        uint32_t code=win64_kernel_thread_exit_code(token);memcpy(win64_pointer(c->rdx,4,1),&code,4);
        win64_kernel_thread_release(token);c->rax=1;return 1;
    }
    if(!strcmp(name,"SetThreadAffinityMask")||!strcmp(name,"SetThreadPriority")) {
        void *token;thread_state *state=acquire_thread(c->rcx,0x420,&token);if(!state){c->rax=0;return 1;}
        /* finish_state takes the same lock before any cleanup or pthread exit.
         * Holding it across the OS call guarantees tid still names this native
         * thread, even if its Win64 handle survives long after completion. */
        pthread_mutex_lock(&threads_mutex);
        if(state->finishing){pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);c->rax=failure(6,0);return 1;}
        pid_t tid=state->tid;
        if(!strcmp(name,"SetThreadPriority")) {
            int priority=(int32_t)c->rdx,nice_value;
            if(priority>=-2&&priority<=2)nice_value=-priority*2;else if(priority==-15)nice_value=19;else if(priority==15)nice_value=-20;
            else{pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);c->rax=failure(87,0);return 1;}
            int result=setpriority(PRIO_PROCESS,(id_t)tid,nice_value),saved_errno=errno;
            pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);c->rax=result?failure(saved_errno==EACCES||saved_errno==EPERM?5:87,0):1;return 1;
        }
        uint64_t old=0;int status=apply_thread_affinity(c,state,tid,&old);
        pthread_mutex_unlock(&threads_mutex);win64_kernel_thread_release(token);c->rax=status?failure(87,0):old;return 1;
    }
    if(!strcmp(name,"SwitchToThread")) {
        struct rusage before,after;int first=getrusage(RUSAGE_THREAD,&before),status=sched_yield(),last=getrusage(RUSAGE_THREAD,&after);
        c->rax=!first&&!status&&!last&&(after.ru_nvcsw!=before.ru_nvcsw||after.ru_nivcsw!=before.ru_nivcsw);return 1;
    }
    if(!strcmp(name,"TlsAlloc")||!strcmp(name,"TlsFree")||!strcmp(name,"TlsSetValue")||!strcmp(name,"TlsGetValue")) {
        thread_state *state=get_current();if(!state){c->rax=!strcmp(name,"TlsAlloc")?UINT32_MAX:0;return 1;}
        pthread_mutex_lock(&threads_mutex);uint32_t index=(uint32_t)c->rcx;
        if(!strcmp(name,"TlsAlloc")) {
            for(index=0;index<TLS_SLOTS&&tls_allocated[index];++index){}
            if(index==TLS_SLOTS){pthread_mutex_unlock(&threads_mutex);c->rax=failure(8,UINT32_MAX);return 1;}
            tls_allocated[index]=1;
            for(thread_state *s=states;s;s=s->next){if(index<64)s->tls_first[index]=0;else s->tls_expansion[index-64]=0;}
            pthread_mutex_unlock(&threads_mutex);c->rax=index;return 1;
        }
        if(index>=TLS_SLOTS || (index>=64&&!tls_allocated[index]) || (!strcmp(name,"TlsFree")&&!tls_allocated[index])) {
            pthread_mutex_unlock(&threads_mutex);c->rax=failure(87,0);return 1;
        }
        uint64_t *slot=index<64?state->tls_first+index:state->tls_expansion+index-64;
        if(!strcmp(name,"TlsFree")){tls_allocated[index]=0;c->rax=1;}
        else if(!strcmp(name,"TlsSetValue")){*slot=c->rdx;c->rax=1;}
        else c->rax=*slot;
        pthread_mutex_unlock(&threads_mutex);if(!strcmp(name,"TlsGetValue"))win64_set_error(0);return 1;
    }
    if(!strcmp(name,"InitializeCriticalSection")||!strcmp(name,"InitializeCriticalSectionAndSpinCount")||!strcmp(name,"InitializeCriticalSectionEx")) {
        if(!strcmp(name,"InitializeCriticalSectionEx")&&(c->r8&~0x01000000u)){c->rax=failure(87,0);return 1;}
        int ok=critical_initialize(c->rcx,!strcmp(name,"InitializeCriticalSection")?0:(uint32_t)c->rdx);
        if(!strcmp(name,"InitializeCriticalSection")&&!ok)stop_thread("InitializeCriticalSection allocation",8);c->rax=(uint64_t)ok;return 1;
    }
    if(!strcmp(name,"EnterCriticalSection")||!strcmp(name,"TryEnterCriticalSection")){c->rax=(uint64_t)critical_enter(c->rcx,!strcmp(name,"TryEnterCriticalSection"));return 1;}
    if(!strcmp(name,"LeaveCriticalSection")){critical_leave(c->rcx);return 1;}
    if(!strcmp(name,"SetCriticalSectionSpinCount")) {
        pthread_mutex_lock(&threads_mutex);critical_state *cs=critical_lookup(c->rcx);if(!cs){pthread_mutex_unlock(&threads_mutex);stop_thread("critical spin on unknown lock",0);}
        c->rax=cs->spin;cs->spin=(uint32_t)c->rdx;uint64_t value=cs->spin;memcpy((unsigned char *)win64_pointer(cs->address,40,1)+32,&value,8);pthread_mutex_unlock(&threads_mutex);return 1;
    }
    if(!strcmp(name,"DeleteCriticalSection")) {
        pthread_mutex_lock(&threads_mutex);critical_state **p=&criticals;while(*p&&(*p)->address!=c->rcx)p=&(*p)->next;
        if(!*p||(*p)->active){pthread_mutex_unlock(&threads_mutex);stop_thread("delete missing/busy critical section",0);}
        critical_state *cs=*p;int status=pthread_mutex_destroy(&cs->mutex);if(status){pthread_mutex_unlock(&threads_mutex);stop_thread("critical mutex destroy",status);}
        *p=cs->next;critical_index_remove(cs);--critical_used;memset(win64_pointer(cs->address,40,1),0,40);free(cs);pthread_mutex_unlock(&threads_mutex);return 1;
    }
    return 0;
}

/* This fixture exercises the OS lifecycle through the public Win64 entrypoints.
 * Its callback is native test code, not a claim that an original game thread
 * entry or a DLL's attachment callbacks have executed. Production hooks are
 * restored before returning; the standalone self-test has no guest workers. */
static uint64_t test_api(const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e,uint64_t fifth,uint64_t sixth) {
    uint64_t stack[8]={0};stack[5]=fifth;stack[6]=sixth;
    CPU c={0};c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;c.rsp=(uintptr_t)stack;
    win64_region_add((uintptr_t)stack,sizeof(stack),1);
    int implemented=win64_threads_call(&c,name);
    if(!implemented)implemented=win64_kernel_call(&c,name);
    win64_region_remove((uintptr_t)stack);
    if(!implemented)stop_thread("self-test missing API",0);
    return c.rax;
}
#define TEST_API(name,a,b,d,e,f,g) test_api(name,(uint64_t)(a),(uint64_t)(b),(uint64_t)(d),(uint64_t)(e),(uint64_t)(f),(uint64_t)(g))
static void test_require(int ok,const char *detail){if(!ok)stop_thread(detail,win64_get_error());}
typedef struct {
    unsigned first,expansion;uint64_t critical;unsigned counter;
    atomic_uint attached,detached;
} thread_fixture;
static thread_fixture fixture;
static int test_attach(CPU *c,uint64_t low,size_t bytes) {
    test_require(bytes>=65536 && !((low+bytes)&15),"self-test stack geometry");
    c->rsp=(low+bytes-64)&~UINT64_C(15);c->mxcsr=0x1f80;
    atomic_fetch_add(&fixture.attached,1);return 1;
}
static void test_detach(CPU *c){(void)c;atomic_fetch_add(&fixture.detached,1);}
static uint64_t test_entry(CPU *c,uint64_t target,uint64_t parameter) {
    (void)c;test_require(target==UINT64_C(0xe5fe0100)&&parameter<4,"self-test callback parameters");
    test_require(TEST_API("SuspendThread",CURRENT_THREAD,0,0,0,0,0)==UINT32_MAX && win64_get_error()==50,"self-test running suspension fails honestly");
    uint64_t value=UINT64_C(0xcafe000000000000)|parameter;
    test_require(TEST_API("TlsSetValue",fixture.first,value,0,0,0,0)==1,"self-test TLS first set");
    test_require(TEST_API("TlsSetValue",fixture.expansion,value^0x55,0,0,0,0)==1,"self-test TLS expansion set");
    for(unsigned iteration=0;iteration<256;++iteration) {
        win64_set_error(4711);
        test_require(TEST_API("TlsGetValue",fixture.first,0,0,0,0,0)==value && !win64_get_error(),"self-test isolated TLS and LastError");
        test_require(frostpunk_teb_read(0x1480+fixture.first*8,8)==value,"self-test GS TLS slot");
        uint64_t expansion=frostpunk_teb_read(0x1780,8),actual;
        memcpy(&actual,win64_pointer(expansion+(fixture.expansion-64)*8,8,0),8);
        test_require(actual==(value^0x55),"self-test GS TLS expansion");
        uint64_t allocation=win64_alloc(64,1);test_require(allocation!=0,"self-test parallel allocation");
        uint64_t expected=value^iteration;
        memcpy(win64_pointer(allocation+16,8,1),&expected,8);
        memcpy(&actual,win64_pointer(allocation+16,8,0),8);
        test_require(actual==expected,"self-test parallel registered heap read/write");win64_free(allocation);
        uint64_t local[4]={value,iteration,0,0};win64_region_add((uintptr_t)local,sizeof(local),1);
        memcpy(win64_pointer((uintptr_t)&local[2],8,1),&expected,8);
        test_require(local[2]==expected,"self-test parallel region registration");win64_region_remove((uintptr_t)local);
        TEST_API("EnterCriticalSection",fixture.critical,0,0,0,0,0);
        test_require(TEST_API("TryEnterCriticalSection",fixture.critical,0,0,0,0,0)==1,"self-test recursive critical section");
        ++fixture.counter;
        TEST_API("LeaveCriticalSection",fixture.critical,0,0,0,0,0);
        TEST_API("LeaveCriticalSection",fixture.critical,0,0,0,0,0);
    }
    uint32_t result=0x5100u+(uint32_t)parameter;
    if(parameter==3){CPU exit_cpu={0};exit_cpu.rcx=result;win64_threads_call(&exit_cpu,"ExitThread");}
    return result;
}
void win64_threads_self_test(void) {
    uint32_t saved_error=win64_get_error();
    test_require(configured&&!process_ready,"self-test must precede guest process readiness");
    unsigned allocated[65];
    for(unsigned i=0;i<65;++i){allocated[i]=(unsigned)TEST_API("TlsAlloc",0,0,0,0,0,0);test_require(allocated[i]!=UINT32_MAX,"self-test TLS allocation");}
    unsigned char critical[40]={0};win64_region_add((uintptr_t)critical,sizeof(critical),1);
    memset(&fixture,0,sizeof(fixture));fixture.first=allocated[0];fixture.expansion=allocated[64];fixture.critical=(uintptr_t)critical;
    test_require(fixture.first<64&&fixture.expansion>=64,"self-test TLS expansion index");
    test_require(TEST_API("InitializeCriticalSectionAndSpinCount",fixture.critical,4000,0,0,0,0)==1,"self-test critical initialization");
    win64_thread_hooks saved_host=host;host.attach=test_attach;host.detach=test_detach;host.call=test_entry;
    uint64_t ids[4]={0};win64_region_add((uintptr_t)ids,sizeof(ids),1);
    for(unsigned i=0;i<4;++i){ids[i]=TEST_API("CreateThread",0,0,UINT64_C(0xe5fe0100),i,4,0);test_require(ids[i]!=0,"self-test CreateThread suspended");}
    test_require(TEST_API("SuspendThread",ids[0],0,0,0,0,0)==1,"self-test nested suspended count");
    test_require(TEST_API("ResumeThread",ids[0],0,0,0,0,0)==2,"self-test nested resume count");
    for(unsigned i=0;i<4;++i){test_require(TEST_API("ResumeThread",ids[i],0,0,0,0,0)==1,"self-test resume previous count");test_require(TEST_API("WaitForSingleObject",ids[i],0,0,0,0,0)==258,"self-test process initialization gate");}
    test_require(!atomic_load(&fixture.attached),"self-test callback ran before process readiness");
    win64_threads_process_ready();
    test_require(TEST_API("WaitForMultipleObjects",4,(uintptr_t)ids,1,10000,0,0)==0,"self-test thread wait-all");
    test_require(fixture.counter==1024&&atomic_load(&fixture.attached)==4&&atomic_load(&fixture.detached)==4,"self-test parallel counters and cleanup");
    for(unsigned i=0;i<4;++i){
        uint32_t code=0;win64_region_add((uintptr_t)&code,4,1);
        test_require(TEST_API("GetExitCodeThread",ids[i],(uintptr_t)&code,0,0,0,0)==1&&code==0x5100u+i,"self-test thread exit status");
        win64_region_remove((uintptr_t)&code);
        test_require(TEST_API("WaitForSingleObject",ids[i],0,0,0,0,0)==0,"self-test terminated thread stays signaled");
        test_require(!TEST_API("SetThreadPriority",ids[i],0,0,0,0,0)&&win64_get_error()==6,"self-test completed priority cannot reuse TID");
        test_require(!TEST_API("SetThreadAffinityMask",ids[i],1,0,0,0,0)&&win64_get_error()==6,"self-test completed affinity cannot reuse TID");
        test_require(TEST_API("ResumeThread",ids[i],0,0,0,0,0)==UINT32_MAX&&win64_get_error()==6,"self-test completed thread cannot resume");
        test_require(TEST_API("CloseHandle",ids[i],0,0,0,0,0)==1,"self-test thread close");
    }
    pthread_mutex_lock(&threads_mutex);host=saved_host;process_ready=0;pthread_mutex_unlock(&threads_mutex);
    win64_region_remove((uintptr_t)ids);
    TEST_API("DeleteCriticalSection",fixture.critical,0,0,0,0,0);win64_region_remove((uintptr_t)critical);
    for(unsigned i=0;i<65;++i)test_require(TEST_API("TlsFree",allocated[i],0,0,0,0,0)==1,"self-test TLS free");
    win64_set_error(saved_error);
    fprintf(stderr,"WIN64_THREADS_SELF_TEST passed workers=4 lifecycle_hook_fixture=1 process_gate=1 suspended_latch=1 explicit_exit=1 persistent_wait=1 TLS_first_and_expansion=1 heap_region_stress=1024 critical_recursive=1024 running_suspend_error=50 completed_thread_TID_liveness_gate=12\n");
}
