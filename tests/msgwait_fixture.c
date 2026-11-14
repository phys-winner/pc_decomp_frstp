/* Real USER32 queue adapter and real kernel wait primitives; platform/input
 * scheduling has a separate deterministic host fixture. No original x86 runs. */
#define main window_regression_main
#include "window_fixture.c"
#undef main
#include "win64_kernel.h"
#include "msgwait_oracle.h"
const char *win64_string_a(uint64_t address) {
    for(size_t i=0;i<32768;++i)if(!*(const char*)win64_pointer(address+i,1,0))return(const char*)(uintptr_t)address;
    _Exit(73);
}
static uint64_t kernel_api(const char *name,unsigned count,...) {
    _Alignas(16) uint64_t stack[32]={0},args[8]={0};va_list ap;va_start(ap,count);for(unsigned i=0;i<count;++i)args[i]=va_arg(ap,uint64_t);va_end(ap);
    win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU c={0};c.rsp=(uintptr_t)&stack[1];c.rcx=args[0];c.rdx=args[1];c.r8=args[2];c.r9=args[3];
    for(unsigned i=4;i<count;++i)stack[6+i-4]=args[i];CPU saved=c;
    require(win64_kernel_call(&c,name),"kernel real handler");uint64_t result=c.rax;c.rax=saved.rax;require(!memcmp(&c,&saved,sizeof(c)),"kernel full CPU no-pop");win64_region_remove((uintptr_t)stack);return result;
}
static uint32_t msgwait(unsigned n,uint64_t *handles,unsigned all,unsigned ms,unsigned mask) {
    _Alignas(16) uint64_t stack[32]={0};win64_region_add((uintptr_t)stack,sizeof(stack),1);
    CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)&stack[1];c.rcx=n;c.rdx=(uintptr_t)handles;c.r8=all;c.r9=ms;stack[6]=mask;CPU saved=c;
    require(win64_window_call(&c,"MsgWaitForMultipleObjects"),"MsgWait real handler");uint32_t result=(uint32_t)c.rax;
    require(c.rax==result,"DWORD return upper bits zero");c.rax=saved.rax;require(!memcmp(&c,&saved,sizeof(c)),"MsgWait complete CPU/RSP/no-pop");win64_region_remove((uintptr_t)stack);return result;
}
static void queue_drain(message *m){while(peek(m,0,0,0,1)){} }
static uint32_t mytid(void){return(uint32_t)syscall(SYS_gettid);}
static double monotonic_time(clockid_t kind){struct timespec t;clock_gettime(kind,&t);return(double)t.tv_sec+t.tv_nsec*1e-9;}
static pthread_mutex_t exchange_mutex=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t exchange_changed=PTHREAD_COND_INITIALIZER;
static unsigned request,ack;static uint32_t destination;static int isolation;
static void *poster(void *unused){(void)unused;
    for(unsigned i=1;i<=512;++i){pthread_mutex_lock(&exchange_mutex);while(request<i)pthread_cond_wait(&exchange_changed,&exchange_mutex);pthread_mutex_unlock(&exchange_mutex);
        require(api("PostThreadMessageW",4,V(destination),V(0x8042),V(i),V(0)),"real concurrent PostThreadMessage");
        pthread_mutex_lock(&exchange_mutex);ack=i;pthread_cond_broadcast(&exchange_changed);pthread_mutex_unlock(&exchange_mutex);
    }return NULL;
}
static void *isolated_queue(void *unused){(void)unused;message m={0};win64_region_add((uintptr_t)&m,sizeof(m),1);peek(&m,0,0,0,1);
    require(api("PostThreadMessageW",4,V(mytid()),V(0x8055),V(0),V(0)),"isolated owner queue post");
    require(msgwait(0,NULL,0,0,0x5ff)==0,"isolated owner sees own queue");pthread_mutex_lock(&exchange_mutex);isolation=1;pthread_cond_broadcast(&exchange_changed);
    while(isolation==1)pthread_cond_wait(&exchange_changed,&exchange_mutex);pthread_mutex_unlock(&exchange_mutex);win64_region_remove((uintptr_t)&m);return NULL;
}
int main(void) {
    require(!window_regression_main(),"existing window regression");win64_kernel_self_test();
    message m={0};win64_region_add((uintptr_t)&m,sizeof(m),1);queue_drain(&m);
    for(unsigned i=0;i<sizeof(msgwait_cases)/sizeof(msgwait_cases[0]);++i){const struct msgwait_case *row=&msgwait_cases[i];uint64_t value=0;error=0x5a17;
        if(row->op==0){queue_drain(&m);continue;}
        if(row->op==1)value=api("PostThreadMessageW",4,V(mytid()),V(row->a),V(1),V(2));
        if(row->op==2)value=msgwait(0,NULL,0,0,row->a);
        if(row->op==3)value=peek(&m,0,row->a,row->b,row->c);
        if(row->op==4){api("PostQuitMessage",1,V(7));continue;}
        if(value!=row->result || error!=row->error){fprintf(stderr,"MSGWAIT_ORACLE_MISMATCH row=%u op=%u a=%x got=%llx/%u expected=%u/%u\n",i,row->op,row->a,(unsigned long long)value,error,row->result,row->error);return 1;}
        require(1,"native queue state/oracle exact result and LastError");
    }
    queue_drain(&m);double wall=monotonic_time(CLOCK_MONOTONIC),cpu=monotonic_time(CLOCK_THREAD_CPUTIME_ID);
    require(msgwait(0,NULL,0,30,0x5ff)==258,"finite empty wait");wall=monotonic_time(CLOCK_MONOTONIC)-wall;cpu=monotonic_time(CLOCK_THREAD_CPUTIME_ID)-cpu;
    require(wall>=.02&&cpu<.015,"blocking condition wait no sleep/poll spin");
    uint64_t handles[64];win64_region_add((uintptr_t)handles,sizeof(handles),1);
    handles[0]=kernel_api("CreateEventW",4,V(0),V(1),V(1),V(0));require(handles[0]!=0,"real manual event create");
    api("PostThreadMessageW",4,V(mytid()),V(0x8060),V(0),V(0));require(msgwait(1,handles,0,0,0x5ff)==1,"native already-new queue fast path before ready handle");
    peek(&m,0,0,0,0);require(msgwait(1,handles,0,0,0x5ff)==0,"ready handle wins after queue made old");
    kernel_api("ResetEvent",1,handles[0]);require(msgwait(1,handles,0,0,0x5ff)==258,"old but present queue is not ready");queue_drain(&m);kernel_api("CloseHandle",1,handles[0]);
    handles[0]=kernel_api("CreateEventW",4,V(0),V(0),V(1),V(0));api("PostThreadMessageW",4,V(mytid()),V(0x8061),V(0),V(0));
    require(msgwait(1,handles,0,0,0x5ff)==1&&kernel_api("WaitForSingleObject",2,handles[0],V(0))==0,"new queue fast path leaves auto-event signaled");
    require(kernel_api("WaitForSingleObject",2,handles[0],V(0))==258,"auto-reset still consumes exactly once");queue_drain(&m);kernel_api("CloseHandle",1,handles[0]);
    handles[0]=kernel_api("CreateSemaphoreW",4,V(0),V(1),V(2),V(0));api("PostThreadMessageW",4,V(mytid()),V(0x8062),V(0),V(0));
    require(msgwait(1,handles,0,0,0x5ff)==1&&kernel_api("WaitForSingleObject",2,handles[0],V(0))==0,"new queue fast path leaves semaphore count untouched");
    require(kernel_api("WaitForSingleObject",2,handles[0],V(0))==258,"real semaphore then consumed once");queue_drain(&m);kernel_api("CloseHandle",1,handles[0]);
    for(unsigned i=0;i<63;++i)handles[i]=kernel_api("CreateEventW",4,V(0),V(1),V(0),V(0));
    api("PostThreadMessageW",4,V(mytid()),V(0x8063),V(0),V(0));require(msgwait(63,handles,0,0,0x5ff)==63,"63 handles plus private queue object");queue_drain(&m);
    require(msgwait(64,handles,0,0,0x5ff)==UINT32_MAX&&error==87,"64 guest handles rejected");
    require(msgwait(0,NULL,1,0,0x5ff)==UINT32_MAX&&error==50,"WaitAll explicit unsupported policy");
    uint64_t old=handles[0];handles[0]=0x1234;require(msgwait(1,handles,0,0,0x5ff)==UINT32_MAX&&error==6,"invalid handle real error");handles[0]=old;
    api("PostThreadMessageW",4,V(mytid()),V(0x8064),V(0),V(0));handles[0]=0x1234;error=0x5a17;
    require(msgwait(1,handles,0,0,0x5ff)==1&&error==0x5a17,"native fresh queue precedes bad-handle validation");handles[0]=old;queue_drain(&m);
    old=handles[1];handles[1]=handles[0];require(msgwait(2,handles,0,0,0x5ff)==UINT32_MAX&&error==87,"duplicate object real validation");handles[1]=old;
    for(unsigned i=0;i<63;++i)if(handles[i])kernel_api("CloseHandle",1,handles[i]);
    pthread_t thread;destination=mytid();require(!pthread_create(&thread,NULL,poster,NULL),"concurrent poster create");
    for(unsigned i=1;i<=512;++i){pthread_mutex_lock(&exchange_mutex);request=i;pthread_cond_broadcast(&exchange_changed);pthread_mutex_unlock(&exchange_mutex);
        require(msgwait(0,NULL,0,1000,0x5ff)==0,"post before/during waiter registration not lost");require(peek(&m,0,0,0,1)&&m.message==0x8042&&m.wp==i,"post wake preserves payload");
        pthread_mutex_lock(&exchange_mutex);while(ack<i)pthread_cond_wait(&exchange_changed,&exchange_mutex);pthread_mutex_unlock(&exchange_mutex);
    }require(!pthread_join(thread,NULL),"poster joined");
    require(!pthread_create(&thread,NULL,isolated_queue,NULL),"isolated queue worker create");pthread_mutex_lock(&exchange_mutex);while(!isolation)pthread_cond_wait(&exchange_changed,&exchange_mutex);pthread_mutex_unlock(&exchange_mutex);
    require(msgwait(0,NULL,0,5,0x5ff)==258,"other thread new input cannot wake this queue");pthread_mutex_lock(&exchange_mutex);isolation=2;pthread_cond_broadcast(&exchange_changed);pthread_mutex_unlock(&exchange_mutex);require(!pthread_join(thread,NULL),"isolated queue cleanup");
    win64_region_remove((uintptr_t)handles);win64_region_remove((uintptr_t)&m);
    printf("MSGWAIT_FIXTURE_PASS checks=%u native_queue_cases=%zu real_kernel_wait=1 handle_priority=1 auto_semaphore_atomic=1 waitall_explicit50=1 concurrent_posts=512 per_thread_queue=1 no_rsp_pop=1 no_original_execution=1 wall_ms=%.3f thread_cpu_ms=%.3f\n",checks,sizeof(msgwait_cases)/sizeof(msgwait_cases[0]),wall*1000,cpu*1000);return 0;
}

