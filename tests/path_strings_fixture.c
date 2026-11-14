/* Actual production pointer registry/LastError plus the isolated path module. */
#define _GNU_SOURCE
#include "../android/host/win64.c"
#include "win64_path.h"
#include <stdatomic.h>
#include <sys/wait.h>
#include <unistd.h>

static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
static _Atomic unsigned long checks,calls;
static void require(int condition,const char *reason){if(!condition){fprintf(stderr,"PATH_FIXTURE_FAIL %s\n",reason);fflush(stderr);_Exit(1);}atomic_fetch_add(&checks,1);}
typedef struct {uint32_t options,result,error,units;const unsigned char *input,*output;} native_case;
typedef struct {uint64_t stack[12];unsigned char source[522],output[530];uint16_t relative[3];} workspace;
static native_case *cases;static unsigned case_count;static unsigned char *pool,*relative_results;static uint32_t relative_count,NULL_relative;

static uint64_t invoke(workspace *w,const char *name,uint64_t a,uint64_t b){
    CPU c;memset(&c,0xA5,sizeof(c));memset(w->stack,0xC7,sizeof(w->stack));c.rsp=(uintptr_t)&w->stack[1];c.rcx=a;c.rdx=b;CPU before=c;uint64_t saved[12];memcpy(saved,w->stack,sizeof(saved));crt_errno=1234;
    require(win64_path_call(&c,name),"real path handler");uint64_t result=c.rax;c.rax=before.rax;
    require(!memcmp(&c,&before,sizeof(c))&&!memcmp(saved,w->stack,sizeof(saved)),"full CPU/RSP/stack canaries unchanged");require(crt_errno==1234,"guest CRT errno unchanged");atomic_fetch_add(&calls,1);return result;
}
static void canonical_case(workspace *w,const native_case *test,unsigned index){
    unsigned source_offset=(test->options&8)?1:0,output_offset=(test->options&16)?1:0;
    memset(w->source,0xA5,sizeof(w->source));memcpy(w->source+source_offset,test->input,test->units*2);
    unsigned char source_before[sizeof(w->source)];memcpy(source_before,w->source,sizeof(source_before));memset(w->output,0xA5,sizeof(w->output));
    for(unsigned i=0;i<528;i+=2){w->output[output_offset+i]=0x5A;w->output[output_offset+i+1]=0xA5;}
    if(test->options&4)memcpy(w->output+output_offset,test->input,test->units*2);
    unsigned char output_before[sizeof(w->output)];memcpy(output_before,w->output,sizeof(output_before));
    uint64_t source=test->options&1?0:test->options&4?(uintptr_t)(w->output+output_offset):(uintptr_t)(w->source+source_offset);
    uint64_t output=test->options&2?0:(uintptr_t)(w->output+output_offset);
    win64_set_error(1927);uint64_t result=invoke(w,"PathCanonicalizeW",output,source);uint32_t error=win64_get_error();
    size_t active=0;
    if(!(test->options&2)){
        for(unsigned i=0;i<260;++i){uint16_t u;memcpy(&u,test->output+i*2,2);if(!u){active=(i+1)*2;break;}}
        require(active!=0,"native output has a bounded NUL terminator");
    }
    if(result!=test->result||error!=test->error||memcmp(w->output+output_offset,test->output,active)){
        fprintf(stderr,"PATH_CASE_MISMATCH case=%u units=%u flags=%u got=%llu/%u want=%u/%u\n",index,test->units,test->options,(unsigned long long)result,error,test->result,test->error);
        for(unsigned i=0;i<active/2;++i){uint16_t a,b;memcpy(&a,w->output+output_offset+i*2,2);memcpy(&b,test->output+i*2,2);if(a!=b){fprintf(stderr,"PATH_OUTPUT_DIFFERENCE unit=%u got=%04x want=%04x\n",i,a,b);break;}}
        _Exit(1);
    }
    require(1,"native result/LastError/active UTF16 output plus NUL equality");require(!memcmp(source_before,w->source,sizeof(source_before)),"raw UTF16 source and guard bytes immutable");
    require(!memcmp(w->output+output_offset+active,output_before+output_offset+active,sizeof(w->output)-output_offset-active),"explicit port policy leaves output tail untouched; native intermediate stale tail is unclaimed");
    for(unsigned i=0;i<output_offset;++i)require(w->output[i]==0xA5,"odd output prefix guard");for(unsigned i=output_offset+528;i<sizeof(w->output);++i)require(w->output[i]==0xA5,"output byte suffix guard");
}
static void minimum_relative(workspace *w){
    uint16_t first[1];win64_region_add((uintptr_t)first,sizeof(first),0);
    const uint16_t values[]={0,'\\',':',0xD800,0xFFFF};
    for(unsigned i=0;i<sizeof(values)/sizeof(values[0]);++i){first[0]=values[i];win64_set_error(1927);require(invoke(w,"PathIsRelativeW",(uintptr_t)first,0)==(values[i]!='\\')&&win64_get_error()==1927&&first[0]==values[i],"relative nonletter requires exactly one UTF16 unit");}
    win64_region_remove((uintptr_t)first);
    uint16_t pair[2]={'A',':'};win64_region_add((uintptr_t)pair,sizeof(pair),0);require(invoke(w,"PathIsRelativeW",(uintptr_t)pair,0)==0,"drive prefix does not read third unit/NUL");pair[1]='/';require(invoke(w,"PathIsRelativeW",(uintptr_t)pair,0)==1,"ordinary slash does not read third unit/NUL");win64_region_remove((uintptr_t)pair);
}
static void negative(const char *name){
    workspace w;memset(&w,0,sizeof(w));win64_region_add((uintptr_t)w.stack,sizeof(w.stack),1);
    uint16_t input[261]={'C',':','\\','a',0};unsigned char output[532];memset(output,0xA5,sizeof(output));size_t source_bytes=10,output_bytes=520;int writable=1;uint64_t src=(uintptr_t)input,dst=(uintptr_t)output;int relative=0;
    if(!strcmp(name,"short-output"))output_bytes=519;
    else if(!strcmp(name,"readonly-output"))writable=0;
    else if(!strcmp(name,"unterminated-input")){input[0]='a';input[1]='b';source_bytes=4;}
    else if(!strcmp(name,"bad-input"))src=1;
    else if(!strcmp(name,"bad-output"))dst=1;
    else if(!strcmp(name,"long-input")){for(unsigned i=0;i<260;++i)input[i]='a';input[260]=0;source_bytes=sizeof(input);}
    else if(!strcmp(name,"UNC")){input[0]='\\';input[1]='\\';input[2]='s';input[3]=0;source_bytes=8;}
    else if(!strcmp(name,"device-namespace")){input[0]='\\';input[1]='\\';input[2]='?';input[3]='\\';input[4]=0;}
    else if(!strcmp(name,"relative-short-letter")){input[0]='A';source_bytes=2;relative=1;}
    else if(!strcmp(name,"relative-bad-input")){src=1;relative=1;}
    else if(!strcmp(name,"overflow-output"))dst=UINT64_MAX-255;
    else if(!strcmp(name,"overflow-input"))src=UINT64_MAX;
    else if(!strcmp(name,"partial-overlap-forward")||!strcmp(name,"partial-overlap-backward")){
        unsigned char overlap[544];memset(overlap,0xA5,sizeof(overlap));win64_region_add((uintptr_t)overlap,sizeof(overlap),1);
        src=(uintptr_t)(overlap+(!strcmp(name,"partial-overlap-forward")?0:4));dst=(uintptr_t)(overlap+(!strcmp(name,"partial-overlap-forward")?4:0));memcpy((void *)(uintptr_t)src,input,10);
        invoke(&w,"PathCanonicalizeW",dst,src);_Exit(1);
    }else _Exit(1);
    win64_region_add((uintptr_t)input,source_bytes,0);win64_region_add((uintptr_t)output,output_bytes,writable);
    invoke(&w,relative?"PathIsRelativeW":"PathCanonicalizeW",relative?src:dst,relative?0:src);_Exit(1);
}
static void load(const char *path){
    FILE *f=fopen(path,"rb");require(f!=NULL,"open native vectors");require(!fseek(f,0,SEEK_END),"seek native vectors");long length=ftell(f);require(length>16&&length<16*1024*1024,"bounded native vectors");rewind(f);pool=malloc((size_t)length);require(pool!=NULL&&fread(pool,1,(size_t)length,f)==(size_t)length&&!fclose(f),"read native vectors");
    uint32_t magic;memcpy(&magic,pool,4);memcpy(&relative_count,pool+4,4);memcpy(&case_count,pool+8,4);memcpy(&NULL_relative,pool+12,4);require(magic==0x31545350&&relative_count==196608&&case_count==7500&&NULL_relative==1,"native corpus header");relative_results=pool+16;size_t offset=16+relative_count;cases=calloc(case_count,sizeof(*cases));require(cases!=NULL,"allocate vector index");
    for(unsigned i=0;i<case_count;++i){native_case *c=&cases[i];require(offset<=(size_t)length&&16<=(size_t)length-offset,"canonical header bounds");memcpy(c,pool+offset,16);offset+=16;require(c->units&&c->units<=260&&c->units*2+528<=(size_t)length-offset,"canonical data bounds");c->input=pool+offset;offset+=c->units*2;c->output=pool+offset;offset+=528;require(!c->input[c->units*2-1]&&!c->input[c->units*2-2],"native UTF16 terminator");}
    require(offset==(size_t)length,"exact native vector byte consumption");
}
static void *reader(void *opaque){unsigned id=(unsigned)(uintptr_t)opaque;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);for(unsigned i=0;i<512;++i)canonical_case(&w,&cases[(id*512+i)%case_count],i);win64_region_remove((uintptr_t)&w);return NULL;}
int main(int argc,char **argv){
    require(argc==2,"vector path required");load(argv[1]);win64_path_self_test();workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
    unsigned n=0;for(unsigned first=0;first<65536;++first)for(unsigned second_index=0;second_index<3;++second_index){static const uint16_t seconds[]={0,':','/'};w.relative[0]=(uint16_t)first;w.relative[1]=seconds[second_index];w.relative[2]=0;uint16_t saved[3];memcpy(saved,w.relative,sizeof(saved));win64_set_error(1927);
        require(invoke(&w,"PathIsRelativeW",(uintptr_t)w.relative,0)==relative_results[n++]&&win64_get_error()==1927&&!memcmp(saved,w.relative,sizeof(saved)),"native exhaustive relative result/LastError/input");}
    win64_set_error(1927);require(invoke(&w,"PathIsRelativeW",0,0)==NULL_relative&&win64_get_error()==1927,"native NULL relative");minimum_relative(&w);
    for(unsigned i=0;i<case_count;++i)canonical_case(&w,&cases[i],i);
    CPU c;memset(&c,0xA5,sizeof(c));CPU saved=c;require(!win64_path_call(&c,"UnknownPathFunction")&&!memcmp(&c,&saved,sizeof(c)),"unknown API unchanged CPU");
    const char *negative_cases[]={"short-output","readonly-output","unterminated-input","bad-input","bad-output","long-input","UNC","device-namespace","relative-short-letter","relative-bad-input","overflow-output","overflow-input","partial-overlap-forward","partial-overlap-backward"};
    for(unsigned i=0;i<sizeof(negative_cases)/sizeof(negative_cases[0]);++i){pid_t pid=fork();require(pid>=0,"negative fork");if(!pid)negative(negative_cases[i]);int status;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==73,"bounds/namespace/overlap explicit73");}
    pthread_t workers[4];for(unsigned i=0;i<4;++i)require(!pthread_create(&workers[i],NULL,reader,(void *)(uintptr_t)i),"corpus worker create");for(unsigned i=0;i<4;++i)require(!pthread_join(workers[i],NULL),"corpus worker join");win64_region_remove((uintptr_t)&w);
    printf("{\"success\":true,\"relative_native_cases\":%u,\"relative_NULL\":true,\"minimal_relative_region_cases\":7,\"canonical_native_cases\":%u,\"negative73_cases\":%zu,\"parallel_workers\":4,\"parallel_corpus_calls\":2048,\"total_calls\":%lu,\"checks\":%lu,\"CPU_RSP_LastError\":true,\"active_UTF16_output_and_NUL_native_exact\":true,\"untouched_tail_explicit_policy\":true,\"outside_MAX_PATH_and_input_guards\":true,\"odd_byte_UTF16_input_output_alias\":true,\"actual_core_pointer_registry\":true}\n",relative_count,case_count,sizeof(negative_cases)/sizeof(negative_cases[0]),atomic_load(&calls),atomic_load(&checks));free(cases);free(pool);return 0;
}
