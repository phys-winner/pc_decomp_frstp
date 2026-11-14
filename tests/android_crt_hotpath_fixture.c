/* Exact production router + stdio + string providers. Other providers decline
 * without side effects. Region TLS itself has its independent production oracle. */
#include "win64.h"
#include "win64_stdio.h"
#include "win64_string.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <time.h>
#endif

typedef struct {uint64_t p;size_t n;int write;} Region;
static Region regions[128];static unsigned region_count,registrations;
static unsigned declines,stdio_calls,string_calls,metadata_calls;
static uint32_t last_error;static int guest_errno;
static unsigned char destination[256],source[256];
static CPU *active_cpu;static uint64_t original_rsp;
static void check(int yes,const char *why){if(!yes){fprintf(stderr,"CRT_HOTPATH_FAIL %s\n",why);exit(1);}}
void win64_region_add(uint64_t p,size_t n,int write){check(region_count<128,"region capacity");regions[region_count++]=(Region){p,n,write};++registrations;}
void win64_region_remove(uint64_t p){for(unsigned i=region_count;i;--i)if(regions[i-1].p==p){regions[i-1]=regions[--region_count];return;}abort();}
static int allowed(uint64_t p,size_t n,int write){
    if(!n)return 1;
    for(unsigned i=region_count;i;--i){Region r=regions[i-1];if(p>=r.p&&p-r.p<r.n)return n<=r.n-(size_t)(p-r.p)&&(!write||r.write);}
    return 0;
}
void *win64_pointer(uint64_t p,size_t n,int write){
    if(!allowed(p,n,write)){
        check(!active_cpu||active_cpu->rsp==original_rsp,"fault must precede RSP pop");
        fprintf(stderr,"CRT_HOTPATH_POINTER_REJECT bytes=%zu writing=%d registrations=%u RSP_unpopped=1\n",n,write,registrations);exit(73);
    }
    return(void*)(uintptr_t)p;
}
const char *win64_string_a(uint64_t p){for(unsigned i=0;i<32768;++i)if(!*(char*)win64_pointer(p+i,1,0))return(const char*)(uintptr_t)p;abort();}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t a[]={c->rcx,c->rdx,c->r8,c->r9};return a[i];}uint64_t v;memcpy(&v,win64_pointer(c->rsp+40+(i-4)*8,8,0),8);return v;}
uint64_t win64_alloc(size_t n,int zero){void*p=zero?calloc(1,n):malloc(n);if(p)win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p;}
void win64_free(uint64_t p){win64_region_remove(p);free((void*)(uintptr_t)p);}
void win64_stdio_set_errno(int n){guest_errno=n;}
int win64_locale_call(CPU*c,const char*n){if(strcmp(n,"___lc_codepage_func"))return 0;c->rax=0;return 1;}
int win64_kernel_stream_open(uint64_t p,int wide,int writing,int*fd,void**token){(void)p;(void)wide;(void)writing;(void)fd;(void)token;abort();}
void win64_kernel_stream_close(void*token){(void)token;abort();}
int baseline_stdio_call(CPU*,const char*);
static int old_stdio(CPU*c,const char*n){++stdio_calls;return baseline_stdio_call(c,n);}
static int new_stdio(CPU*c,const char*n){++stdio_calls;return win64_stdio_call(c,n);}
static int real_string(CPU*c,const char*n){++string_calls;return win64_string_call(c,n);}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;++declines;return 0;}
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;++declines;return 0;}
static int unrelated_notification(CPU*c){(void)c;++declines;return 0;}
static void metadata_lock(void){++metadata_calls;}
static void metadata_unlock(void){++metadata_calls;}
void win64_region_mutation_begin(void){++metadata_calls;}
void win64_region_mutation_end(void){++metadata_calls;}
#include "crt_routers.inc"

typedef struct {CPU cpu;unsigned char bytes[256];int native_errno,crt_errno;uint32_t error;unsigned registration_count,negative_providers;} Result;
static uint64_t checks;
static Result invoke(int candidate,const char*dll,const char*name,CPU initial){
    declines=stdio_calls=string_calls=metadata_calls=0;registrations=0;errno=E2BIG;guest_errno=31337;last_error=0x51cafe;
    active_cpu=&initial;original_rsp=initial.rsp;
    int yes=candidate?candidate_route(&initial,dll,name):baseline_route(&initial,dll,name);
    active_cpu=NULL;
    check(yes==1,"known name handled");check(initial.rsp==original_rsp+8,"exactly one RSP pop");
    Result result={.cpu=initial,.native_errno=errno,.crt_errno=guest_errno,.error=last_error,
        .registration_count=registrations,.negative_providers=declines};memcpy(result.bytes,destination,sizeof(destination));
    check(stdio_calls==1,"stdio init remains in route");check(!metadata_calls,"no unrelated metadata mutation");++checks;return result;
}
static CPU arguments(const char*name,size_t n){
    CPU c;memset(&c,0xa5,sizeof(c));c.rsp=UINT64_C(0x123456789abcd000);c.rcx=(uintptr_t)destination;
    if(!strcmp(name,"strcat_s")||!strcmp(name,"strcpy_s")){c.rdx=n;c.r8=(uintptr_t)source;}
    else {c.rdx=(uintptr_t)source;c.r8=n;}
    return c;
}
static void pair(const char*name,size_t n,unsigned mode,int cold){
    unsigned char before[256];memset(destination,0x5a,sizeof(destination));destination[0]=0;
    if(mode&1)memcpy(destination,"Ice ",5);
    if(!strcmp(name,"strncmp")){memcpy(destination,source,sizeof(destination));if(mode&2)destination[3]^=1;}
    memcpy(before,destination,sizeof(before));CPU c=arguments(name,n);
    Result a=invoke(0,"api-ms-win-crt-string-l1-1-0.dll",name,c);memcpy(destination,before,sizeof(before));
    Result b=invoke(1,"api-ms-win-crt-string-l1-1-0.dll",name,c);
    check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&!memcmp(a.bytes,b.bytes,sizeof(a.bytes)),"baseline whole CPU/memory");
    check(a.native_errno==b.native_errno&&a.crt_errno==b.crt_errno&&a.error==b.error,"native/guest error state");
    CPU expected=c;expected.rsp+=8;expected.rax=b.cpu.rax;check(!memcmp(&expected,&b.cpu,sizeof(CPU)),"only RAX and RSP change");
    if(cold)check(a.registration_count==4&&b.registration_count==4,"cold four FILE registrations per provider");
    if(name[0]=='s')check(b.negative_providers==0&&a.negative_providers>0,"hot string skips pure-decline providers");
}
static void aliases(void){
    for(unsigned variant=0;variant<2;++variant){
        CPU c={0};c.rcx=1;check((variant?win64_stdio_call(&c,"__acrt_iob_func"):baseline_stdio_call(&c,"__acrt_iob_func"))==1,"iob alias");
        check(allowed(c.rax,40,0)&&!allowed(c.rax,1,1),"FILE header stays readonly");
        check(allowed(c.rax+16,24,1)&&!allowed(c.rax+15,2,1),"FILE writable field aliases");
        uint64_t value=UINT64_C(0x123456789abcdef0);memcpy(win64_pointer(c.rax+16,8,1),&value,8);
        check(*(uint64_t*)win64_pointer(c.rax+16,8,0)==value,"persistent writable FILE field");
        CPU copy;memset(&copy,0xa5,sizeof(copy));copy.rsp=UINT64_C(0x123456789abcd000);copy.rcx=c.rax+16;copy.rdx=(uintptr_t)source;copy.r8=8;
        Result routed=invoke(variant,"ucrtbase.dll","memcpy",copy);CPU expected=copy;expected.rax=copy.rcx;expected.rsp+=8;
        check(!memcmp(&routed.cpu,&expected,sizeof(copy))&&!memcmp(win64_pointer(copy.rcx,8,0),source,8),"routed copy into FILE writable alias");
    }
}
static uint64_t nanos(void){
#ifdef _WIN32
    LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return(uint64_t)((double)t.QuadPart*1e9/f.QuadPart);
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;
#endif
}
int main(int argc,char**argv){
    win64_region_add((uintptr_t)destination,sizeof(destination),1);win64_region_add((uintptr_t)source,sizeof(source),1);
    memcpy(source,"Frostpunk shader family",23);source[23]=0;
    if(argc==3&&!strcmp(argv[1],"--cold")){
        pair(argv[2],128,0,1);aliases();printf("{\"success\":true,\"cold_name\":\"%s\",\"FILE_registrations_each\":4,\"readonly_and_writable_aliases\":true}\n",argv[2]);return 0;
    }
    if(argc==4&&!strcmp(argv[1],"--negative")){
        int candidate=!strcmp(argv[2],"candidate");const char*name=argv[3];CPU c=arguments(name,8);
        if(!strcmp(name,"strcat_s")){memset(destination,'X',8);regions[0].n=8;}
        else if(!strcmp(name,"strcpy_s")){memset(source,'X',8);regions[1].n=8;c.rdx=128;}
        else {memset(destination,'X',8);memset(source,'X',8);regions[0].n=regions[1].n=8;c.r8=9;}
        (void)invoke(candidate,"api-ms-win-crt-string-l1-1-0.dll",name,c);return 99;
    }
    pair("strcat_s",128,0,1);aliases();
    const char*declining[]={"memcpy","memmove","strcat_s","strcpy_s","strncmp","not_stdio"};
    for(unsigned i=0;i<6;++i)for(unsigned variant=0;variant<2;++variant){CPU c;memset(&c,0xa5,sizeof(c));CPU before=c;
        int yes=variant?win64_stdio_call(&c,declining[i]):baseline_stdio_call(&c,declining[i]);
        check(!yes&&!memcmp(&c,&before,sizeof(c)),"stdio declines with whole CPU/RSP unchanged");}
    for(unsigned n=0;n<=64;++n){memset(source,'A',sizeof(source));source[n]=0;
        pair("strcpy_s",n+1,0,0);pair("strcat_s",n+5,1,0);
        for(unsigned count=0;count<=n+2;++count)pair("strncmp",count,count&3,0);
        pair("memcpy",n,0,0);pair("memmove",n,0,0);
    }
    /* Count zero must not dereference either invalid source pointer. */
    CPU c=arguments("strncmp",0);c.rcx=UINT64_MAX;c.rdx=1;
    Result a=invoke(0,"ucrtbase.dll","strncmp",c),b=invoke(1,"ucrtbase.dll","strncmp",c);
    check(!a.cpu.rax&&!memcmp(&a.cpu,&b.cpu,sizeof(CPU)),"count-zero invalid pointer semantics");
    /* An early NUL/mismatch before a logical unreadable tail must succeed. */
    regions[0].n=regions[1].n=1;destination[0]=source[0]=0;
    a=invoke(0,"ucrtbase.dll","strncmp",arguments("strncmp",999));b=invoke(1,"ucrtbase.dll","strncmp",arguments("strncmp",999));
    check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&!b.cpu.rax,"NUL before unreadable tail");
    destination[0]='A';source[0]='B';a=invoke(0,"ucrtbase.dll","strncmp",arguments("strncmp",999));b=invoke(1,"ucrtbase.dll","strncmp",arguments("strncmp",999));
    check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&(int32_t)b.cpu.rax<0,"mismatch before unreadable tail");
    regions[0].n=regions[1].n=sizeof(destination);
    const char*copies[]={"memcpy","memmove"};
    for(unsigned i=0;i<2;++i){unsigned char original[256];for(unsigned n=0;n<256;++n)destination[n]=(unsigned char)n;memcpy(original,destination,256);
        CPU c=arguments(copies[i],61);c.rcx=(uintptr_t)destination+3;c.rdx=(uintptr_t)destination;
        Result a=invoke(0,"vcruntime140.dll",copies[i],c);memcpy(destination,original,256);Result b=invoke(1,"vcruntime140.dll",copies[i],c);
        check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&!memcmp(a.bytes,b.bytes,256),"existing overlapping copy semantics");}
    /* Unknown CRT names and these same names under a non-CRT DLL decline. */
    for(unsigned i=0;i<2;++i)for(unsigned variant=0;variant<2;++variant){
        CPU before=arguments("strncmp",0),p=before;const char*dll=i?"unrelated.dll":"ucrtbase.dll",*name=i?"strcpy_s":"not_a_CRT_name";
        int yes=variant?candidate_route(&p,dll,name):baseline_route(&p,dll,name);check(!yes&&!memcmp(&p,&before,sizeof(p)),"negative provider decline CPU/RSP");
    }
    memcpy(source,"Frostpunk shader family",23);source[23]=0;
    uint64_t elapsed[2];const unsigned iterations=100000;
    for(unsigned variant=0;variant<2;++variant){uint64_t began=nanos();
        for(unsigned i=0;i<iterations;++i){const char*name=i%3==0?"strcat_s":i%3==1?"strcpy_s":"strncmp";destination[0]=0;CPU p=arguments(name,128);
            int yes=variant?candidate_route(&p,"api-ms-win-crt-string-l1-1-0.dll",name):baseline_route(&p,"api-ms-win-crt-string-l1-1-0.dll",name);check(yes,"benchmark route");}
        elapsed[variant]=nanos()-began;
    }
    printf("{\"success\":true,\"paired_calls\":%" PRIu64 ",\"whole_CPU_memory_errors\":true,\"single_RSP_pop\":true,\"first_FILE_registration\":true,\"negative_provider_decline\":true,\"benchmark_calls_each\":%u,\"baseline_ns\":%" PRIu64 ",\"candidate_ns\":%" PRIu64 ",\"unrelated_providers_stubbed\":true,\"game_executed\":false}\n",checks,iterations,elapsed[0],elapsed[1]);return 0;
}
