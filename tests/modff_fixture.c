#include "win64_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#define LOCAL __declspec(thread)
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#define LOCAL _Thread_local
#endif
static LOCAL int guest_errno;
static LOCAL uint64_t handler;
static LOCAL unsigned char *allowed;
static LOCAL size_t allowed_bytes;
void win64_stdio_set_errno(int value){guest_errno=value;}
uint64_t win64_math_error_handler(void){return handler;}
void *win64_pointer(uint64_t address,size_t bytes,int writable){
    uintptr_t p=(uintptr_t)address,b=(uintptr_t)allowed;
    if(!writable||bytes!=4||!b||p<b||p-b>allowed_bytes||bytes>allowed_bytes-(p-b)){
        fprintf(stderr,"STOP Win64 pointer invalid modff output span\n");fflush(stderr);_Exit(73);
    }
    errno=999;return(void*)p; /* Successful validation may disturb native errno. */
}
typedef struct{
    uint32_t input,mxcsr,result,integral,expected_mxcsr;
    int32_t expected_errno;
    uint32_t matherr_calls,last_error;
}row;
_Static_assert(sizeof(row)==32,"modff fixture row32");
static row *rows;static size_t count;
static _Atomic unsigned failures;
static _Atomic unsigned long exact_calls;
static int import_math(CPU*c,const char*dll,const char*name){int crt=!strcmp(dll,"api-ms-win-crt-math-l1-1-0.dll");
#include "modff_route.inc"
    return 0;
}
static void environment(uint64_t out[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(out[0]),"=r"(out[1]));
#else
    out[0]=_mm_getcsr();out[1]=0;
#endif
}
static void set_environment(const uint64_t value[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(value[0]),"r"(value[1]):"memory");
#else
    _mm_setcsr((unsigned)value[0]);
#endif
}
static int one(const row*r,size_t index){
    unsigned char storage[32],expected_storage[32];memset(storage,0x5d,sizeof(storage));
    unsigned offset=8u+(unsigned)(index&3u);allowed=storage+offset;allowed_bytes=4;
    memcpy(expected_storage,storage,sizeof(storage));memcpy(expected_storage+offset,&r->integral,4);
    CPU c,expected;memset(&c,0xa7,sizeof(c));c.mxcsr=r->mxcsr;c.xmm[0].u32[0]=r->input;c.rdx=(uintptr_t)allowed;
    expected=c;expected.mxcsr=r->expected_mxcsr;expected.xmm[0].u32[0]=r->result;if(index&1)expected.rsp+=8;
    guest_errno=1379;errno=2718;uint64_t before[2],after[2];environment(before);
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=index&1?import_math(&c,"api-ms-win-crt-math-l1-1-0.dll","modff"):win64_math_call(&c,"modff");
    environment(after);int native_errno=errno;
    int okay=handled&&!memcmp(&c,&expected,sizeof(c))&&!memcmp(storage,expected_storage,sizeof(storage))&&guest_errno==r->expected_errno&&native_errno==2718&&!memcmp(before,after,sizeof(before));
#if defined(_WIN32)
    okay=okay&&GetLastError()==r->last_error;
#endif
    if(!okay&&atomic_fetch_add(&failures,1)<16)fprintf(stderr,"MODFF_FAIL input=%08x value=%08x/%08x integral=%08x CSR=%x/%x errno=%d/%d host_errno=%d CPU=%d memory=%d nativeFP=%d\n",r->input,c.xmm[0].u32[0],r->result,r->integral,c.mxcsr,r->expected_mxcsr,guest_errno,r->expected_errno,native_errno,memcmp(&c,&expected,sizeof(c)),memcmp(storage,expected_storage,sizeof(storage)),memcmp(before,after,sizeof(before)));
    if(okay)atomic_fetch_add(&exact_calls,1);allowed=NULL;allowed_bytes=0;return okay;
}
#if !defined(_WIN32)
static void *worker(void*context){
    size_t seed=(size_t)(uintptr_t)context;uint64_t saved[2],wanted[2];environment(saved);
    wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((seed&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;set_environment(wanted);
    for(size_t i=0;i<2048;++i)if(!one(&rows[(seed*997+i*8191)%count],i)){set_environment(saved);return(void*)1;}
    set_environment(saved);return NULL;
}
#endif
static unsigned guard_cases(void){
#if defined(_WIN32)
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    unsigned char *p=VirtualAlloc(NULL,2*page,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);DWORD old;
    if(!p||!VirtualProtect(p+page,page,PAGE_NOACCESS,&old))return 0;
#else
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char *p=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED||mprotect(p+page,page,PROT_NONE))return 0;
#endif
    allowed=p+page-4;allowed_bytes=4;unsigned checks=0;
    for(unsigned i=0;i<8;++i){
        CPU c={0};c.mxcsr=0x1f80;c.rdx=(uintptr_t)allowed;c.xmm[0].u32[0]=i&1?0xbfc00000:0x3fc00000;
        memset(allowed-4,0xa9,8);if(!win64_math_call(&c,"modff"))break;
        uint32_t integral;memcpy(&integral,allowed,4);
        if(integral!=(i&1?0xbf800000:0x3f800000)||c.xmm[0].u32[0]!=(i&1?0xbf000000:0x3f000000))break;
        if(memcmp(allowed-4,"\xa9\xa9\xa9\xa9",4))break;++checks;
    }
#if defined(_WIN32)
    VirtualFree(p,0,MEM_RELEASE);
#else
    munmap(p,2*page);
#endif
    allowed=NULL;allowed_bytes=0;return checks;
}
int main(int argc,char **argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"--negative")){
        if(argc!=3)return 2;CPU c={0};unsigned char buffer[8];allowed=buffer;allowed_bytes=4;
        c.mxcsr=0x1f80;c.rip=0x14189fd4b;c.rdx=(uintptr_t)buffer;c.xmm[0].u32[0]=0x3fc00000;
        if(!strcmp(argv[2],"reserved"))c.mxcsr|=0x10000;
        else if(!strcmp(argv[2],"unmasked_invalid")){c.xmm[0].u32[0]=0x7f800001;c.mxcsr&=~128u;}
        else if(!strcmp(argv[2],"null_output"))c.rdx=0;
        else if(!strcmp(argv[2],"short_output"))allowed_bytes=3;
        else if(!strcmp(argv[2],"unregistered_output"))allowed=NULL;
        else if(!strcmp(argv[2],"out_of_range_output"))c.rdx+=4;
        else return 2;win64_math_call(&c,"modff");return 99;
    }
    win64_math_self_test();FILE*f=fopen(argv[1],"rb");if(!f)return 3;fseek(f,0,SEEK_END);long bytes=ftell(f);rewind(f);
    if(bytes<=0||bytes%sizeof(row))return 4;count=(size_t)bytes/sizeof(row);rows=malloc((size_t)bytes);
    if(!rows||fread(rows,(size_t)bytes,1,f)!=1)return 5;fclose(f);
    uint64_t saved[2],wanted[2];environment(saved);
    for(size_t i=0;i<count;++i){
#if defined(__aarch64__)
        wanted[0]=(saved[0]&~(UINT64_C(3)<<22))|((i&3u)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
        wanted[0]=UINT32_C(0x9fd5)|((i&3u)<<13);wanted[1]=0;
#endif
        set_environment(wanted);if(!one(&rows[i],i)&&atomic_load(&failures)>=16)break;
    }
    set_environment(saved);unsigned concurrent=0;
#if !defined(_WIN32)
    if(!atomic_load(&failures)){
        pthread_t threads[4];for(uintptr_t i=0;i<4;++i)if(pthread_create(&threads[i],NULL,worker,(void*)i))return 6;
        for(unsigned i=0;i<4;++i){void*result;if(pthread_join(threads[i],&result)||result)atomic_fetch_add(&failures,1);}concurrent=8192;
    }
#endif
    unsigned guarded=guard_cases();if(guarded!=8)atomic_fetch_add(&failures,1);
    CPU c;memset(&c,0xa5,sizeof(c));CPU old=c;
    if(win64_math_call(&c,"not_modff")||memcmp(&c,&old,sizeof(c)))atomic_fetch_add(&failures,1);
    if(import_math(&c,"unrelated.dll","modff")||memcmp(&c,&old,sizeof(c)))atomic_fetch_add(&failures,1);
    printf("{\"success\":%s,\"cases\":%zu,\"bit_exact_calls\":%lu,\"failures\":%u,\"concurrent_cases\":%u,\"guarded_output_cases\":%u,\"fullCPU_MXCSR_errno_nativeFP\":true,\"single_import_pop\":true}\n",atomic_load(&failures)?"false":"true",count,atomic_load(&exact_calls),atomic_load(&failures),concurrent,guarded);
    free(rows);return atomic_load(&failures)?1:0;
}
