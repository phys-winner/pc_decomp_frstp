/* Real Bionic pthreads, full current production providers and old lifecycle
 * self-tests. The provider copy differs only at the two owner TID reads. */
#define main legacy_thread_name_main
#include "thread-provider-fixture.inc"
#undef main
static unsigned long tid_checks;
static void tid_need(int ok,const char*what){++tid_checks;if(!ok){fprintf(stderr,"CRITICAL_TID_FULL_FAIL %s check=%lu\n",what,tid_checks);exit(1);}}
static unsigned char tid_guest[40];
static atomic_uint tid_pairs,tid_native_starts,tid_native_detaches;
static uint64_t tid_control(void){uint64_t v;__asm__ volatile("mrs %0,fpcr":"=r"(v));return v;}
static uint64_t tid_status(void){uint64_t v;__asm__ volatile("mrs %0,fpsr":"=r"(v));return v;}
static void tid_api(const char*name){CPU c,b;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)tid_guest;c.mxcsr=0x9fc0;b=c;
    errno=2345;win64_set_error(1927);uint64_t fpcr=tid_control(),fpsr=tid_status();tid_need(win64_threads_call(&c,name),"actual provider route");
    if(strcmp(name,"LeaveCriticalSection"))b.rax=c.rax;tid_need(!memcmp(&c,&b,sizeof(c)),"fullCPU and no provider pop");
    tid_need(errno==2345&&win64_get_error()==1927&&tid_control()==fpcr&&tid_status()==fpsr,"nativeFP/errno/LastError");}
static void tid_pair(void){tid_api("EnterCriticalSection");tid_need(criticals->owner==(pid_t)syscall(SYS_gettid),"native owner identity");
    tid_api("TryEnterCriticalSection");tid_need(criticals->recursion==2,"recursive TryEnter");tid_api("LeaveCriticalSection");tid_api("LeaveCriticalSection");
    tid_need(!criticals->owner&&!criticals->recursion&&!criticals->active,"balanced critical state");atomic_fetch_add(&tid_pairs,1);}
static void*tid_foreign(void*adopt){tid_need(current==NULL,"foreign starts with NULL current");
    for(unsigned i=0;i<32;++i)tid_pair();tid_need(current==NULL,"fallback does not lazily adopt");
    if(adopt){thread_state*s=get_current();tid_need(s&&s==current&&s->tid==(pid_t)syscall(SYS_gettid),"adopted native TID");
        for(unsigned i=0;i<32;++i)tid_pair();finish_state(s);tid_need(!current,"finish clears TLS before potential state free");
        for(unsigned i=0;i<32;++i)tid_pair();tid_need(!current,"post-finish fallback stays NULL");}
    return NULL;}
static uint64_t tid_guest_entry(CPU*c,uint64_t target,uint64_t parameter){(void)c;(void)parameter;tid_need(target==0xe5fe0300,"guest entry target");
    tid_need(current&&current->started&&current->tid==(pid_t)syscall(SYS_gettid),"native_start fills TID before guest");atomic_fetch_add(&tid_native_starts,1);
    for(unsigned i=0;i<64;++i)tid_pair();return 0x6300;}
static void tid_guest_detach(CPU*c){(void)c;tid_need(current&&current->tid==(pid_t)syscall(SYS_gettid),"valid TID retained through detach");atomic_fetch_add(&tid_native_detaches,1);}
static void*tid_wrong_leave(void*p){(void)p;tid_need(!current,"foreign leave fallback");critical_leave((uintptr_t)tid_guest);return NULL;}
static uint64_t tid_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))abort();return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
typedef struct{uint64_t ns,checksum;unsigned iterations;}tid_bench;
static void*tid_bench_foreign(void*opaque){tid_bench*b=opaque;tid_need(!current,"benchmark foreign NULL");uint64_t start=tid_now();
    for(unsigned i=0;i<b->iterations;++i){if(!critical_enter((uintptr_t)tid_guest,0))abort();++b->checksum;critical_leave((uintptr_t)tid_guest);}b->ns=tid_now()-start;
    tid_need(!current,"benchmark fallback did not adopt");return NULL;}
#if defined(CRITICAL_TID_SHARED)
__attribute__((visibility("default"))) int critical_tid_entry(int argc,char**argv){(void)argc;(void)argv;
    win64_thread_hooks hooks={name_attach,name_detach,name_entry,name_current_teb};win64_threads_configure(&hooks,65536);
    win64_region_add((uintptr_t)tid_guest,sizeof(tid_guest),1);tid_need(critical_initialize((uintptr_t)tid_guest,0)==1,"bench critical initialize");
    thread_state*s=get_current();tid_need(s&&s->tid==(pid_t)syscall(SYS_gettid),"bench adoption");
    for(unsigned i=0;i<64;++i){critical_enter((uintptr_t)tid_guest,0);critical_leave((uintptr_t)tid_guest);}
    tid_bench adopted={.iterations=200000},foreign={.iterations=200000};uint64_t start=tid_now();
    for(unsigned i=0;i<adopted.iterations;++i){if(!critical_enter((uintptr_t)tid_guest,0))abort();++adopted.checksum;critical_leave((uintptr_t)tid_guest);}adopted.ns=tid_now()-start;
    pthread_t t;tid_need(!pthread_create(&t,NULL,tid_bench_foreign,&foreign)&&!pthread_join(t,NULL),"foreign benchmark");
    TEST_API("DeleteCriticalSection",(uintptr_t)tid_guest,0,0,0,0,0);win64_region_remove((uintptr_t)tid_guest);finish_state(s);tid_need(!current,"benchmark cleanup current");pthread_key_delete(current_key);
    printf("{\"success\":true,\"whole_enter_leave\":[{\"name\":\"adopted_current\",\"iterations\":%u,\"ns\":%"PRIu64",\"checksum\":%"PRIu64"},{\"name\":\"foreign_null_current\",\"iterations\":%u,\"ns\":%"PRIu64",\"checksum\":%"PRIu64"}],\"actual_full_provider\":true,\"game_fps_proven\":false}\n",adopted.iterations,adopted.ns,adopted.checksum,foreign.iterations,foreign.ns,foreign.checksum);return 0;}
#else
int main(int argc,char**argv){if(argc>1&&strncmp(argv[1],"--tid-",6))return legacy_thread_name_main(argc,argv);
    int legacy=legacy_thread_name_main(1,argv);if(legacy)return legacy;
    win64_region_add((uintptr_t)tid_guest,sizeof(tid_guest),1);tid_need(critical_initialize((uintptr_t)tid_guest,4000)==1,"new critical initialize");
    if(argc>1){if(!strcmp(argv[1],"--tid-unknown"))critical_enter(1,0);else if(!strcmp(argv[1],"--tid-unowned"))critical_leave((uintptr_t)tid_guest);
        else if(!strcmp(argv[1],"--tid-foreign")){critical_enter((uintptr_t)tid_guest,0);pthread_t t;pthread_create(&t,NULL,tid_wrong_leave,NULL);pthread_join(t,NULL);}
        else if(!strcmp(argv[1],"--tid-busy-delete")){critical_enter((uintptr_t)tid_guest,0);TEST_API("DeleteCriticalSection",(uintptr_t)tid_guest,0,0,0,0,0);}else return 2;return 99;}
    tid_need(current&&current->tid==(pid_t)syscall(SYS_gettid),"legacy adopted main retains native TID");for(unsigned i=0;i<64;++i)tid_pair();
    pthread_t native;tid_need(!pthread_create(&native,NULL,tid_foreign,(void*)1)&&!pthread_join(native,NULL),"foreign/adopt/finish lifecycle");
    win64_thread_hooks saved=host;host.call=tid_guest_entry;host.detach=tid_guest_detach;
    uint64_t id=TEST_API("CreateThread",0,0,0xe5fe0300,0,0,0);tid_need(id&&TEST_API("WaitForSingleObject",id,10000,0,0,0,0)==0,"new guest worker");
    tid_need(TEST_API("CloseHandle",id,0,0,0,0,0)==1,"close new guest");host=saved;
    tid_need(atomic_load(&tid_pairs)==224&&atomic_load(&tid_native_starts)==1&&atomic_load(&tid_native_detaches)==1,"lifecycle counts");
    TEST_API("DeleteCriticalSection",(uintptr_t)tid_guest,0,0,0,0,0);win64_region_remove((uintptr_t)tid_guest);
    printf("{\"success\":true,\"checks\":%lu,\"whole_recursive_pairs\":224,\"adopt_finish_fallback\":true,\"native_start_detach\":true,\"fullCPU_FP_errno_LastError\":true,\"original_thread_selftest_and_name_regression\":true}\n",tid_checks);return 0;}
#endif
