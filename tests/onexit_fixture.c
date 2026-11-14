/* Actual production helpers, isolated by section GC. No callbacks execute. */
#include <stdlib.h>
#include <stdint.h>
static int fail_next_malloc,fail_next_realloc;
static void *fixture_malloc(size_t bytes);
static void *fixture_realloc(void *pointer,size_t bytes);
#define malloc fixture_malloc
#define realloc fixture_realloc
#include "../android/host/win64.c"
#undef malloc
#undef realloc
static void *fixture_malloc(size_t bytes) { if(fail_next_malloc){fail_next_malloc=0;return NULL;}return malloc(bytes); }
static void *fixture_realloc(void *pointer,size_t bytes) { if(fail_next_realloc){fail_next_realloc=0;return NULL;}return realloc(pointer,bytes); }
static _Thread_local uint32_t fixture_last_error;
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes) { if(offset!=0x68||bytes!=4)abort();fixture_last_error=(uint32_t)value; }
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes) { if(offset!=0x68||bytes!=4)abort();return fixture_last_error; }
#define CHECK(condition) do { if(!(condition)){fprintf(stderr,"ONEXIT_FIXTURE_FAIL line=%d\n",__LINE__);return 2;} } while(0)
typedef struct {uint64_t table;unsigned thread,count;} worker_args;
static void *worker(void *opaque) {
    worker_args *a=opaque;
    for(unsigned i=0;i<a->count;++i){uint64_t tag=((uint64_t)(a->thread+1)<<32)|(i+1);
        if(crt_atexit_register(tag)||register_onexit_function(a->table,tag))abort();}
    return NULL;
}
static int ordered(const uint64_t *entries,size_t count,unsigned threads,unsigned each) {
    unsigned next[4]={0};
    if(threads>4||count!=threads*each)return 0;
    for(size_t i=0;i<count;++i){unsigned thread=(unsigned)(entries[i]>>32),sequence=(unsigned)entries[i];
        if(!thread||thread>threads||sequence!=++next[thread-1])return 0;}
    for(unsigned i=0;i<threads;++i)if(next[i]!=each)return 0;
    return 1;
}
int main(int argc,char **argv) {
    if(argc>1){uint64_t bad[3]={0};win64_region_add((uintptr_t)bad,sizeof(bad),1);
        if(!strcmp(argv[1],"--invalid-pointer"))initialize_onexit_table(1);
        else if(!strcmp(argv[1],"--inverted")){bad[0]=16;bad[1]=8;bad[2]=32;register_onexit_function((uintptr_t)bad,1);}
        else if(!strcmp(argv[1],"--unaligned")){bad[1]=1;bad[2]=8;register_onexit_function((uintptr_t)bad,1);}
        else return 3;return 99;}
    onexit_registration_self_test();
    fail_next_realloc=1;CHECK(crt_atexit_register(1)==-1&&!exit_functions&&!exit_count&&!exit_capacity);
    for(unsigned i=0;i<32;++i)CHECK(!crt_atexit_register(i+1));
    uint64_t *old=exit_functions;fail_next_realloc=1;CHECK(crt_atexit_register(33)==-1&&exit_functions==old&&exit_count==32&&exit_capacity==32);
    for(unsigned i=0;i<32;++i)CHECK(exit_functions[i]==i+1);
    CHECK(!crt_atexit_register(33)&&exit_count==33&&exit_capacity==64);
    free(exit_functions);exit_functions=NULL;exit_count=exit_capacity=0;
    uint64_t table[3]={0};win64_region_add((uintptr_t)table,sizeof(table),1);
    CHECK(!initialize_onexit_table((uintptr_t)table));
    fail_next_malloc=1;CHECK(register_onexit_function((uintptr_t)table,1)==-1&&!table[0]&&!table[1]&&!table[2]);
    for(unsigned i=0;i<32;++i)CHECK(!register_onexit_function((uintptr_t)table,i+1));
    uint64_t saved[3];memcpy(saved,table,sizeof(saved));fail_next_malloc=1;
    CHECK(register_onexit_function((uintptr_t)table,33)==-1&&!memcmp(saved,table,sizeof(saved)));
    for(unsigned i=0;i<32;++i)CHECK(rd64(table[0]+i*8)==i+1);
    CHECK(!register_onexit_function((uintptr_t)table,33)&&table[1]-table[0]==33*8&&table[2]-table[0]==64*8);
    win64_free(table[0]);memset(table,0,sizeof(table));
    pthread_t threads[4];worker_args args[4];
    for(unsigned i=0;i<4;++i){args[i]=(worker_args){(uintptr_t)table,i,2000};CHECK(!pthread_create(&threads[i],NULL,worker,&args[i]));}
    for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    CHECK(ordered(exit_functions,exit_count,4,2000));
    CHECK(ordered((const uint64_t *)(uintptr_t)table[0],(size_t)((table[1]-table[0])/8),4,2000));
    memcpy(saved,table,sizeof(saved));CHECK(!initialize_onexit_table((uintptr_t)table)&&!memcmp(saved,table,sizeof(saved)));
    win64_free(table[0]);win64_region_remove((uintptr_t)table);free(exit_functions);
    exit_functions=NULL;exit_count=exit_capacity=0;
    puts("{\"success\":true,\"threads\":4,\"registrations_per_api\":8000,\"total_concurrent_registrations\":16000,\"per_thread_order\":true,\"allocation_failure_injections\":4,\"game_boot_proven\":false}");return 0;
}
