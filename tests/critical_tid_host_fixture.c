/* Exact extracted production critical-section bodies, paired on native Win32
 * pthread equivalents. ARM full-provider lifecycle is checked separately. */
#include "cpu64.h"
#include "region_pthread_win32.h"
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
typedef int pid_t;
#define SYS_gettid 178
static _Thread_local unsigned tid_reads;
static long fixture_syscall(int number){if(number!=SYS_gettid)abort();++tid_reads;return GetCurrentThreadId();}
#define syscall fixture_syscall
static int pthread_mutex_trylock(pthread_mutex_t*m){return TryEnterCriticalSection(m)?0:EBUSY;}
typedef struct thread_state{pid_t tid;}thread_state;
typedef struct critical_state {
    uint64_t address;pthread_mutex_t mutex;unsigned active,recursion;pid_t owner;
    uint32_t spin;struct critical_state *next;
}critical_state;
static pthread_mutex_t threads_mutex;
static critical_state*criticals;
static _Thread_local thread_state*current;
static _Thread_local uint32_t guest_error;
static unsigned char guest[40];
static _Atomic(unsigned long) checks;
static unsigned long cases;
static void need(int ok,const char*s){unsigned long n=__c11_atomic_fetch_add(&checks,1,__ATOMIC_RELAXED)+1;if(!ok){fprintf(stderr,"CRITICAL_TID_HOST_FAIL %s checks=%lu\n",s,n);exit(1);}}
static _Noreturn void stop_thread(const char*s,int error){fprintf(stderr,"STOP Win64 thread %s error=%d\n",s,error);fflush(stderr);_Exit(73);}
static void*win64_pointer(uint64_t p,size_t n,int writing){(void)writing;if(p!=(uintptr_t)guest||n!=40)stop_thread("fixture pointer",0);return guest;}
static void win64_set_error(uint32_t value){guest_error=value;}
#include "critical-baseline.inc"
#include "critical-candidate.inc"
static int which;
static int enter(int try_only){return which?candidate_critical_enter((uintptr_t)guest,try_only):baseline_critical_enter((uintptr_t)guest,try_only);}
static void leave(void){if(which)candidate_critical_leave((uintptr_t)guest);else baseline_critical_leave((uintptr_t)guest);}
static unsigned owner(void){unsigned v;memcpy(&v,guest+16,4);return v;}
static unsigned recursion(void){unsigned v;memcpy(&v,guest+12,4);return v;}
static void api(CPU*c,int release,int try_only){if(release)leave();else c->rax=enter(try_only);}
static void cpu_call(int release,int try_only){CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)guest;c.mxcsr=0x9fc0;b=c;
    errno=2345;guest_error=1927;SetLastError(1739);unsigned csr=_mm_getcsr();api(&c,release,try_only);if(!release)b.rax=c.rax;
    need(!memcmp(&c,&b,sizeof(c)),"fullCPU/RSP");need(errno==2345&&guest_error==1927&&GetLastError()==1739&&_mm_getcsr()==csr,"native/guest error and FP");}
typedef struct{int adopted,try_busy;unsigned iterations;}worker_arg;
static unsigned shared_count;
static void*worker(void*opaque){worker_arg*a=opaque;thread_state state={(pid_t)GetCurrentThreadId()};need(!current,"foreign starts NULL");if(a->adopted)current=&state;
    if(a->try_busy){unsigned before=tid_reads;need(!enter(1),"foreign TryEnter busy");need(tid_reads==before,"failed TryEnter needs no TID");return NULL;}
    for(unsigned i=0;i<a->iterations;++i){need(enter(0)==1,"worker enter");need(owner()==GetCurrentThreadId(),"actual native owner");++shared_count;leave();}
    current=NULL;return NULL;}
int main(int argc,char**argv){need(!pthread_mutex_init(&threads_mutex,NULL),"threads mutex");which=argc>1&&strstr(argv[1],"baseline")?0:1;
    need(baseline_critical_initialize((uintptr_t)guest,4000)==1,"initialize");
    if(argc>1){if(strstr(argv[1],"unknown")){if(which)candidate_critical_enter(1,0);else baseline_critical_enter(1,0);}
        else if(strstr(argv[1],"unowned"))leave();
        else if(strstr(argv[1],"foreign")){enter(0);worker_arg a={0,0,1};pthread_t t;
            /* The foreign owner check is exercised by a dedicated entry below. */
            extern void*foreign_leave(void*);need(!pthread_create(&t,NULL,foreign_leave,&a),"foreign create");pthread_join(t,NULL);}
        else return 2;return 99;}
    unsigned syscall_total[2]={0};
    for(which=0;which<2;++which)for(int adopted=0;adopted<2;++adopted){thread_state state={(pid_t)GetCurrentThreadId()};current=adopted?&state:NULL;
        for(unsigned depth=1;depth<=5;++depth)for(unsigned iteration=0;iteration<64;++iteration){unsigned before=tid_reads;
            for(unsigned d=0;d<depth;++d){cpu_call(0,d&1);need(owner()==GetCurrentThreadId()&&recursion()==d+1,"recursive owner/depth");}
            for(unsigned d=depth;d;--d)cpu_call(1,0);
            need(!owner()&&!recursion()&&!criticals->active,"balanced state");
            unsigned expected=which&&adopted?0:depth*2;need(tid_reads-before==expected,"exact syscall count");syscall_total[which]+=tid_reads-before;++cases;}
        current=NULL;}
    for(which=0;which<2;++which){thread_state state={(pid_t)GetCurrentThreadId()};current=&state;need(enter(0)==1,"busy owner");
        worker_arg busy={0,1,0};pthread_t t;need(!pthread_create(&t,NULL,worker,&busy),"busy create");need(!pthread_join(t,NULL),"busy join");leave();current=NULL;
        shared_count=0;worker_arg args[4];pthread_t threads[4];for(unsigned i=0;i<4;++i){args[i]=(worker_arg){(int)(i&1),0,1024};need(!pthread_create(&threads[i],NULL,worker,&args[i]),"parallel create");}
        for(unsigned i=0;i<4;++i)need(!pthread_join(threads[i],NULL),"parallel join");need(shared_count==4096,"parallel total");}
    printf("{\"success\":true,\"paired_cases\":%lu,\"baseline_tid_reads\":%u,\"candidate_tid_reads\":%u,\"parallel_pairs\":8192,\"failed_try_preserves_tid\":true,\"fullCPU_FP_errno_LastError\":true}\n",cases,syscall_total[0],syscall_total[1]);return 0;}
void*foreign_leave(void*unused){(void)unused;need(!current,"foreign owner is not adopted");leave();return NULL;}
