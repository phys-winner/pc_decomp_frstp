/* Production utility provider, native LONG32/fullRAX vectors, real ARM regions. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define TLS __declspec(thread)
typedef struct{uint64_t at;size_t bytes;int write;} test_region;
static TLS test_region regions[8];static TLS unsigned region_count;
static TLS int crt_errno;static TLS uint32_t guest_error;
void win64_region_add(uint64_t at,size_t bytes,int write){if(region_count==8)abort();regions[region_count++]=(test_region){at,bytes,write};}
void win64_region_remove(uint64_t at){for(unsigned i=0;i<region_count;++i)if(regions[i].at==at){regions[i]=regions[--region_count];return;}abort();}
void *win64_pointer(uint64_t at,size_t bytes,int write){for(unsigned i=0;i<region_count;++i){test_region *r=&regions[i];if(at>=r->at&&at-r->at<=r->bytes&&bytes<=r->bytes-(at-r->at)&&(!write||r->write))return(void*)(uintptr_t)at;}fputs("STOP strtol fixture checked pointer\n",stderr);_Exit(73);}
void win64_stdio_set_errno(int value){crt_errno=value;}
uint32_t win64_get_error(void){return guest_error;}void win64_set_error(uint32_t value){guest_error=value;}
uint64_t win64_argument(CPU*c,unsigned i){(void)c;(void)i;abort();}
const char *win64_string_a(uint64_t p){(void)p;abort();}
#else
#include "../android/host/win64.c"
#define TLS _Thread_local
static TLS unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t off,unsigned bytes){uint64_t value=0;if(off+bytes>sizeof(fixture_teb))abort();memcpy(&value,fixture_teb+off,bytes);return value;}
void frostpunk_teb_write(uint64_t off,uint64_t value,unsigned bytes){if(off+bytes>sizeof(fixture_teb))abort();memcpy(fixture_teb+off,&value,bytes);}
#endif
#include "win64_utility.h"
void dispatch(CPU*c,uint64_t t){(void)c;(void)t;abort();}
typedef struct{int32_t base,seed;uint32_t csr,flags;char text[256];uint64_t result;int32_t error,end;uint32_t expected_csr,last_error;} row;
_Static_assert(sizeof(row)==296,"strtol record296");
static row *rows;static uint32_t row_count;
static void need(int okay,const char *why){if(!okay){fprintf(stderr,"STRTOL_FIXTURE_FAIL %s\n",why);_Exit(1);}}
static int route(CPU *c,const char *name){const int crt=1;
#include "strtol_route.inc"
    return 0;
}
static void native_state(uint64_t state[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr":"=r"(state[0]),"=r"(state[1]));
#else
    state[0]=_mm_getcsr();state[1]=0;
#endif
}
static void native_set(const uint64_t state[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(state[0]),"r"(state[1]):"memory");
#else
    _mm_setcsr((unsigned)state[0]);
#endif
}
static int one(const row *r){
    unsigned char output[32],wanted[32];memset(output,0xa5,sizeof(output));memcpy(wanted,output,sizeof(output));
    if(!(r->flags&2u)){uint64_t end=(uintptr_t)r->text+(uint64_t)r->end;memcpy(wanted+9,&end,8);}
    win64_region_add((uintptr_t)output,32,1);
    CPU c,b;memset(&c,0xa7,sizeof(c));c.rcx=(uintptr_t)r->text;c.rdx=(r->flags&2u)?0:(uintptr_t)(output+9);
    c.r8=UINT64_C(0xabcdf01200000000)|(uint32_t)r->base;c.mxcsr=r->csr;b=c;
    crt_errno=r->seed;errno=2718;win64_set_error(0x5a17);uint64_t old[2],after[2];native_state(old);
    const char *api=(r->flags&1u)?"strtoul":"strtol";int handled=(r->flags&8u)?route(&c,api):win64_utility_call(&c,api);
    native_state(after);int okay=handled&&c.rax==r->result&&crt_errno==r->error&&errno==2718&&win64_get_error()==r->last_error&&c.mxcsr==r->expected_csr;
    c.rax=b.rax;if(r->flags&8u)b.rsp+=8;okay=okay&&!memcmp(&c,&b,sizeof(c))&&!memcmp(output,wanted,sizeof(output))&&!memcmp(old,after,sizeof(old));
    if(!okay)fprintf(stderr,"STRTOL_MISMATCH api=%s text=%s base=%d expected=%llx error=%d end=%d\n",api,r->text,r->base,(unsigned long long)r->result,r->error,r->end);
    win64_region_remove((uintptr_t)output);return okay;
}
typedef struct{unsigned count,fail;} worker_state;
static void worker_body(worker_state *s){uint64_t saved[2],mode[2];native_state(saved);
#if defined(__aarch64__)
    mode[0]=(saved[0]&~(UINT64_C(3)<<22))|(UINT64_C(2)<<22)|(UINT64_C(1)<<24);mode[1]=0x95;
#else
    win64_region_add((uintptr_t)rows,(size_t)row_count*sizeof(row),0);
    mode[0]=0xffd5;mode[1]=0;
#endif
    native_set(mode);for(unsigned i=0;i<2048;++i){if(!one(&rows[(i*37+s->count)%row_count]))s->fail=1;++s->count;}native_set(saved);
#if defined(_WIN32)
    win64_region_remove((uintptr_t)rows);
#endif
}
#if defined(_WIN32)
static DWORD WINAPI worker(void *p){worker_body(p);return 0;}
#else
static void *worker(void *p){worker_body(p);return NULL;}
#endif
int main(int argc,char **argv){
    if(argc!=2)return 2;
    if(argv[1][0]=='-'){
        char text[8]="123";unsigned char output[8];CPU c={0};c.rcx=(uintptr_t)text;c.rdx=(uintptr_t)output;c.r8=10;
        win64_region_add((uintptr_t)text,sizeof(text),0);win64_region_add((uintptr_t)output,8,1);
        if(!strcmp(argv[1],"--null"))c.rcx=0;
        else if(!strcmp(argv[1],"--base1"))c.r8=1;
        else if(!strcmp(argv[1],"--base-negative"))c.r8=UINT32_MAX;
        else if(!strcmp(argv[1],"--base37"))c.r8=37;
        else if(!strcmp(argv[1],"--input"))c.rcx=1;
        else if(!strcmp(argv[1],"--output"))c.rdx=1;
        else if(!strcmp(argv[1],"--readonly-output")){win64_region_remove((uintptr_t)output);win64_region_add((uintptr_t)output,8,0);}
        else if(!strcmp(argv[1],"--short-output")){win64_region_remove((uintptr_t)output);win64_region_add((uintptr_t)output,7,1);}
        else if(!strcmp(argv[1],"--unterminated"))memset(text,'1',sizeof(text));
        else return 2;win64_utility_call(&c,"strtol");return 99;
    }
    FILE *f=fopen(argv[1],"rb");need(f!=NULL,"dataset open");need(!fseek(f,0,SEEK_END),"dataset seek");long size=ftell(f);rewind(f);need(size>0&&size%sizeof(row)==0,"dataset geometry");
    row_count=(uint32_t)(size/sizeof(row));rows=malloc((size_t)size);need(rows!=NULL&&fread(rows,(size_t)size,1,f)==1,"dataset load");fclose(f);win64_region_add((uintptr_t)rows,(size_t)size,0);
    uint64_t saved[2],mode[2];native_state(saved);
#if defined(__aarch64__)
    mode[0]=(saved[0]&~(UINT64_C(3)<<22))|(UINT64_C(1)<<22)|(UINT64_C(1)<<24);mode[1]=0x9f;
#else
    mode[0]=0xdfd5;mode[1]=0;
#endif
    native_set(mode);unsigned signed_count=0,unsigned_count=0;for(unsigned i=0;i<row_count;++i){need(one(&rows[i]),"native fullRAX/endptr/errno/CPU/FP");if(rows[i].flags&1u)++unsigned_count;else ++signed_count;}native_set(saved);
    worker_state state[4]={{0}};
#if defined(_WIN32)
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,&state[i],0,NULL);need(threads[i]!=NULL,"thread create");}
    for(unsigned i=0;i<4;++i){need(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0,"thread join");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)need(!pthread_create(&threads[i],NULL,worker,&state[i]),"thread create");for(unsigned i=0;i<4;++i)need(!pthread_join(threads[i],NULL),"thread join");
#endif
    for(unsigned i=0;i<4;++i)need(!state[i].fail&&state[i].count==2048,"parallel TLS state");
    CPU c={0},before=c;need(!win64_utility_call(&c,"strtoll")&&!memcmp(&c,&before,sizeof(c)),"unsupported64bit name unchanged");
    printf("{\"success\":true,\"cases\":%u,\"strtol_cases\":%u,\"strtoul_regression_cases\":%u,\"concurrent_cases\":8192,\"LONG32_fullRAX\":true,\"fullCPU_MXCSR_nativeFP_errno_LastError\":true,\"endptr_unaligned_canaries_NULL\":true,\"gameplay_proven\":false}\n",row_count,signed_count,unsigned_count);win64_region_remove((uintptr_t)rows);free(rows);return 0;
}
