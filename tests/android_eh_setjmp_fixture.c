/* Real eh64.c walker over a synthetic, data-only PE. No original code runs. */
#include "cpu64.h"
#include "eh64.c"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#include <signal.h>
#define LOCAL _Thread_local
#endif

#ifndef FP_EH_CANDIDATE
#define FP_EH_CANDIDATE 0
#endif
static _Alignas(16) unsigned char image[0x3000];
typedef struct {
    CPU initial;
    uint64_t outer,inner,expected_frame,expected_handler,expected_resume;
    unsigned mode,mask_change,checks,funclets,caught,depth;
    unsigned char stack[16384];
} case_state;
static LOCAL case_state state;
static LOCAL unsigned callback_entered;
static void check(int value,const char *message) {
    ++state.checks;
    if(!value){fprintf(stderr,"EH_FIXTURE_FAIL candidate=%d mode=%u %s\n",FP_EH_CANDIDATE,state.mode,message);exit(1);}
}
int es3_cs_depth_get(void){return (int)state.depth;}
void es3_cs_depth_set(int value){check(value>=0&&(unsigned)value<=state.depth,"dispatch depth restore");state.depth=(unsigned)value;}
static uint64_t va(unsigned rva){return (uintptr_t)image+rva;}
static void environment(uint64_t out[2]) {
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]) {
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static void make_image(void) {
    memset(image,0,sizeof(image));wr32((uintptr_t)image+0x3c,0x80);
    wr32((uintptr_t)image+0x80+0x18+0x70+24,0x400);
    wr32((uintptr_t)image+0x80+0x18+0x70+28,48);
    const uint32_t rows[12]={0x1000,0x1100,0x600,0x1100,0x1200,0x680,
                            0x1200,0x1300,0x700,0x1300,0x1400,0x720};
    memcpy(image+0x400,rows,sizeof(rows));
    /* Outer/inner C++ FuncInfo; inner's second IP state has no handler. */
    for(unsigned i=0;i<2;++i){unsigned u=0x600+i*0x80,fi=0x800+i*0x40;
        image[u]=9;image[u+1]=i?4:0;image[u+2]=i?1:0;
        if(i){image[u+4]=4;image[u+5]=0xf2;}
        unsigned hd=u+4+(i?4:0);wr32((uintptr_t)image+hd,0x1600);wr32((uintptr_t)image+hd+4,fi);
        int32_t fields[8]={0x19930522,2,0,1,(int32_t)(0x900+i*0x20),i?2:1,(int32_t)(0x980+i*0x20),0};
        memcpy(image+fi,fields,sizeof(fields));
        int32_t tries[5]={0,0,0,1,(int32_t)(0xa00+i*0x20)};memcpy(image+0x900+i*0x20,tries,sizeof(tries));
        int32_t ips[4]={(int32_t)(0x1000+i*0x100),0,0x1150,1};memcpy(image+0x980+i*0x20,ips,i?16:8);
        int32_t handler[5]={0,0,0x40,(int32_t)(0x1400+i*0x100),0};memcpy(image+0xa00+i*0x20,handler,sizeof(handler));
    }
    image[0x700]=1; /* Unhandled leaf. */
    image[0x720]=1;image[0x721]=5;image[0x722]=2;
    image[0x724]=5;image[0x725]=0x32; /* sub rsp,32 */
    image[0x726]=1;image[0x727]=0x30; /* push rbx */
    es3_eh_image_base=(uintptr_t)image;eh_init();
    check(pdata==(rfunc_t*)(image+0x400)&&pdata_n==4,"real EH directory initialization");
}
static uint64_t object=UINT64_C(0x928a77151f8300c9);
void dispatch(CPU *c,uint64_t target) {
    ++state.depth;c->rip=target;
    check(target==state.expected_handler,"exact funclet target");
    CPU expected=state.initial;expected.rsp=((state.expected_frame-0x200)&~UINT64_C(15))-8;
    expected.rcx=0;expected.rdx=state.expected_frame;expected.rip=target;
    check(!memcmp(c,&expected,sizeof(expected)),"full CPU at funclet (including unwind RBX)");
    check(rd64(state.expected_frame+0x40)==object,"catch-object copy");
    check(rd64(c->rsp)==0,"funclet callback return sentinel");
    c->rax=state.expected_resume;c->rsp+=8;++state.funclets;--state.depth;
}
static void expected_catch(CPU *c,uint64_t frame,unsigned continuation) {
    CPU expected=state.initial;expected.rsp=frame;expected.rip=va(continuation);
    expected.rcx=0;expected.rdx=frame;expected.rax=va(continuation);
    check(!memcmp(c,&expected,sizeof(expected)),"full CPU after catch continuation");
    check(state.depth==(frame==state.inner?2u:1u),"exact catching-frame dispatch depth");++state.caught;
}
static void native_callback(CPU *c) {
    ++callback_entered;
#if !defined(_WIN32)
    if(state.mask_change){sigset_t add,previous;sigemptyset(&add);sigaddset(&add,SIGUSR1);
        check(!pthread_sigmask(SIG_BLOCK,&add,&previous),"native callback changes mask");
        if(state.mask_change==2)check(!pthread_sigmask(SIG_SETMASK,&previous,NULL),"scoped native callback restores mask before throw");}
#endif
    check(es3_eh_throw(c,va(0x1350),(uintptr_t)&object,va(0xb00))==0,"throw unexpectedly returned");
    check(0,"supported throw must find handler");
}
static void inner(CPU *c) {
    uint64_t _ind=0;ES3_EH_ENTER(va(0x1100));
    push64(c,va(state.mode==2?0x1151:0x1120));push64(c,c->rbx);c->rsp-=32;c->rbx=UINT64_C(0xdeadbeef01234567);c->rip=va(0x1350);
    state.expected_frame=state.mode==2?state.outer:state.inner;
    state.expected_handler=va(state.mode==2?0x1400:0x1500);
    state.expected_resume=va(state.mode==2?0x1050:0x1140);
    native_callback(c);check(0,"native callback throw returned");
_ljump:
    check(_ind==va(0x1140),"inner resume address");expected_catch(c,state.inner,0x1140);
    if(state.mode==3){
        state.expected_frame=state.outer;state.expected_handler=va(0x1400);state.expected_resume=va(0x1050);
        /* Preserve the funclet result as the input to the next guest throw. */
        state.initial.rax=c->rax;state.initial.rcx=c->rcx;state.initial.rdx=c->rdx;
        check(es3_eh_throw(c,va(0x1151),(uintptr_t)&object,va(0xb00))==0,"rethrow unexpectedly returned");check(0,"rethrow not caught");
    }
    ES3_EH_LEAVE();
}
static void outer(CPU *c) {
    uint64_t _ind=0;ES3_EH_ENTER(va(0x1000));
    if(!state.mode){ES3_EH_LEAVE();return;}
    push64(c,va(0x1020));c->rsp-=128;state.inner=c->rsp;
    ++state.depth;inner(c);--state.depth;
    check(state.mode==1,"only inner-only catch returns normally");
    ES3_EH_LEAVE();return;
_ljump:
    check(_ind==va(0x1050),"outer resume address");expected_catch(c,state.outer,0x1050);
    ES3_EH_LEAVE();
}
static void one(unsigned serial,unsigned mode,unsigned mask_change) {
    unsigned checks=state.checks,funclets=state.funclets,caught=state.caught;
    memset(&state,0,sizeof(state));state.checks=checks;state.funclets=funclets;state.caught=caught;
    memset(state.stack,0xcc,sizeof(state.stack));state.mode=mode;state.mask_change=mask_change;state.depth=1;
    CPU c;for(size_t i=0;i<sizeof(c);++i)((unsigned char*)&c)[i]=(unsigned char)(serial*37+i*13);
    c.mxcsr=0x1fa4|((serial&3u)<<13);state.outer=((uintptr_t)state.stack+8192)&~UINT64_C(15);c.rsp=state.outer;c.rip=va(0x1000);
    wr64(c.rsp,0);state.initial=c;
    uint64_t old[2],wanted[2],now[2];environment(old);
#if defined(__aarch64__)
    wanted[0]=(old[0]&~(UINT64_C(3)<<22))|((uint64_t)(serial&3u)<<22);wanted[1]=0x15;
#else
    wanted[0]=0x1f95u|((serial&3u)<<13)|64u|32768u;wanted[1]=0;
#endif
    set_environment(wanted);errno=1379;
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    outer(&c);environment(now);int saved_errno=errno;
#if defined(_WIN32)
    check(GetLastError()==0x5a17,"native LastError unchanged");
#endif
    check(!memcmp(now,wanted,sizeof(now)),"native FP unchanged");check(saved_errno==1379,"native errno unchanged");
    check(eh_top==NULL,"EH frame stack restored");check(state.depth==1,"outer dispatch depth restored");
    if(!mode)check(!memcmp(&c,&state.initial,sizeof(c)),"normal entry full CPU");
    set_environment(old);
}
static void unhandled(void) {
    CPU c;memset(&c,0xa5,sizeof(c));c.rsp=((uintptr_t)state.stack+4096)&~UINT64_C(15);wr64(c.rsp,0);
    CPU expected=c;expected.rsp+=8;
    check(!es3_eh_throw(&c,va(0x1250),(uintptr_t)&object,va(0xb00)),"unhandled throw remains unhandled");
    check(!memcmp(&c,&expected,sizeof(c)),"unhandled virtual leaf unwind CPU");
    check(!eh_top,"unhandled EH stack remains empty");
    c=expected;check(!es3_eh_throw(&c,0,(uintptr_t)&object,va(0xb00))&&!memcmp(&c,&expected,sizeof(c)),"zero-PC rejection CPU");
}
static LOCAL jmp_buf thread_exit;
static void exit_inside_eh(void) {
    uint64_t _ind=0;ES3_EH_ENTER(va(0x1000));(void)_ind;
#if !defined(_WIN32)
    sigset_t add;sigemptyset(&add);sigaddset(&add,SIGUSR1);check(!pthread_sigmask(SIG_BLOCK,&add,NULL),"exit callback mask");
#endif
    longjmp(thread_exit,1);
_ljump: check(0,"thread exit must not enter guest catch");
}
static void thread_boundary(void) {
#if !defined(_WIN32)
    sigset_t before,after;check(!pthread_sigmask(SIG_SETMASK,NULL,&before),"read thread mask before exit");
#endif
    /* Exact production thread-boundary API deliberately remains setjmp. */
    if(!setjmp(thread_exit))exit_inside_eh();
    es3_eh_thread_reset();check(!eh_top,"thread-exit EH reset");
#if !defined(_WIN32)
    check(!pthread_sigmask(SIG_SETMASK,NULL,&after),"read thread mask after exit");
    check(sigismember(&before,SIGUSR1)==sigismember(&after,SIGUSR1),"unchanged thread setjmp restores mask");
#endif
}
static unsigned mask_counterexample(void) {
#if defined(_WIN32)
    return 0;
#else
    sigset_t original,base,after;check(!pthread_sigmask(SIG_SETMASK,NULL,&original),"read original mask");
    base=original;sigdelset(&base,SIGUSR1);check(!pthread_sigmask(SIG_SETMASK,&base,NULL),"set counterexample mask");
    one(90,2,2);check(!pthread_sigmask(SIG_SETMASK,NULL,&after),"read scoped callback mask");
    check(!sigismember(&after,SIGUSR1),"restored callback mask is equivalent");
    one(91,2,1);check(!pthread_sigmask(SIG_SETMASK,NULL,&after),"read changed callback mask");
    int blocked=sigismember(&after,SIGUSR1);
    check(blocked==FP_EH_CANDIDATE,"documented native callback mask divergence");
    check(!pthread_sigmask(SIG_SETMASK,&original,NULL),"restore original mask");return 1;
#endif
}
typedef struct {unsigned checks,funclets,caught;} worker_result;
#if defined(_WIN32)
static DWORD WINAPI worker(void *opaque) {
#else
static void *worker(void *opaque) {
#endif
    for(unsigned i=0;i<256;++i)one(i,i%4,0);
    unhandled();thread_boundary();worker_result *out=opaque;*out=(worker_result){state.checks,state.funclets,state.caught};return 0;
}
static double clock_seconds(void) {
#if defined(_WIN32)
    LARGE_INTEGER value,frequency;QueryPerformanceCounter(&value);QueryPerformanceFrequency(&frequency);return(double)value.QuadPart/(double)frequency.QuadPart;
#else
    struct timespec value;clock_gettime(CLOCK_MONOTONIC,&value);return(double)value.tv_sec+value.tv_nsec*1e-9;
#endif
}
static volatile unsigned benchmark_sink;
static void empty_eh(CPU *c) {
    uint64_t _ind=0;ES3_EH_ENTER(va(0x1000));c->rax+=1;ES3_EH_LEAVE();return;
_ljump:benchmark_sink+=(unsigned)_ind;ES3_EH_LEAVE();
}
int main(int argc,char **argv) {
    make_image();
    if(argc==3&&!strcmp(argv[1],"--benchmark")){
        unsigned count=(unsigned)strtoul(argv[2],NULL,10);if(!count||count>20000000)return 2;
        CPU c={0};for(unsigned i=0;i<1000;++i)empty_eh(&c);
        double begin=clock_seconds();for(unsigned i=0;i<count;++i)empty_eh(&c);double elapsed=clock_seconds()-begin;
        check(c.rax==(uint64_t)count+1000&&!eh_top,"benchmark real EH entries balanced");
        printf("{\"success\":true,\"candidate\":%d,\"entries\":%u,\"seconds\":%.9f,\"ns_per_entry\":%.3f,\"device_speedup_proven\":false}\n",FP_EH_CANDIDATE,count,elapsed,elapsed*1e9/count);return 0;
    }
    if(argc!=1)return 2;
    for(unsigned i=0;i<2048;++i)one(i,i%4,0);
    unhandled();thread_boundary();unsigned counterexamples=mask_counterexample();
    worker_result result[4];
#if defined(_WIN32)
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,&result[i],0,NULL);check(threads[i]!=NULL,"create worker");}
    for(unsigned i=0;i<4;++i){check(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0,"join worker");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)check(!pthread_create(&threads[i],NULL,worker,&result[i]),"create worker");
    for(unsigned i=0;i<4;++i)check(!pthread_join(threads[i],NULL),"join worker");
#endif
    for(unsigned i=0;i<4;++i){state.checks+=result[i].checks;state.funclets+=result[i].funclets;state.caught+=result[i].caught;}
    printf("{\"success\":true,\"candidate\":%d,\"cases\":3072,\"checks\":%u,\"funclets\":%u,\"caught_continuations\":%u,\"parallel_cases\":1024,\"unhandled_cases\":5,\"thread_exit_boundaries\":5,\"mask_counterexamples\":%u,\"mask_changing_callback_equivalent\":false,\"generic_candidate_qualified\":false,\"full_CPU_native_FP_errno\":true,\"production_changed\":false}\n",
        FP_EH_CANDIDATE,state.checks,state.funclets,state.caught,counterexamples);return 0;
}
