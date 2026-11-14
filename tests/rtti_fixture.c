#include "win64_rtti.h"
#include "win64.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define TLS __declspec(thread)
#define strcasecmp _stricmp
#else
#include <pthread.h>
#include <strings.h>
#define TLS _Thread_local
#endif
typedef struct{uint32_t table,source,target,kind,delta,reference,mode,seed;} input_row;
typedef struct{input_row input;uint64_t expected;uint32_t error;int32_t native_errno;uint32_t csr_after,reserved;} row;
_Static_assert(sizeof(row)==56,"RTTI row56");
static unsigned char *image;static size_t image_bytes;static row *rows;static size_t row_count;
typedef struct{uint64_t address;size_t bytes;int write;} region;
static TLS region ranges[4];static TLS unsigned range_count;static TLS uint32_t last_error,guest_errno;
static _Atomic unsigned failures;static _Atomic unsigned long calls;
static void check(int okay,const char*why){if(!okay){fprintf(stderr,"RTTI_FIXTURE_FAIL %s\n",why);_Exit(1);}}
uint32_t win64_get_error(void){return last_error;}
void win64_set_error(uint32_t value){last_error=value;}
void win64_stdio_set_errno(int value){guest_errno=(uint32_t)value;}
uint64_t win64_alloc(size_t bytes,int zero){(void)bytes;(void)zero;check(0,"dynamic cast must not allocate");return 0;}
void *win64_pointer(uint64_t address,size_t bytes,int writing){
    for(unsigned i=0;i<range_count;++i){region*r=&ranges[i];if(address>=r->address&&address-r->address<=r->bytes&&bytes<=r->bytes-(address-r->address)&&(!writing||r->write))return(void*)(uintptr_t)address;}
    fprintf(stderr,"STOP Win64 RTTI fixture checked span address=%llx bytes=%zu\n",(unsigned long long)address,bytes);_Exit(73);
}
static void add(void*p,size_t n,int write){check(range_count<4,"range capacity");ranges[range_count++]=(region){(uintptr_t)p,n,write};}
static void fp_get(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void fp_set(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static int imported(CPU*c,const char*dll,const char*name){
#include "rtti_route.inc"
    return 0;
}
static int one(const row*r,unsigned index){
    uint64_t object[8],stack[8];for(unsigned i=0;i<8;++i){object[i]=UINT64_C(0xbad050a1bad050a1)+i;stack[i]=UINT64_C(0x5637281956372819)+i;}
    object[0]=(uintptr_t)(image+r->input.table);stack[5]=(UINT64_C(0x91234567)<<32)|r->input.reference;
    uint64_t object_before[8],stack_before[8];memcpy(object_before,object,sizeof(object));memcpy(stack_before,stack,sizeof(stack));
    range_count=0;add(image,image_bytes,0);add(object,sizeof(object),0);add(stack,sizeof(stack),0);
    CPU c;unsigned char*raw=(unsigned char*)&c;for(unsigned i=0;i<sizeof(c);++i)raw[i]=(unsigned char)(i*13u+index*7u);
    c.rcx=r->input.kind?0:(uintptr_t)object;c.rdx=(UINT64_C(0x81234567)<<32)|r->input.delta;
    c.r8=(uintptr_t)image+r->input.source;c.r9=(uintptr_t)image+r->input.target;c.rsp=(uintptr_t)stack;
    CPU expected=c;expected.rax=r->expected?(uintptr_t)object:0;if(index&1)expected.rsp+=8;
    errno=2718;last_error=0x5a17;guest_errno=1379;uint64_t before[2],after[2];fp_get(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=(index&1)?imported(&c,"VcRuNtImE140.DlL","__RTDynamicCast"):win64_rtti_call(&c,"__RTDynamicCast");
    fp_get(after);int native_errno=errno;int okay=handled&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(before,after,sizeof(before))&&native_errno==2718&&last_error==0x5a17&&guest_errno==1379&&!memcmp(object,object_before,sizeof(object))&&!memcmp(stack,stack_before,sizeof(stack));
#if defined(_WIN32)
    okay=okay&&GetLastError()==0x5a17;
#endif
    if(!okay&&atomic_fetch_add(&failures,1)<12)fprintf(stderr,"RTTI_FAIL index=%u table=%x source=%x target=%x kind=%u result=%llx expected=%llx CPU=%d FP=%d errno=%d\n",index,r->input.table,r->input.source,r->input.target,r->input.kind,(unsigned long long)c.rax,(unsigned long long)expected.rax,memcmp(&c,&expected,sizeof(c)),memcmp(before,after,sizeof(before)),native_errno);
    atomic_fetch_add(&calls,1);return okay;
}
static void *load(const char*path,size_t *bytes){FILE*f=fopen(path,"rb");if(!f)return NULL;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);void*p=malloc((size_t)n);if(n<=0||!p||fread(p,(size_t)n,1,f)!=1)return NULL;fclose(f);*bytes=(size_t)n;return p;}
#if !defined(_WIN32)
static void *worker(void*data){uintptr_t seed=(uintptr_t)data;uint64_t original[2],wanted[2];fp_get(original);wanted[0]=(original[0]&~(UINT64_C(3)<<22))|(seed<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;fp_set(wanted);
    for(unsigned i=0;i<2048;++i)if(!one(&rows[(i*137+seed*31)%row_count],i)){fp_set(original);return(void*)1;}fp_set(original);return NULL;}
#endif
int main(int argc,char**argv){
    if(argc<3)return 2;image=load(argv[1],&image_bytes);size_t bytes;rows=load(argv[2],&bytes);check(image&&rows&&bytes%sizeof(row)==0,"input files");row_count=bytes/sizeof(row);check(row_count!=0,"rows");
    for(size_t i=0;i<row_count;++i){uint32_t table=rows[i].input.table;check(table>=8&&table<=image_bytes-8,"table input bounds");uint64_t locator;memcpy(&locator,image+table-8,8);if(locator<image_bytes){locator+=(uintptr_t)image;memcpy(image+table-8,&locator,8);}}
    check(win64_rtti_init(image,image_bytes),"image initialization");
    if(argc==4){
        const char*reason=argv[3];CPU c={0};uint64_t object[8]={0},stack[8]={0};row*r=&rows[0];
        object[0]=(uintptr_t)(image+r->input.table);c.rcx=(uintptr_t)object;c.rsp=(uintptr_t)stack;c.r8=(uintptr_t)(image+r->input.source);c.r9=(uintptr_t)(image+r->input.target);
        range_count=0;add(image,image_bytes,0);add(object,sizeof(object),0);add(stack,sizeof(stack),0);
        uint64_t locator;memcpy(&locator,image+r->input.table-8,8);uint32_t hierarchy;memcpy(&hierarchy,(void*)(uintptr_t)(locator+16),4);uint32_t array;memcpy(&array,image+hierarchy+12,4);uint32_t base;memcpy(&base,image+array,4);
        if(!strcmp(reason,"delta"))c.rdx=1;
        else if(!strcmp(reason,"reference"))stack[5]=1;
        else if(!strcmp(reason,"stack"))c.rsp=1;
        else if(!strcmp(reason,"stack_overflow"))c.rsp=UINT64_MAX-7;
        else if(!strcmp(reason,"object"))c.rcx=1;
        else if(!strcmp(reason,"vtable"))object[0]=0;
        else if(!strcmp(reason,"locator_self")){uint32_t wrong=0;memcpy((void*)(uintptr_t)(locator+20),&wrong,4);}
        else if(!strcmp(reason,"locator_offset")){uint32_t wrong=8;memcpy((void*)(uintptr_t)(locator+4),&wrong,4);}
        else if(!strcmp(reason,"construction")){uint32_t wrong=4;memcpy((void*)(uintptr_t)(locator+8),&wrong,4);}
        else if(!strcmp(reason,"multiple")){uint32_t wrong=1;memcpy(image+hierarchy+4,&wrong,4);}
        else if(!strcmp(reason,"virtual")){uint32_t wrong=3;memcpy(image+hierarchy+4,&wrong,4);}
        else if(!strcmp(reason,"count")){uint32_t wrong=65;memcpy(image+hierarchy+8,&wrong,4);}
        else if(!strcmp(reason,"array")){uint32_t wrong=0xfffffff0;memcpy(image+hierarchy+12,&wrong,4);}
        else if(!strcmp(reason,"pmd")){uint32_t wrong=8;memcpy(image+base+8,&wrong,4);}
        else if(!strcmp(reason,"private")){uint32_t wrong=0x44;memcpy(image+base+20,&wrong,4);}
        else if(!strcmp(reason,"source"))c.r8=1;
        else if(!strcmp(reason,"target"))c.r9=UINT64_MAX;
        else if(!strcmp(reason,"uninitialized"))check(!win64_rtti_init(NULL,0),"init rejects NULL");
        else return 2;imported(&c,"VCRUNTIME140.dll","__RTDynamicCast");return 99;
    }
    uint64_t original[2],wanted[2];fp_get(original);
    for(size_t i=0;i<row_count;++i){
#if defined(__aarch64__)
        wanted[0]=(original[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=0x9fd5|((i&3u)<<13);wanted[1]=0;
#endif
        fp_set(wanted);if(!one(&rows[i],(unsigned)i)&&atomic_load(&failures)>=12)break;
    }fp_set(original);unsigned parallel=0;
#if !defined(_WIN32)
    if(!atomic_load(&failures)){pthread_t thread[4];for(uintptr_t i=0;i<4;++i)check(!pthread_create(&thread[i],NULL,worker,(void*)i),"pthread create");for(unsigned i=0;i<4;++i){void*result;check(!pthread_join(thread[i],&result)&&!result,"pthread join");}parallel=8192;}
#endif
    const char*dlls[]={"ucrtbase.dll","VCRUNTIME140_1.dll","kernel32.dll"};unsigned declines=0;
    for(unsigned i=0;i<3;++i){CPU c;memset(&c,0xa5,sizeof(c));CPU before=c;check(!imported(&c,dlls[i],"__RTDynamicCast")&&!memcmp(&c,&before,sizeof(c)),"namespace decline");++declines;}
    const char*names[]={"__RTCastToVoid","__RTtypeid","__rtdynamiccast","__RTDynamicCastX"};
    for(unsigned i=0;i<4;++i){CPU c;memset(&c,0xa5,sizeof(c));CPU before=c;check(!imported(&c,"vcruntime140.dll",names[i])&&!memcmp(&c,&before,sizeof(c)),"name decline");++declines;}
    printf("{\"success\":%s,\"rows\":%zu,\"calls\":%lu,\"parallel\":%u,\"declines\":%u,\"failures\":%u,\"fullCPU_memory_nativeFP_errno_TLS\":true,\"single_import_pop\":true}\n",atomic_load(&failures)?"false":"true",row_count,atomic_load(&calls),parallel,declines,atomic_load(&failures));free(rows);free(image);return atomic_load(&failures)?1:0;
}
