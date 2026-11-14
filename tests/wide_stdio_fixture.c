/* Isolated real ARM64 formatter, native UCRT vectors, strict guest regions. */
#define _GNU_SOURCE
#include "win64_wide_stdio.h"
#include "win64.h"
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(_WIN32)
#include <xmmintrin.h>
#endif

typedef struct {uint64_t control,status;} native_fp;
static native_fp read_fp(void){native_fp s;
#if defined(_WIN32)
    s.control=_mm_getcsr();s.status=0;
#else
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr":"=r"(s.control),"=r"(s.status));
#endif
    return s;
}
static void write_fp(native_fp s){
#if defined(_WIN32)
    _mm_setcsr((unsigned)s.control);
#else
    __asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1"::"r"(s.control),"r"(s.status));
#endif
}
static native_fp mode_fp(unsigned mode){native_fp s;
#if defined(_WIN32)
    s.control=0x1f80|0x25|((mode&3)<<13)|((mode&4)?0x40:0)|((mode&8)?0x8000:0);s.status=0;
#else
    s.control=((uint64_t)(mode&3)<<22)|((mode&4)?UINT64_C(1)<<24:0)|((mode&8)?UINT64_C(1)<<25:0);s.status=0x1f;
#endif
    return s;
}

typedef struct {uint64_t address;size_t bytes;int writable;} region;
static _Thread_local region regions[64];
static _Thread_local unsigned region_count;
static _Thread_local int guest_errno;
static int *child_errno;
static _Atomic unsigned long checks,calls;
static void require(int condition,const char *reason){if(!condition){fprintf(stderr,"WIDE_FIXTURE_FAIL %s\n",reason);_Exit(1);}atomic_fetch_add(&checks,1);}
static _Noreturn void bounds(void){fprintf(stderr,"STOP wide fixture guest region bounds\n");fflush(stderr);_Exit(73);}
void win64_region_add(uint64_t address,size_t bytes,int writable){if(!address||!bytes||region_count==64||bytes>UINT64_MAX-address)bounds();regions[region_count++]=(region){address,bytes,writable};}
void win64_region_remove(uint64_t address){for(unsigned i=0;i<region_count;++i)if(regions[i].address==address){regions[i]=regions[--region_count];return;}bounds();}
void *win64_pointer(uint64_t address,size_t bytes,int writable){
    if(!address||bytes>UINT64_MAX-address)bounds();
    for(unsigned i=0;i<region_count;++i){region *r=&regions[i];if(address>=r->address&&address-r->address<=r->bytes&&bytes<=r->bytes-(address-r->address)&&(!writable||r->writable))return(void *)(uintptr_t)address;}
    bounds();
}
uint64_t win64_argument(CPU *c,unsigned index){if(index<4){switch(index){case 0:return c->rcx;case 1:return c->rdx;case 2:return c->r8;default:return c->r9;}}uint64_t v;memcpy(&v,win64_pointer(c->rsp+0x28+(index-4)*8,8,0),8);return v;}
void win64_stdio_set_errno(int value){guest_errno=value;if(child_errno)*child_errno=value;}

typedef struct {
    uint32_t secure,options;uint64_t capacity,count;
    uint32_t null_buffer,invalid_callback;int32_t result,error;
    uint32_t format_bytes,left_bytes,right_bytes,output_bytes;
} vector_header;
_Static_assert(sizeof(vector_header)==56,"Pinned Win64/native fixture vector layout");
typedef struct {vector_header h;unsigned char *format,*left,*right,*expected;} vector;
static vector vectors[4096];static unsigned vector_count;
static unsigned char *data;static size_t data_bytes;
static void load_vectors(const char *path){
    FILE *file=fopen(path,"rb");require(file!=NULL,"open native vector file");require(!fseek(file,0,SEEK_END),"seek vectors");long n=ftell(file);require(n>=8&&n<1024*1024,"bounded vectors");rewind(file);
    data_bytes=(size_t)n;data=malloc(data_bytes);require(data!=NULL&&fread(data,1,data_bytes,file)==data_bytes,"read vectors");require(!fclose(file),"close vectors");
    uint32_t magic;memcpy(&magic,data,4);memcpy(&vector_count,data+4,4);require(magic==UINT32_C(0x31504657)&&vector_count<=4096,"native vector header");size_t offset=8;
    for(unsigned i=0;i<vector_count;++i){vector *v=&vectors[i];require(sizeof(v->h)<=data_bytes-offset,"case header bounds");memcpy(&v->h,data+offset,sizeof(v->h));offset+=sizeof(v->h);
        uint32_t sizes[4]={v->h.format_bytes,v->h.left_bytes,v->h.right_bytes,v->h.output_bytes};unsigned char **pointers[4]={&v->format,&v->left,&v->right,&v->expected};
        require(v->h.capacity<65536&&v->h.output_bytes==((v->h.null_buffer&1)?0:((v->h.capacity?v->h.capacity:1)+8)*2),"bounded output vector");
        for(unsigned j=0;j<4;++j){require(!(sizes[j]&1)&&sizes[j]<=data_bytes-offset,"UTF16 case bounds");*pointers[j]=sizes[j]?data+offset:NULL;offset+=sizes[j];if(j==1&&(v->h.null_buffer&8))require(sizes[j]==24,"native color three QWORD slots");else if(j==1&&(v->h.null_buffer&2))require(sizes[j]==8,"native integer QWORD slot");else if(j<3&&sizes[j])require(sizes[j]>=2&&!(*pointers[j])[sizes[j]-1]&&!(*pointers[j])[sizes[j]-2],"native UTF16 terminator");}
    }
    require(offset==data_bytes,"exact native vector consumption");
}
static void run_vector(const vector *v,int invalid_child){
    unsigned char output[131088];require(v->h.output_bytes<=sizeof(output),"case output stack size");
    for(size_t i=0;i<sizeof(output);i+=2){output[i]=0x5A;output[i+1]=0xA5;}
    uint64_t va[3]={(uintptr_t)v->left,(v->h.null_buffer&4)?UINT64_MAX:(uintptr_t)v->right,UINT64_MAX},stack[12];if(v->h.null_buffer&8)memcpy(va,v->left,24);else if(v->h.null_buffer&2)memcpy(&va[0],v->left,8);memset(stack,0xC7,sizeof(stack));CPU c;memset(&c,0xA5,sizeof(c));
    c.rsp=(uintptr_t)&stack[1];c.rcx=v->h.options;c.rdx=(v->h.null_buffer&1)?0:(uintptr_t)output;c.r8=v->h.capacity;c.r9=v->h.secure?v->h.count:(uintptr_t)v->format;
    stack[6]=v->h.secure?(uintptr_t)v->format:0;stack[7]=v->h.secure?0:(uintptr_t)va;stack[8]=(uintptr_t)va;
    CPU expected=c;expected.rax=(uint32_t)v->h.result;uint64_t stack_before[12],va_before[3];memcpy(stack_before,stack,sizeof(stack));memcpy(va_before,va,sizeof(va));
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)va,(v->h.null_buffer&8)?24:(v->h.null_buffer&4)?8:16,0);win64_region_add((uintptr_t)output,v->h.output_bytes?v->h.output_bytes:2,1);
    guest_errno=1927;errno=2719;native_fp saved_fp=read_fp(),before_fp=mode_fp((unsigned)(v-vectors));write_fp(before_fp);
    int handled=win64_wide_stdio_call(&c,v->h.secure?"__stdio_common_vsnwprintf_s":"__stdio_common_vswprintf");
    native_fp after_fp=read_fp();write_fp(saved_fp);require(!memcmp(&before_fp,&after_fp,sizeof(before_fp)),"complete native FP controls and sticky flags preserved");
    require(!invalid_child,"invalid native callback must stop, not return");require(handled,"native vector handled");
    require(!memcmp(&c,&expected,sizeof(c)),"native result and complete CPU/RSP preservation");require(!memcmp(stack_before,stack,sizeof(stack))&&!memcmp(va_before,va,sizeof(va)),"saved stack/va bytes unchanged");
    require(guest_errno==v->h.error,"native guest errno equality");
    if(v->h.output_bytes&&memcmp(output,v->expected,v->h.output_bytes))for(size_t i=0;i<v->h.output_bytes;++i)if(output[i]!=v->expected[i]){fprintf(stderr,"WIDE_OUTPUT_MISMATCH byte=%zu actual=%02x expected=%02x capacity=%llu count=%llu\n",i,output[i],v->expected[i],(unsigned long long)v->h.capacity,(unsigned long long)v->h.count);break;}
    require(!v->h.output_bytes||!memcmp(output,v->expected,v->h.output_bytes),"native exact UTF16 output and canaries");
    require(errno==2719,"host native errno preserved");
    win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)va);win64_region_remove((uintptr_t)stack);atomic_fetch_add(&calls,1);
}
static void extra_case(const char *name){
    uint16_t fmt[32]={'%','s','/','%','s',0},left[]={'A',0},right[]={'B',0},output[8];memset(output,0xA5,sizeof(output));size_t format_bytes=12;
    if(!strcmp(name,"short-va-dat")){fmt[5]='.';fmt[6]='d';fmt[7]='a';fmt[8]='t';fmt[9]=0;format_bytes=20;}
    if(!strcmp(name,"short-va-index")){const uint16_t index[]={'_','%','.','3','d','%','s',0};memcpy(fmt,index,sizeof(index));format_bytes=sizeof(index);}
    if(!strcmp(name,"short-va-custom")){const char *text="%s/custom_localizations";format_bytes=(strlen(text)+1)*2;for(size_t i=0;i<=strlen(text);++i)fmt[i]=(unsigned char)text[i];}
    if(!strcmp(name,"short-va-alt")){const char *text="%s.alt";format_bytes=(strlen(text)+1)*2;for(size_t i=0;i<=strlen(text);++i)fmt[i]=(unsigned char)text[i];}
    if(!strncmp(name,"short-va-color",14)||!strcmp(name,"bad-color-format")){const char *text="|#color=%02x%02x%02x|";format_bytes=(strlen(text)+1)*2;for(size_t i=0;i<=strlen(text);++i)fmt[i]=(unsigned char)text[i];}
    uint64_t va[2]={(uintptr_t)left,(uintptr_t)right},stack[10]={0};CPU c={0};c.rsp=(uintptr_t)stack;c.rcx=6;c.r9=(uintptr_t)fmt;stack[6]=(uintptr_t)va;
    win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)fmt,format_bytes,0);win64_region_add((uintptr_t)left,sizeof(left),0);win64_region_add((uintptr_t)right,sizeof(right),0);win64_region_add((uintptr_t)va,sizeof(va),0);win64_region_add((uintptr_t)output,sizeof(output),1);
    if(!strncmp(name,"short-va-color",14)){win64_region_remove((uintptr_t)va);win64_region_add((uintptr_t)va,(size_t)strtoul(name+14,NULL,10),0);}
    else if(!strcmp(name,"bad-color-format"))fmt[11]='X';
    else if(!strcmp(name,"short-va")||!strcmp(name,"short-va-dat")||!strcmp(name,"short-va-index")||!strcmp(name,"short-va-custom")||!strcmp(name,"short-va-alt")){win64_region_remove((uintptr_t)va);win64_region_add((uintptr_t)va,(!strcmp(name,"short-va-custom")||!strcmp(name,"short-va-alt"))?7:8,0);}
    else if(!strcmp(name,"NULL-va"))stack[6]=0;
    else if(!strcmp(name,"unterminated-input")){win64_region_remove((uintptr_t)left);win64_region_add((uintptr_t)left,2,0);}
    else if(!strcmp(name,"short-format")){win64_region_remove((uintptr_t)fmt);win64_region_add((uintptr_t)fmt,format_bytes-2,0);}
    else if(!strcmp(name,"bad-format"))fmt[1]='d';
    else if(!strcmp(name,"bad-option"))c.rcx=4;
    else if(!strcmp(name,"nonNULL-locale"))stack[5]=1;
    else if(!strcmp(name,"nonquery-vswprintf")){c.rdx=(uintptr_t)output;c.r8=8;}
    else if(!strcmp(name,"NULL-format"))c.r9=0;
    else if(!strcmp(name,"overflow-va"))stack[6]=UINT64_MAX-7;
    else if(!strcmp(name,"overflow-input"))va[0]=UINT64_MAX;
    else if(!strcmp(name,"secure-short-output")||!strcmp(name,"secure-readonly-output")||!strcmp(name,"capacity-overflow")){
        c.rcx=4;c.rdx=(uintptr_t)output;c.r8=8;c.r9=3;stack[5]=(uintptr_t)fmt;stack[6]=0;stack[7]=(uintptr_t)va;
        win64_region_remove((uintptr_t)output);win64_region_add((uintptr_t)output,!strcmp(name,"secure-short-output")?2:sizeof(output),strcmp(name,"secure-readonly-output")!=0);
        if(!strcmp(name,"capacity-overflow"))c.r8=UINT64_MAX;
        win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s");_Exit(1);
    }else _Exit(1);
    win64_wide_stdio_call(&c,"__stdio_common_vswprintf");_Exit(1);
}
static void *worker(void *unused){(void)unused;win64_region_add((uintptr_t)data,data_bytes,0);unsigned valid[4096],n=0;for(unsigned i=0;i<vector_count;++i)if(!vectors[i].h.invalid_callback)valid[n++]=i;
    for(unsigned i=0;i<512;++i)run_vector(&vectors[valid[i%n]],0);
    win64_region_remove((uintptr_t)data);require(!region_count,"thread registration cleanup");return NULL;
}
int main(int argc,char **argv){
    require(argc==2,"native vector path required");load_vectors(argv[1]);win64_region_add((uintptr_t)data,data_bytes,0);win64_wide_stdio_self_test();
    unsigned valid=0,invalid=0;
    for(unsigned i=0;i<vector_count;++i){if(!vectors[i].h.invalid_callback){run_vector(&vectors[i],0);++valid;continue;}
        int *shared=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);require(shared!=MAP_FAILED,"child errno shared page");*shared=1927;
        pid_t pid=fork();require(pid>=0,"native-invalid fork");if(!pid){child_errno=shared;run_vector(&vectors[i],1);_Exit(1);}int status;require(waitpid(pid,&status,0)==pid,"native-invalid wait");
        require(WIFEXITED(status)&&WEXITSTATUS(status)==73&&*shared==vectors[i].h.error,"native invalid callback explicit73 and exact guest errno");require(!munmap(shared,4096),"child errno cleanup");++invalid;
    }
    const char *negative[]={"short-va","NULL-va","unterminated-input","short-format","bad-format","bad-option","nonNULL-locale","nonquery-vswprintf","NULL-format","overflow-va","overflow-input","secure-short-output","secure-readonly-output","capacity-overflow","short-va-dat","short-va-index","short-va-custom","short-va-alt","short-va-color8","short-va-color16","short-va-color23","bad-color-format"};
    for(unsigned i=0;i<sizeof(negative)/sizeof(negative[0]);++i){pid_t pid=fork();require(pid>=0,"bounds fork");if(!pid)extra_case(negative[i]);int status;require(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==73,"bounds/options/format child explicit73");}
    uint64_t stack[8]={0};uint16_t fmt[]={'%','s','/','%','s',0};win64_region_add((uintptr_t)stack,sizeof(stack),1);win64_region_add((uintptr_t)fmt,sizeof(fmt),0);
    CPU c;memset(&c,0xA5,sizeof(c));CPU original=c;require(!win64_wide_stdio_call(&c,"not_a_stdio_import")&&!memcmp(&c,&original,sizeof(c)),"unknown name leaves entire CPU untouched");
    c=(CPU){0};c.rsp=(uintptr_t)stack;c.rcx=4;stack[5]=(uintptr_t)fmt;stack[7]=UINT64_MAX;require(win64_wide_stdio_call(&c,"__stdio_common_vsnwprintf_s")&&c.rax==0,"secure no-work does not read va_list");
    win64_region_remove((uintptr_t)fmt);win64_region_remove((uintptr_t)stack);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)require(!pthread_create(&threads[i],NULL,worker,NULL),"concurrent formatter worker create");for(unsigned i=0;i<4;++i)require(!pthread_join(threads[i],NULL),"concurrent formatter worker join");
    win64_region_remove((uintptr_t)data);require(!region_count,"all guest region cleanup");
    printf("{\"success\":true,\"native_valid_vectors\":%u,\"native_invalid_callback_stops\":%u,\"bounds_unsupported_stops\":%zu,\"parallel_workers\":4,\"parallel_calls\":2048,\"total_returned_calls\":%lu,\"checks\":%lu,\"full_CPU_RSP_va_canaries\":true,\"UTF16_units_exact\":true}\n",valid,invalid,sizeof(negative)/sizeof(negative[0]),atomic_load(&calls),atomic_load(&checks));free(data);return 0;
}
