/* Actual production core pointer validation and per-thread errno. */
#include "../android/host/win64.c"
#include "win64_utility.h"
/* The atoi regression has no guest callbacks. */
void dispatch(CPU *cpu,uint64_t target){(void)cpu;(void)target;abort();}
typedef struct {uint32_t length,result;int32_t error;char *text;} record;
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ATOI_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(uint64_t argument){CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=argument;cpu.rsp=0x12345678;before=cpu;if(!win64_utility_call(&cpu,"atoi"))abort();uint64_t result=cpu.rax;cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return result;}
static void *reader(void *opaque){record *records=opaque;for(unsigned i=0;i<10000;++i){record *r=&records[i%128];crt_errno=1927;if(invoke((uintptr_t)r->text)!=r->result||crt_errno!=r->error)abort();}return NULL;}
int main(int argc,char **argv){win64_utility_self_test();CHECK(argc==2);
    if(!strcmp(argv[1],"--null")){invoke(0);return 99;}if(!strcmp(argv[1],"--invalid-pointer")){invoke(1);return 99;}
    if(!strcmp(argv[1],"--unterminated-digits")){static char digits[]={'1','2','3'};win64_region_add((uintptr_t)digits,3,0);invoke((uintptr_t)digits);return 99;}
    FILE *file=fopen(argv[1],"rb");CHECK(file);CHECK(!fseek(file,0,SEEK_END));long file_bytes=ftell(file);CHECK(file_bytes>8&&file_bytes<16777216);rewind(file);unsigned char *pool=malloc((size_t)file_bytes);CHECK(pool);CHECK(fread(pool,(size_t)file_bytes,1,file)==1);CHECK(fgetc(file)==EOF);fclose(file);
    win64_region_add((uintptr_t)pool,(size_t)file_bytes,0);uint32_t header[2];memcpy(header,pool,8);CHECK(header[0]==0x494f5441&&header[1]>128);size_t position=8;
    record *data=calloc(header[1],sizeof(*data));CHECK(data);
    for(unsigned i=0;i<header[1];++i){uint32_t words[3];CHECK(position<=(size_t)file_bytes&&sizeof(words)<=(size_t)file_bytes-position);memcpy(words,pool+position,sizeof(words));position+=sizeof(words);data[i].length=words[0];data[i].result=words[1];data[i].error=(int32_t)words[2];CHECK(data[i].length&&data[i].length<=131072&&data[i].length<=(size_t)file_bytes-position);data[i].text=(char*)pool+position;position+=data[i].length;}
    CHECK(position==(size_t)file_bytes);
    for(unsigned i=0;i<header[1];++i){crt_errno=1927;uint64_t result=invoke((uintptr_t)data[i].text);
        if(result!=data[i].result||crt_errno!=data[i].error){fprintf(stderr,"atoi case%u got=%llu/%d expected=%u/%d\n",i,(unsigned long long)result,crt_errno,data[i].result,data[i].error);return 2;}}
    static const char nonnul[]={'1','2','3','x'};win64_region_add((uintptr_t)nonnul,4,0);crt_errno=1927;CHECK(invoke((uintptr_t)nonnul)==123&&crt_errno==1927);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,reader,data));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    printf("{\"success\":true,\"checks\":%u,\"native_cases\":%u,\"concurrent_calls\":40000,\"threads\":4,\"CPU_RSP_preserved\":true,\"per_thread_errno\":true,\"first_nondigit_does_not_require_NUL\":true,\"Windows_long32_saturation\":true}\n",checks,header[1]);return 0;
}
