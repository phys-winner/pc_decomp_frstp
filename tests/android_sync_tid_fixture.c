#include "../android/host/win64.c"
#include "../android/host/win64_sync.c"
#include <sys/wait.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(fixture_teb)||bytes>8)abort();memcpy(&v,fixture_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(fixture_teb)||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
void dispatch(CPU *cpu,uint64_t address){(void)cpu;(void)address;abort();}
static void *check_tid(void *ignored){(void)ignored;pid_t tid=(pid_t)syscall(SYS_gettid);for(unsigned i=0;i<65536;++i){errno=EDOM;if(current_tid()!=tid||errno!=EDOM)abort();}return (void *)(uintptr_t)tid;}
static uint64_t stamp(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static pid_t before_tid(void){return (pid_t)syscall(SYS_gettid);}
int main(void){
    check_tid(NULL);pthread_t threads[8];void *ids[8];
    for(unsigned i=0;i<8;++i)if(pthread_create(&threads[i],NULL,check_tid,NULL))abort();
    for(unsigned i=0;i<8;++i)if(pthread_join(threads[i],&ids[i]))abort();
    for(unsigned i=0;i<8;++i)for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])abort();
    pid_t parent=current_tid(),child=fork();if(child<0)abort();
    if(!child){check_tid(NULL);_exit(current_tid()==parent?1:0);}int status;if(waitpid(child,&status,0)!=child||!WIFEXITED(status)||WEXITSTATUS(status))abort();
    win64_sync_self_test();
    pid_t (*volatile functions[])(void)={before_tid,current_tid};uint64_t times[2];volatile uint64_t sink=0;
    for(unsigned k=0;k<2;++k){uint64_t start=stamp();for(unsigned i=0;i<1000000;++i)sink+=functions[k]();times[k]=stamp()-start;}
    printf("{\"success\":true,\"tid_checks\":655360,\"threads\":8,\"fork\":true,\"sync_self_test\":true,\"syscall_ns\":%llu,\"candidate_ns\":%llu,\"sink\":%llu}\n",(unsigned long long)times[0],(unsigned long long)times[1],(unsigned long long)sink);
}
