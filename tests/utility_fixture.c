/* Exact expected values are recorded from native Windows ucrtbase.dll. */
#include "win64_utility.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif
static uint16_t *expected;static uint64_t seeds[8];static uint32_t sequence_count,length;
static void require(int condition,const char *reason){if(!condition){fprintf(stderr,"UTILITY_FIXTURE_FAIL %s\n",reason);exit(1);}}
uint64_t win64_argument(CPU *cpu,unsigned index){require(index==0,"one register argument only");return cpu->rcx;}
/* This older fixture exercises only rand/srand. The atoi unit uses real core. */
void *win64_pointer(uint64_t address,size_t bytes,int writable){(void)address;(void)bytes;(void)writable;abort();}
const char *win64_string_a(uint64_t address){(void)address;abort();}
void win64_stdio_set_errno(int value){(void)value;abort();}
/* This rand-only fixture must never enter the unrelated guest comparator. */
void dispatch(CPU *cpu,uint64_t target){(void)cpu;(void)target;abort();}
static uint64_t call(const char *name,uint64_t argument){
    CPU cpu={0};cpu.rsp=UINT64_C(0x12345678);cpu.rcx=argument;cpu.rax=UINT64_C(0xfedcba9876543210);cpu.rbx=UINT64_C(0xabcdef);cpu.cf=cpu.of=1;CPU before=cpu;
    require(win64_utility_call(&cpu,name),"known import handled");uint64_t result=cpu.rax;cpu.rax=before.rax;
    require(!memcmp(&cpu,&before,sizeof(cpu)),"CPU registers/flags/RSP preserved");return result;
}
static void compare(unsigned sequence,unsigned start,unsigned count){
    for(unsigned i=0;i<count;++i){uint64_t value=call("rand",0);if(value!=expected[(size_t)sequence*length+start+i]){fprintf(stderr,"UTILITY_VALUE_MISMATCH sequence=%u index=%u expected=%u actual=%llu\n",sequence,start+i,expected[(size_t)sequence*length+start+i],(unsigned long long)value);exit(1);}require(value<=32767,"RAND_MAX range");}
}
typedef struct {unsigned index;} worker_state;
#ifdef _WIN32
static DWORD WINAPI worker(void *argument){
#else
static void *worker(void *argument){
#endif
    worker_state *state=argument;compare(1,0,512);call("srand",seeds[4+state->index]);compare(4+state->index,0,512);return 0;
}
int main(int argc,char **argv){
    require(argc==2,"expected oracle fixture path");FILE *file=fopen(argv[1],"rb");require(file!=NULL,"open oracle fixture");char magic[8];
    require(fread(magic,1,8,file)==8 && !memcmp(magic,"FPRNG001",8) && fread(&sequence_count,4,1,file)==1 && fread(&length,4,1,file)==1 && sequence_count==8 && length==65536,"fixture magic/dimensions");
    expected=malloc((size_t)sequence_count*length*2);require(expected!=NULL,"expected allocation");
    for(unsigned i=0;i<sequence_count;++i)require(fread(&seeds[i],8,1,file)==1 && fread(expected+(size_t)i*length,2,length,file)==length,"read complete oracle sequence");
    require(fgetc(file)==EOF && !ferror(file) && !fclose(file),"fixture exact end");
    /* Native thread begins with seed1; self-test must preserve that state. */
    win64_utility_self_test();compare(1,0,64);
    for(unsigned sequence=0;sequence<sequence_count;++sequence){call("srand",seeds[sequence]);compare(sequence,0,length);call("srand",seeds[sequence]);compare(sequence,0,64);}
    call("srand",seeds[0]);compare(0,0,17);worker_state states[4];
#ifdef _WIN32
    HANDLE threads[4];for(unsigned i=0;i<4;++i){states[i].index=i;threads[i]=CreateThread(NULL,0,worker,&states[i],0,NULL);require(threads[i]!=NULL,"native thread creation");}
    require(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0,"join independent native threads");
    for(unsigned i=0;i<4;++i){DWORD status=1;require(GetExitCodeThread(threads[i],&status) && !status && CloseHandle(threads[i]),"native worker success");}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i){states[i].index=i;require(!pthread_create(&threads[i],NULL,worker,&states[i]),"native thread creation");}
    for(unsigned i=0;i<4;++i)require(!pthread_join(threads[i],NULL),"join independent native threads");
#endif
    compare(0,17,64);CPU unknown={0};unknown.rax=123;unknown.rsp=456;CPU before=unknown;require(!win64_utility_call(&unknown,"rand_s") && !memcmp(&unknown,&before,sizeof(unknown)),"unsupported import unhandled without CPU mutation");
    uint64_t values=(uint64_t)sequence_count*(length+64)+64+17+64+4*1024;
    printf("UTILITY_FIXTURE_PASS exact_values=%llu sequences=%u per_sequence=%u threads=4 default_seed=1 reseed=1 seed32_truncation=1 thread_isolation=1 RAND_MAX32767=1 no_rsp_pop=1 original_game_exec=0\n",(unsigned long long)values,sequence_count,length);
    free(expected);return 0;
}
