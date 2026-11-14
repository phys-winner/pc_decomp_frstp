/* Native UCRT records, real production pointer/errno/TEB implementation. */
#include "../android/host/win64.c"
#include "win64_utility.h"
/* The converter regression has no guest callbacks. */
void dispatch(CPU *cpu,uint64_t target){(void)cpu;(void)target;abort();}
static _Thread_local unsigned char numeric_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(numeric_teb))abort();memcpy(&v,numeric_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(numeric_teb))abort();memcpy(numeric_teb+offset,&value,bytes);}
typedef struct{uint32_t operation;int32_t base;uint32_t flags,mxcsr;uint64_t expected;int32_t error,end,seed;uint32_t source;char text[128];} numeric_record;
_Static_assert(sizeof(numeric_record)==168,"prepared record layout");
#include "numeric_mantissa_vectors.h"
static numeric_record *records;static uint32_t record_count;static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"NUMERIC_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static int one(const numeric_record *r,unsigned index,unsigned char *guard,const char *text){
    CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));memset(guard,0x6b,40);
    cpu.rcx=(uintptr_t)text;cpu.rdx=(r->flags&2)?0:(uintptr_t)(guard+9);
    cpu.r8=UINT64_C(0xfedcba9800000000)|(uint32_t)r->base;cpu.mxcsr=r->mxcsr;before=cpu;
    crt_errno=r->seed;win64_set_error(0x5a17);errno=2718;
    uint64_t control,status,after_control,after_status;
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(control),"=r"(status));
    if(!win64_utility_call(&cpu,r->operation==1?"strtod":"strtoul"))abort();
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(after_control),"=r"(after_status));
    uint64_t value=r->operation==1?cpu.xmm[0].u64[0]:cpu.rax;
    if(r->operation==1)cpu.xmm[0].u64[0]=before.xmm[0].u64[0];else cpu.rax=before.rax;
    uint64_t end=0;memcpy(&end,guard+9,8);uint64_t expected_end=(uintptr_t)text+(uint64_t)r->end;
    int bad=value!=r->expected||crt_errno!=r->error||errno!=2718||win64_get_error()!=0x5a17||memcmp(&cpu,&before,sizeof(cpu))||control!=after_control||status!=after_status;
    if(!(r->flags&2)&&end!=expected_end)bad=1;
    for(unsigned j=0;j<40;++j)if(((r->flags&2)||j<9||j>=17)&&guard[j]!=0x6b)bad=1;
    if(bad){fprintf(stderr,"NUMERIC_MISMATCH case=%u source=%u op=%u text=%s base=%d csr=%x got=%016llx/%d/end%lld expected=%016llx/%d/end%d hosterrno=%d\n",index,r->source,r->operation,text,r->base,r->mxcsr,(unsigned long long)value,crt_errno,(long long)(end-(uintptr_t)text),(unsigned long long)r->expected,r->error,r->end,errno);return 0;}
    return 1;
}
static void *worker(void *opaque){
    unsigned char guard[40];win64_region_add((uintptr_t)guard,sizeof(guard),1);unsigned start=(unsigned)(uintptr_t)opaque;
    for(unsigned i=0;i<2048;++i){numeric_record *r=&records[(i*17+start)%record_count];if(!one(r,i,guard,r->text))abort();}
    win64_region_remove((uintptr_t)guard);return NULL;
}
int main(int argc,char **argv){
    CHECK(argc==2);
    if(argv[1][0]=='-'){
        static char text[128];unsigned char guard[40];CPU c={0};strcpy(text,"1e309");
        win64_region_add((uintptr_t)text,sizeof(text),1);win64_region_add((uintptr_t)guard,sizeof(guard),1);
        c.rcx=(uintptr_t)text;c.rdx=(uintptr_t)(guard+9);c.mxcsr=0x1f80;c.r8=10;const char *name="strtod";
        if(!strcmp(argv[1],"--null-double"))c.rcx=0;
        else if(!strcmp(argv[1],"--null-unsigned")){c.rcx=0;name="strtoul";}
        else if(!strcmp(argv[1],"--base")){c.r8=1;name="strtoul";}
        else if(!strcmp(argv[1],"--input"))c.rcx=1;
        else if(!strcmp(argv[1],"--output"))c.rdx=1;
        else if(!strcmp(argv[1],"--unterminated"))memset(text,'1',sizeof(text));
        else if(!strcmp(argv[1],"--range-overflow"))c.mxcsr|=1u<<13;
        else if(!strcmp(argv[1],"--directed-interior")){strcpy(text,".5");c.mxcsr|=2u<<13;}
        else if(!strcmp(argv[1],"--range-underflow")){strcpy(text,"1e-400");c.mxcsr|=2u<<13;}
        else if(!strcmp(argv[1],"--range-subnormal")){strcpy(text,"5e-324");c.mxcsr|=3u<<13;}
        else if(!strcmp(argv[1],"--range-max")){strcpy(text,"0x1.fffffffffffffp1023");c.mxcsr|=1u<<13;}
        else if(!strcmp(argv[1],"--mantissa-bound")){
            static char long_text[1024];const char *tie="1.00000000000000011102230246251565404236316680908203125";
            size_t n=strlen(tie);memcpy(long_text,tie,n);memset(long_text+n,'0',800);long_text[n+800]='1';long_text[n+801]=0;
            win64_region_add((uintptr_t)long_text,sizeof(long_text),0);c.rcx=(uintptr_t)long_text;
        }
        else return 3;
        win64_utility_call(&c,name);return 99;
    }
    FILE *f=fopen(argv[1],"rb");CHECK(f);char magic[8];CHECK(fread(magic,1,8,f)==8&&!memcmp(magic,"FPNUM001",8));CHECK(fread(&record_count,4,1,f)==1&&record_count>1000&&record_count<1000000);
    records=calloc(record_count,sizeof(*records));CHECK(records);CHECK(fread(records,sizeof(*records),record_count,f)==record_count);CHECK(fgetc(f)==EOF&&!ferror(f));CHECK(!fclose(f));
    win64_region_add((uintptr_t)records,(size_t)record_count*sizeof(*records),0);unsigned char guard[40];win64_region_add((uintptr_t)guard,sizeof(guard),1);
    uint64_t old_control,old_status;__asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(old_control),"=r"(old_status));
    uint64_t mode=(UINT64_C(1)<<24)|(UINT64_C(2)<<22),status=0x9f;__asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(mode),"r"(status):"memory");
    for(unsigned i=0;i<record_count;++i)CHECK(one(&records[i],i,guard,records[i].text));
    unsigned boundary_count=(unsigned)(sizeof(numeric_mantissa_vectors)/sizeof(numeric_mantissa_vectors[0]));
    for(unsigned i=0;i<boundary_count;++i){
        const numeric_mantissa_vector *v=&numeric_mantissa_vectors[i];numeric_record r={0};r.operation=1;r.mxcsr=0x1f80;r.seed=r.error=1927;r.expected=v->bits;r.end=v->end;r.source=3;
        win64_region_add((uintptr_t)v->text,strlen(v->text)+1,0);CHECK(one(&r,record_count+i,guard,v->text));
    }
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    CPU c={0},before;c.rax=123;c.rsp=456;before=c;CHECK(!win64_utility_call(&c,"strtof")&&!memcmp(&c,&before,sizeof(c)));
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(old_control),"r"(old_status):"memory");
    printf("{\"success\":true,\"native_cases\":%u,\"native_mantissa_boundary_cases\":%u,\"checks\":%u,\"concurrent_calls\":8192,\"threads\":4,\"fullCPU\":true,\"XMM0_low64_only_strtod\":true,\"RAX_zeroextended32_strtoul\":true,\"no_RSP_pop\":true,\"endptr_unaligned_canaries\":true,\"MXCSR_preserved\":true,\"hostFP_preserved\":true,\"LastError_errno_preserved\":true}\n",record_count,boundary_count,checks);return 0;
}
