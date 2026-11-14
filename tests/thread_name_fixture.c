/* Production pointer registry, thread lifecycle, native labels and current TEB. */
#include "../android/host/win64.c"
#include "../android/host/win64_threads.c"
static _Thread_local unsigned char name_teb[0x2000];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(name_teb)||bytes>sizeof(name_teb)-offset||bytes>8)abort();memcpy(&value,name_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(name_teb)||bytes>sizeof(name_teb)-offset||bytes>8)abort();memcpy(name_teb+offset,&value,bytes);}
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"THREAD_NAME_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static const char *const labels[]={"Worker","JobSystemWorker000","12345678901\xf0\x9f\x8c\xa8","12345678901234\xc3\xa9","","Audio: \xf0\x9f\x8c\xa8 snow"};
static const char *const native_labels[]={"Worker","JobSystemWorker","12345678901\xf0\x9f\x8c\xa8","12345678901234","","Audio: \xf0\x9f\x8c\xa8 sno"};
static atomic_uint name_attached,name_detached,name_ran;
static int name_attach(CPU *cpu,uint64_t low,size_t bytes){cpu->rsp=(low+bytes-64)&~UINT64_C(15);atomic_fetch_add(&name_attached,1);return 1;}
static void name_detach(CPU *cpu){(void)cpu;atomic_fetch_add(&name_detached,1);}
static void *name_current_teb(void){return name_teb;}
static uint64_t name_entry(CPU *cpu,uint64_t target,uint64_t parameter){(void)cpu;if(target!=0xe5fe0200||parameter>=6)abort();char name[16];if(pthread_getname_np(pthread_self(),name,sizeof(name))||strcmp(name,native_labels[parameter])||strcmp(current->guest_name,labels[parameter]))abort();atomic_fetch_add(&name_ran,1);return 0x6200+parameter;}
typedef struct {uint64_t stack[2],arguments[6];} notification;
static int notify(notification *n,uint64_t caller,uint32_t count,uint32_t tid,const char *name){
    for(unsigned i=0;i<6;++i)n->arguments[i]=UINT64_C(0xa5a5000000000000)+i;uint32_t type=0x1000,flags=0;memcpy(n->arguments,&type,4);uint64_t address=(uintptr_t)name;memcpy((unsigned char*)n->arguments+8,&address,8);memcpy((unsigned char*)n->arguments+16,&tid,4);memcpy((unsigned char*)n->arguments+20,&flags,4);n->stack[0]=caller;
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=0x406d1388;cpu.rdx=0;cpu.r8=count;cpu.r9=(uintptr_t)n->arguments;cpu.rsp=(uintptr_t)n->stack;before=cpu;win64_set_error(1927);int saved_errno=errno=1234;int handled=win64_threads_raise_exception(&cpu);if(memcmp(&cpu,&before,sizeof(cpu))||win64_get_error()!=1927||errno!=saved_errno)abort();return handled;
}
static void *rename_sender(void *opaque){uint32_t target=(uint32_t)(uintptr_t)opaque;notification n;win64_region_add((uintptr_t)&n,sizeof(n),1);for(unsigned i=0;i<1000;++i)if(!notify(&n,0x140e2793b,6,target,labels[i%6]))abort();win64_region_remove((uintptr_t)&n);return NULL;}
int main(int argc,char **argv){win64_thread_hooks hooks={name_attach,name_detach,name_entry,name_current_teb};win64_threads_configure(&hooks,65536);win64_threads_self_test();notification n;win64_region_add((uintptr_t)&n,sizeof(n),1);for(unsigned i=0;i<6;++i)win64_region_add((uintptr_t)labels[i],strlen(labels[i])+1,0);
    if(argc==2){uint32_t tid=(uint32_t)syscall(SYS_gettid);static const char invalid_utf8[]="\xc0\xaf";static char too_long[257];memset(too_long,'a',256);win64_region_add((uintptr_t)invalid_utf8,sizeof(invalid_utf8),0);win64_region_add((uintptr_t)too_long,sizeof(too_long),0);
        if(!strcmp(argv[1],"--dead-tid")){win64_threads_process_ready();win64_region_add((uintptr_t)&tid,4,1);uint64_t id=TEST_API("CreateThread",0,0,0xe5fe0200,0,4,(uintptr_t)&tid);if(!id||!notify(&n,0x140e2793b,6,tid,labels[0])||TEST_API("ResumeThread",id,0,0,0,0,0)!=1||TEST_API("WaitForSingleObject",id,10000,0,0,0,0)||!TEST_API("CloseHandle",id,0,0,0,0,0))return 5;notify(&n,0x140e2793b,6,tid,labels[0]);return 99;}
        if(!strcmp(argv[1],"--unknown-tid")){notify(&n,0x140e2793b,6,0x7fffffff,labels[0]);return 99;}
        if(!strcmp(argv[1],"--invalid-utf8")){notify(&n,0x140e2793b,6,tid,invalid_utf8);return 99;}
        if(!strcmp(argv[1],"--too-long")){notify(&n,0x140e2793b,6,tid,too_long);return 99;}
        if(!notify(&n,0x140e2793b,6,tid,labels[0]))return 3;CPU c={0};c.rcx=0x406d1388;c.r8=6;c.r9=(uintptr_t)n.arguments;c.rsp=(uintptr_t)n.stack;
        if(!strcmp(argv[1],"--count"))c.r8=4;else if(!strcmp(argv[1],"--raise-flags"))c.rdx=1;else if(!strcmp(argv[1],"--type")){uint32_t type=0;memcpy(n.arguments,&type,4);}else if(!strcmp(argv[1],"--info-flags")){uint32_t flags=1;memcpy((unsigned char*)n.arguments+20,&flags,4);}else if(!strcmp(argv[1],"--invalid-record"))c.r9=1;else if(!strcmp(argv[1],"--short-six")){win64_region_remove((uintptr_t)&n);win64_region_add((uintptr_t)n.stack,sizeof(n.stack),1);win64_region_add((uintptr_t)n.arguments,24,1);}else return 4;
        win64_threads_raise_exception(&c);return 99;
    }
    CHECK(argc==1);win64_threads_process_ready();
    for(unsigned i=0;i<6;++i){uint32_t tid=0;win64_region_add((uintptr_t)&tid,4,1);uint64_t id=TEST_API("CreateThread",0,0,0xe5fe0200,i,4,(uintptr_t)&tid);CHECK(id&&tid);unsigned before=atomic_load(&name_ran);CHECK(notify(&n,0x140e2793b,6,tid,labels[i]));
        pthread_mutex_lock(&threads_mutex);thread_state *s=states;while(s&&(uint32_t)s->tid!=tid)s=s->next;char native[16];int valid=s&&s->published&&!s->started&&!s->finishing&&s->suspend_count==1&&!strcmp(s->guest_name,labels[i])&&!pthread_getname_np(s->native,native,sizeof(native))&&!strcmp(native,native_labels[i]);pthread_mutex_unlock(&threads_mutex);CHECK(valid&&atomic_load(&name_ran)==before);
        CHECK(TEST_API("ResumeThread",id,0,0,0,0,0)==1);CHECK(TEST_API("WaitForSingleObject",id,10000,0,0,0,0)==0);CHECK(TEST_API("CloseHandle",id,0,0,0,0,0)==1);win64_region_remove((uintptr_t)&tid);}
    CHECK(atomic_load(&name_ran)==6&&atomic_load(&name_attached)==6&&atomic_load(&name_detached)==6);
    for(unsigned i=0;i<6;++i){CHECK(notify(&n,0x141bd2a83,3,UINT32_MAX,labels[i]));CHECK(!strcmp(current->guest_name,labels[i]));char native[16];CHECK(!pthread_getname_np(pthread_self(),native,sizeof(native))&&!strcmp(native,native_labels[i]));}
    CHECK(notify(&n,0x141b1aa8c,6,UINT32_MAX,labels[0]));CHECK(!notify(&n,0x140000001,6,UINT32_MAX,labels[0]));
    char maximum[256],maximum_native[16];memset(maximum,'M',255);maximum[255]=0;win64_region_add((uintptr_t)maximum,sizeof(maximum),0);CHECK(notify(&n,0x141bd2a83,3,UINT32_MAX,maximum));CHECK(!memcmp(current->guest_name,maximum,256)&&!pthread_getname_np(pthread_self(),maximum_native,sizeof(maximum_native))&&!strcmp(maximum_native,"MMMMMMMMMMMMMMM"));
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=0xe06d7363;before=c;win64_set_error(1927);CHECK(!win64_threads_raise_exception(&c)&&!memcmp(&c,&before,sizeof(c))&&win64_get_error()==1927);
    pthread_t senders[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&senders[i],NULL,rename_sender,(void*)(uintptr_t)(uint32_t)current->tid));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(senders[i],NULL));
    char native[16];CHECK(!pthread_getname_np(pthread_self(),native,sizeof(native)));int matched=0;for(unsigned i=0;i<6;++i)if(!strcmp(current->guest_name,labels[i])&&!strcmp(native,native_labels[i]))matched=1;CHECK(matched);
    printf("{\"success\":true,\"checks\":%u,\"original_threads_self_test\":true,\"suspended_name_then_resume\":6,\"current_thread_notifications\":8,\"maximum_guest_name_bytes\":255,\"concurrent_notifications\":4000,\"sender_threads\":4,\"native_pthread_labels_checked\":true,\"guest_full_names_retained\":true,\"UTF8_prefix_not_split\":true,\"CPU_RSP_RAX_LastError_errno_preserved\":true,\"unrelated_exceptions_unhandled\":true}\n",checks);return 0;
}
