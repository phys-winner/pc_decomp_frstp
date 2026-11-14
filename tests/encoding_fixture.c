/* Production Win64 pointer registry, arguments, LastError and ABI. */
#include "../android/host/win64.c"
#include "win64_encoding.h"
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
typedef struct {uint32_t page,flags;int32_t count,capacity;uint32_t options,result,error,used,units,reserved;unsigned char text[64];} oracle_record;
_Static_assert(sizeof(oracle_record)==104,"oracle record");
typedef struct {uint64_t stack[9];unsigned char source[129],text[64];uint32_t used[3];} workspace;
typedef struct {oracle_record header;const unsigned char *source;} test_case;
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ENCODING_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(workspace *w,uint64_t page,uint64_t flags,uint64_t source,uint64_t count,uint64_t output,uint64_t capacity,uint64_t replacement,uint64_t used){CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=page;cpu.rdx=flags;cpu.r8=source;cpu.r9=count;cpu.rsp=(uintptr_t)w->stack;w->stack[5]=output;w->stack[6]=capacity;w->stack[7]=replacement;w->stack[8]=used;before=cpu;
    if(!win64_encoding_call(&cpu,"WideCharToMultiByte"))abort();uint64_t result=cpu.rax;cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return result;}
static int compare(workspace *w,const test_case *test,unsigned index){const oracle_record *r=&test->header;unsigned offset=(r->options&32)?1:0;memset(w->source,0xa5,sizeof(w->source));memcpy(w->source+offset,test->source,(size_t)r->units*2);memset(w->text,0xa5,sizeof(w->text));w->used[0]=0x12345678;w->used[1]=0x11223344;w->used[2]=0x87654321;
    uint64_t source=(r->options&1)?0:(uintptr_t)(w->source+offset);uint64_t destination=(r->options&2)?0:(r->options&16)?source:(uintptr_t)w->text;
    win64_set_error(1927);crt_errno=1234;uint64_t result=invoke(w,r->page,r->flags,source,(uint32_t)r->count,destination,(uint32_t)r->capacity,(r->options&4)?(uintptr_t)"?":0,(r->options&8)?(uintptr_t)&w->used[1]:0);
    if(result!=r->result||win64_get_error()!=r->error||w->used[1]!=r->used||memcmp(w->text,r->text,64)||memcmp(w->source+offset,test->source,(size_t)r->units*2)||w->used[0]!=0x12345678||w->used[2]!=0x87654321||crt_errno!=1234){fprintf(stderr,"encoding case%u page%u flags%x count%d capacity%d options%x got=%llu/error%u/used%x expected=%u/%u/%x\n",index,r->page,r->flags,r->count,r->capacity,r->options,(unsigned long long)result,win64_get_error(),w->used[1],r->result,r->error,r->used);return 0;}return 1;}
static void *reader(void *opaque){test_case *tests=opaque;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);for(unsigned i=0;i<5000;++i)if(!compare(&w,&tests[i%128],i))abort();win64_region_remove((uintptr_t)&w);return NULL;}
int main(int argc,char **argv){win64_encoding_self_test();CHECK(argc==2);workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
    if(argv[1][0]=='-'){static const uint16_t normal[]={65,0},unterminated[]={65};win64_region_add((uintptr_t)normal,sizeof(normal),0);win64_region_add((uintptr_t)unterminated,sizeof(unterminated),0);uint64_t source=(uintptr_t)normal,destination=(uintptr_t)w.text,used=0,page=65001;int32_t count=-1;
        if(!strcmp(argv[1],"--unsupported-page"))page=1251;else if(!strcmp(argv[1],"--invalid-input"))source=1;else if(!strcmp(argv[1],"--invalid-output"))destination=1;else if(!strcmp(argv[1],"--invalid-used-default"))used=1;else if(!strcmp(argv[1],"--unterminated"))source=(uintptr_t)unterminated;else return 3;
        invoke(&w,page,0,source,(uint32_t)count,destination,64,0,used);return 99;}
    FILE *file=fopen(argv[1],"rb");CHECK(file);CHECK(!fseek(file,0,SEEK_END));long length=ftell(file);CHECK(length>8&&length<134217728);rewind(file);unsigned char *pool=malloc((size_t)length);CHECK(pool);CHECK(fread(pool,(size_t)length,1,file)==1);CHECK(fgetc(file)==EOF);fclose(file);uint32_t header[2];memcpy(header,pool,8);CHECK(header[0]==0x38435457&&header[1]>128);test_case *tests=calloc(header[1],sizeof(*tests));CHECK(tests);size_t position=8;
    for(unsigned i=0;i<header[1];++i){CHECK(position<=(size_t)length&&sizeof(oracle_record)<=(size_t)length-position);memcpy(&tests[i].header,pool+position,sizeof(oracle_record));position+=sizeof(oracle_record);CHECK(tests[i].header.units<=32&&tests[i].header.units*2<=(size_t)length-position);tests[i].source=pool+position;position+=(size_t)tests[i].header.units*2;}
    CHECK(position==(size_t)length);
    for(unsigned i=0;i<header[1];++i)CHECK(compare(&w,&tests[i],i));
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,reader,tests));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    printf("{\"success\":true,\"checks\":%u,\"native_cases\":%u,\"concurrent_calls\":20000,\"threads\":4,\"CPU_RSP_LastError\":true,\"input_and_output_canaries\":true,\"UTF16_unit_width\":2,\"guest_ACP_OEM_UTF8_policy\":true}\n",checks,header[1]);return 0;
}
