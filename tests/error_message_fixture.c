/* Real registry, allocator, LastError/TEB and argument decoder from production. */
#include "../android/host/win64.c"
#include <stdatomic.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
static _Thread_local int fail_record,fail_buffer;
static atomic_uint record_count;
static void *error_test_calloc(size_t n,size_t bytes){if(fail_record)return NULL;void *p=calloc(n,bytes);if(p)atomic_fetch_add(&record_count,1);return p;}
static void error_test_free(void *p){if(p)atomic_fetch_sub(&record_count,1);free(p);}
static uint64_t error_test_alloc(size_t bytes,int zero){return fail_buffer?0:win64_alloc(bytes,zero);}
#define calloc error_test_calloc
#define free error_test_free
#define win64_alloc error_test_alloc
#define allocations error_allocations
#include "../android/host/win64_error_message.c"
#undef allocations
#undef win64_alloc
#undef free
#undef calloc
typedef struct {uint32_t flags,identifier,language,minimum,null_output,result,error;char text[32];} oracle_record;
_Static_assert(sizeof(oracle_record)==60,"oracle ABI");
typedef struct {uint64_t stack[8],output[3];} workspace;
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ERROR_MESSAGE_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(workspace *w,const char *name,uint64_t flags,uint64_t source,uint64_t identifier,uint64_t language,uint64_t destination,uint64_t minimum,uint64_t arguments){CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=flags;cpu.rdx=source;cpu.r8=identifier;cpu.r9=language;cpu.rsp=(uintptr_t)w->stack;
    w->stack[5]=destination;w->stack[6]=minimum;w->stack[7]=arguments;before=cpu;if(!win64_error_message_call(&cpu,name))abort();uint64_t result=cpu.rax;cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return result;}
static uint64_t format(workspace *w,uint32_t language,uint32_t minimum){return invoke(w,"FormatMessageA",0x1300,0,50,language,(uintptr_t)&w->output[1],minimum,0);}
static void *worker(void *unused){(void)unused;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
    for(unsigned i=0;i<2000;++i){win64_set_error(1927);if(format(&w,0x400,i%4097)!=31||win64_get_error()!=1927||memcmp(win64_pointer(w.output[1],32,0),unsupported_request,32)||invoke(&w,"LocalFree",w.output[1],0,0,0,0,0,0)||win64_get_error()!=1927)abort();}
    win64_region_remove((uintptr_t)&w);return NULL;}
int main(int argc,char **argv){win64_error_message_self_test();CHECK(!atomic_load(&record_count));workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
    CHECK(argc==2);
    if(argv[1][0]=='-'){
        uint64_t flags=0x1300,source=0,language=0x400,minimum=0,arguments=0,destination=(uintptr_t)&w.output[1];
        if(!strcmp(argv[1],"--flags"))flags=0x1200;else if(!strcmp(argv[1],"--source"))source=1;else if(!strcmp(argv[1],"--inserts"))arguments=1;else if(!strcmp(argv[1],"--language"))language=0x419;else if(!strcmp(argv[1],"--minimum"))minimum=4097;else if(!strcmp(argv[1],"--invalid-output"))destination=1;else return 3;
        invoke(&w,"FormatMessageA",flags,source,50,language,destination,minimum,arguments);return 99;
    }
    FILE *file=fopen(argv[1],"rb");CHECK(file);uint32_t header[2];CHECK(fread(header,sizeof(header),1,file)==1&&header[0]==0x47534d45);oracle_record *data=malloc((size_t)header[1]*sizeof(*data));CHECK(data);CHECK(fread(data,sizeof(*data),header[1],file)==header[1]);CHECK(fgetc(file)==EOF);fclose(file);
    for(unsigned i=0;i<header[1];++i){oracle_record *r=&data[i];w.output[0]=UINT64_C(0x1122334455667788);w.output[1]=UINT64_C(0xa5a5a5a5a5a5a5a5);w.output[2]=UINT64_C(0x8877665544332211);win64_set_error(1927);crt_errno=1234;
        uint64_t result=invoke(&w,"FormatMessageA",r->flags,0,r->identifier,r->language,r->null_output?0:(uintptr_t)&w.output[1],r->minimum,0);
        if(result!=r->result||win64_get_error()!=r->error){fprintf(stderr,"oracle case%u result=%llu/%u error=%u/%u\n",i,(unsigned long long)result,r->result,win64_get_error(),r->error);return 2;}
        CHECK(w.output[0]==UINT64_C(0x1122334455667788)&&w.output[2]==UINT64_C(0x8877665544332211)&&crt_errno==1234);
        if(result){uint64_t address=w.output[1];CHECK(!memcmp(win64_pointer(address,32,0),r->text,32));CHECK(accessible(address,32,1)&&!accessible(address,33,0));
            CHECK(!invoke(&w,"LocalFree",address,0,0,0,0,0,0)&&win64_get_error()==r->error);CHECK(!accessible(address,1,0));
            CHECK(invoke(&w,"LocalFree",address,0,0,0,0,0,0)==address&&win64_get_error()==6);
        }else CHECK(w.output[1]==(r->null_output?UINT64_C(0xa5a5a5a5a5a5a5a5):0));
    }
    win64_set_error(1927);CHECK(!invoke(&w,"LocalFree",0,0,0,0,0,0,0)&&win64_get_error()==1927);
    uint64_t foreign=win64_alloc(16,0);CHECK(foreign);CHECK(invoke(&w,"LocalFree",foreign,0,0,0,0,0,0)==foreign&&win64_get_error()==6&&accessible(foreign,16,1));win64_free(foreign);
    fail_record=1;CHECK(!format(&w,0x400,0)&&!w.output[1]&&win64_get_error()==8&&!atomic_load(&record_count));fail_record=0;
    fail_buffer=1;CHECK(!format(&w,0x400,0)&&!w.output[1]&&win64_get_error()==8&&!atomic_load(&record_count));fail_buffer=0;
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));before=cpu;CHECK(!win64_error_message_call(&cpu,"FormatMessageUnexpected")&&!memcmp(&cpu,&before,sizeof(cpu)));
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,worker,NULL));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));CHECK(!atomic_load(&record_count)&&!error_allocations);
    printf("{\"success\":true,\"checks\":%u,\"native_cases\":%u,\"concurrent_owned_allocations\":8000,\"threads\":4,\"CPU_RSP_LastError_preserved\":true,\"output_canaries\":true,\"region_removed_after_free\":true,\"foreign_and_double_free_safe_rejected\":true,\"allocation_failures\":2}\n",checks,header[1]);return 0;
}
