/* Fixed private identity; real provider on ARM and exact extracted C on host. */
#include "win64_environment.h"
#include "win64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

typedef struct {uintptr_t base;size_t size;int write;} fixture_region;
static TLS fixture_region regions[4];static TLS unsigned region_count;
static TLS uint32_t last_error;static TLS int guest_errno;
void win64_set_error(uint32_t value){last_error=value;}
uint32_t win64_get_error(void){return last_error;}
static void check(int okay,const char *why){if(!okay){fprintf(stderr,"USERNAME_FIXTURE_FAIL %s\n",why);_Exit(1);}}
void *win64_pointer(uint64_t address,size_t size,int writing){
    for(unsigned i=0;i<region_count;++i){fixture_region *r=&regions[i];
        if(address>=r->base&&address-r->base<=r->size&&size<=r->size-(address-r->base)&&(!writing||r->write))return(void*)(uintptr_t)address;
    }
    fputs("STOP username fixture checked-buffer boundary\n",stderr);_Exit(73);
}
static void region(void *base,size_t size,int write){check(region_count<4,"region capacity");regions[region_count++]=(fixture_region){(uintptr_t)base,size,write};}
#if defined(_WIN32)
#include "username_helper.inc"
#endif
static int imported(CPU *c,const char *dll,const char *name){
#include "username_route.inc"
    return 0;
}
typedef struct {uint64_t control,status;} fp_state;
static fp_state fp_get(void){fp_state s;
#if defined(_WIN32)
    s.control=_mm_getcsr();s.status=0;
#else
    __asm__ volatile("mrs %0, fpcr":"=r"(s.control));__asm__ volatile("mrs %0, fpsr":"=r"(s.status));
#endif
    return s;
}
static unsigned calls;
static void invoke(unsigned *count,int route,const char *dll,void *output,void *size,uint32_t wanted_result,uint32_t wanted_error){
    CPU c,b;unsigned char *bytes=(unsigned char*)&c;for(unsigned i=0;i<sizeof(c);++i)bytes[i]=(unsigned char)(i*13+*count);
    c.rcx=(uintptr_t)output;c.rdx=(uintptr_t)size;c.rsp=UINT64_C(0x123456789abcdef0);b=c;
    last_error=0x1234;guest_errno=2718;errno=2345;fp_state before=fp_get();
    check(route?imported(&c,dll,"GetUserNameA"):win64_identity_call(&c,"GetUserNameA"),"handled");
    fp_state after=fp_get();check(before.control==after.control&&before.status==after.status,"native FP preserved");
    check(c.rax==wanted_result&&last_error==wanted_error,"BOOL zeroextended/LastError");
    check(errno==2345&&guest_errno==2718,"native+guest errno preserved");
    c.rax=b.rax;if(route)b.rsp+=8;check(!memcmp(&c,&b,sizeof(c)),"wholeCPU and single route pop");++*count;
}
static void one(unsigned *count,unsigned capacity,int route,int null_buffer,unsigned alignment){
    unsigned char actual[48],expected[48],size_bytes[12],size_expected[12];memset(actual,0xa5,sizeof(actual));memcpy(expected,actual,sizeof(actual));
    memset(size_bytes,0x5c,sizeof(size_bytes));memcpy(size_bytes+alignment,&capacity,4);memcpy(size_expected,size_bytes,sizeof(size_bytes));
    uint32_t required=capacity<8?16:8;memcpy(size_expected+alignment,&required,4);
    if(capacity>=8)memcpy(expected+8,"Android",8);
    region_count=0;region(actual+8,8,1);region(size_bytes+alignment,4,1);
    invoke(count,route,route&1?"aDvApI32.dLl":"ADVAPI32.DLL",null_buffer?NULL:actual+8,size_bytes+alignment,capacity>=8,capacity>=8?0x1234:122);
    check(!memcmp(actual,expected,sizeof(actual))&&!memcmp(size_bytes,size_expected,sizeof(size_bytes)),"entire outputs/canaries/write size");
}
static void overlap(unsigned *count,int offset,unsigned capacity){
    unsigned char actual[96],expected[96];memset(actual,0xa5,sizeof(actual));memcpy(actual+32+offset,&capacity,4);memcpy(expected,actual,sizeof(actual));
    uint32_t required=capacity<8?16:8;if(capacity>=8)memcpy(expected+32,"Android",8);memcpy(expected+32+offset,&required,4);
    region_count=0;region(actual,sizeof(actual),1);
    invoke(count,offset&1,"advapi32.dll",actual+32,actual+32+offset,capacity>=8,capacity>=8?0x1234:122);
    check(!memcmp(actual,expected,sizeof(actual)),"alias name-before-count native order");
}
static void worker_body(unsigned *count){for(unsigned i=0;i<2048;++i)one(count,i%259,i&1,0,i&3);}
#if defined(_WIN32)
static DWORD WINAPI worker(void *arg){worker_body(arg);return 0;}
#else
static void *worker(void *arg){worker_body(arg);return NULL;}
#endif
int main(int argc,char **argv){
    if(argc>1){unsigned char name[8],size[4];uint32_t capacity=8;memcpy(size,&capacity,4);CPU c={0};c.rcx=(uintptr_t)name;c.rdx=(uintptr_t)size;
        region_count=0;
        if(!strcmp(argv[1],"--short-output")){region(name,7,1);region(size,4,1);}
        else if(!strcmp(argv[1],"--readonly-output")){region(name,8,0);region(size,4,1);}
        else if(!strcmp(argv[1],"--null-output")){c.rcx=0;region(size,4,1);}
        else if(!strcmp(argv[1],"--short-size")){region(name,8,1);region(size,3,1);}
        else if(!strcmp(argv[1],"--readonly-size")){region(name,8,1);region(size,4,0);}
        else if(!strcmp(argv[1],"--null-size")){c.rdx=0;region(name,8,1);}
        else return 2;
        win64_identity_call(&c,"GetUserNameA");return 99;
    }
    for(unsigned capacity=0;capacity<=512;++capacity)for(unsigned alignment=0;alignment<4;++alignment)one(&calls,capacity,capacity&1,0,alignment);
    for(unsigned capacity=0;capacity<8;++capacity)one(&calls,capacity,1,1,0);
    one(&calls,UINT32_MAX,1,0,3);
    for(int offset=-12;offset<=12;++offset)for(unsigned capacity=0;capacity<10;++capacity)overlap(&calls,offset,capacity);
    const char *dlls[]={"KERNEL32.dll","kernelbase.dll","ADVAPI32.dll.bad","user32.dll"};
    const char *names[]={"GetUserNameW","getusernamea","GetUserNameA_bad","GetUserName"};
    CPU c,b;memset(&c,0xa5,sizeof(c));b=c;last_error=27;errno=2345;
    for(unsigned i=0;i<4;++i){check(!imported(&c,dlls[i],"GetUserNameA")&&!memcmp(&c,&b,sizeof(c))&&last_error==27&&errno==2345,"exact DLL namespace");check(!win64_identity_call(&c,names[i])&&!memcmp(&c,&b,sizeof(c)),"exact export decline");}
    unsigned counts[4]={0};
#if defined(_WIN32)
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,worker,&counts[i],0,NULL);check(threads[i]!=NULL,"thread create");}
    for(unsigned i=0;i<4;++i){check(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0,"thread join");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)check(!pthread_create(&threads[i],NULL,worker,&counts[i]),"thread create");
    for(unsigned i=0;i<4;++i)check(!pthread_join(threads[i],NULL),"thread join");
#endif
    unsigned parallel=0;for(unsigned i=0;i<4;++i)parallel+=counts[i];
    printf("{\"success\":true,\"calls\":%u,\"parallel_calls\":%u,\"overlap_cases\":250,\"namespace_cases\":8,\"identity\":\"Android\",\"short_required\":16,\"success_required\":8,\"fullCPU\":true,\"LastError_errno_nativeFP\":true,\"gameplay_proven\":false}\n",calls+parallel,parallel);return 0;
}
