/* Production pointer registry/allocations/LastError, no guest CPU execution. */
#include "../android/host/win64.c"
#include "win64_encoding.h"
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
typedef struct{uint32_t page,flags;int32_t count,capacity;uint32_t options,result,error,bytes;unsigned char output[128];} oracle_record;
_Static_assert(sizeof(oracle_record)==160,"reverse UTF8 oracle ABI");
typedef struct{oracle_record record;const unsigned char *input;} test_case;
typedef struct{uint64_t stack[7];unsigned char source[65],output[129];} workspace;
static _Atomic unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"REVERSE_ENCODING_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(workspace *w,uint32_t page,uint32_t flags,uint64_t source,int32_t count,uint64_t output,int32_t capacity){
 CPU c;memset(&c,0xa5,sizeof(c));c.rcx=page;c.rdx=flags;c.r8=source;c.r9=(uint32_t)count;c.rsp=(uintptr_t)w->stack;w->stack[5]=output;w->stack[6]=(uint32_t)capacity;CPU saved=c;
 if(!win64_encoding_call(&c,"MultiByteToWideChar"))abort();uint64_t result=c.rax;c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))abort();return result;
}
static int compare(workspace *w,const test_case *test,unsigned number){
 const oracle_record *r=&test->record;unsigned inoffset=(r->options&32)?1:0,outoffset=(r->options&64)?1:0;memset(w->source,0xa5,sizeof(w->source));memcpy(w->source+inoffset,test->input,r->bytes);memset(w->output,0xa5,sizeof(w->output));
 uint64_t source=(r->options&1)?0:(uintptr_t)(w->source+inoffset),destination=(r->options&2)?0:(r->options&16)?source:(uintptr_t)(w->output+outoffset);
 win64_set_error(1927);crt_errno=1234;uint64_t result=invoke(w,r->page,r->flags,source,r->count,destination,r->capacity);
 if(result!=r->result||win64_get_error()!=r->error||crt_errno!=1234||memcmp(w->output+outoffset,r->output,128)||memcmp(w->source+inoffset,test->input,r->bytes)||w->source[inoffset?r->bytes+1:r->bytes]!=0xa5||w->output[outoffset?0:128]!=0xa5){
  fprintf(stderr,"REVERSE_MISMATCH case=%u page=%u flags=%x bytes=%u count=%d capacity=%d options=%x got=%llu/%u expected=%u/%u\n",number,r->page,r->flags,r->bytes,r->count,r->capacity,r->options,(unsigned long long)result,win64_get_error(),r->result,r->error);
  fprintf(stderr,"input:");for(unsigned i=0;i<r->bytes;++i)fprintf(stderr,"%02x",test->input[i]);fprintf(stderr,"\nactual:");for(unsigned i=0;i<16;++i)fprintf(stderr,"%02x",w->output[outoffset+i]);fprintf(stderr,"\nexpected:");for(unsigned i=0;i<16;++i)fprintf(stderr,"%02x",r->output[i]);fputc('\n',stderr);return 0;
 }return 1;
}
typedef struct{test_case *tests;uint32_t count;} context;
static void *reader(void *opaque){context *ctx=opaque;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);for(unsigned i=0;i<5000;++i){unsigned at=(i*104729u)%ctx->count;if(!compare(&w,&ctx->tests[at],at))abort();}win64_region_remove((uintptr_t)&w);return NULL;}
int main(int argc,char **argv){
 win64_encoding_self_test();CHECK(argc==2);workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
 if(argv[1][0]=='-'){
  static const unsigned char normal[]={65,66,0},unterminated[]={65};win64_region_add((uintptr_t)normal,sizeof(normal),0);win64_region_add((uintptr_t)unterminated,sizeof(unterminated),0);uint64_t source=(uintptr_t)normal,out=(uintptr_t)w.output;uint32_t page=65001;int32_t count=-1,capacity=32;
  if(!strcmp(argv[1],"--unsupported-page"))page=1251;else if(!strcmp(argv[1],"--invalid-input"))source=1;else if(!strcmp(argv[1],"--invalid-output"))out=1;else if(!strcmp(argv[1],"--unterminated"))source=(uintptr_t)unterminated;else if(!strcmp(argv[1],"--short-output")){static uint16_t small[1];win64_region_add((uintptr_t)small,2,1);out=(uintptr_t)small;capacity=2;}else return 3;
  invoke(&w,page,0,source,count,out,capacity);return 99;
 }
 FILE *f=fopen(argv[1],"rb");CHECK(f);CHECK(!fseek(f,0,SEEK_END));long length=ftell(f);CHECK(length>8&&length<134217728);rewind(f);unsigned char *pool=malloc((size_t)length);CHECK(pool&&fread(pool,(size_t)length,1,f)==1&&fgetc(f)==EOF);fclose(f);
 uint32_t header[2];memcpy(header,pool,8);CHECK(header[0]==0x3257544d&&header[1]>1000);test_case *tests=calloc(header[1],sizeof(*tests));CHECK(tests);size_t offset=8;
 for(unsigned i=0;i<header[1];++i){CHECK(offset<=(size_t)length&&sizeof(oracle_record)<=(size_t)length-offset);memcpy(&tests[i].record,pool+offset,sizeof(oracle_record));offset+=sizeof(oracle_record);CHECK(tests[i].record.bytes>0&&tests[i].record.bytes<=64&&tests[i].record.bytes<=(size_t)length-offset);tests[i].input=pool+offset;offset+=tests[i].record.bytes;}
 CHECK(offset==(size_t)length);for(unsigned i=0;i<header[1];++i)CHECK(compare(&w,&tests[i],i));
 pthread_t threads[4];context ctx={tests,header[1]};for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,reader,&ctx));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
 printf("{\"success\":true,\"checks\":%u,\"native_cases\":%u,\"concurrent_calls\":20000,\"threads\":4,\"CPU_RSP_LastError\":true,\"source_and_output_canaries\":true,\"UTF16_unit_width\":2,\"guest_ACP_OEM_UTF8_policy\":true,\"no_original_execution\":true}\n",checks,header[1]);return 0;
}
