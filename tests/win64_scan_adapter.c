#include "win64_scan.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
typedef struct{uint64_t p;size_t n;int write;} region;
static region regions[512];static unsigned nregions;static int guest_errno;
void win64_region_add(uint64_t p,size_t n,int write){if(nregions==512)abort();regions[nregions++]=(region){p,n,write};}
void win64_region_remove(uint64_t p){for(unsigned i=0;i<nregions;++i)if(regions[i].p==p){regions[i]=regions[--nregions];return;}abort();}
void *win64_pointer(uint64_t p,size_t n,int write){for(unsigned i=0;i<nregions;++i)if(p>=regions[i].p&&p-regions[i].p<=regions[i].n&&n<=regions[i].n-(p-regions[i].p)&&(!write||regions[i].write))return(void*)(uintptr_t)p;fprintf(stderr,"scan fixture invalid range %llx+%zu write%d\n",(unsigned long long)p,n,write);abort();}
const char *win64_string_a(uint64_t p){for(unsigned i=0;i<65536;++i)if(!*(char*)win64_pointer(p+i,1,0))return(const char*)(uintptr_t)p;abort();}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t r[]={c->rcx,c->rdx,c->r8,c->r9};return r[i];}uint64_t v;memcpy(&v,win64_pointer(c->rsp+40+(i-4)*8,8,0),8);return v;}
void win64_stdio_set_errno(int value){guest_errno=value;}
EXPORT int scan_errno(void){return guest_errno;}
EXPORT void scan_add(void*p,size_t n,int write){win64_region_add((uintptr_t)p,n,write);}
EXPORT void scan_remove(void*p){win64_region_remove((uintptr_t)p);}
EXPORT void scan_selftest(void){win64_scan_self_test();}
EXPORT int scan_run(uint64_t options,const char*input,uint64_t count,const char*format,uint64_t locale,uint64_t*va){
    uint64_t stack[8]={0};CPU c={0};c.rcx=options;c.rdx=(uintptr_t)input;c.r8=count;c.r9=(uintptr_t)format;c.rsp=(uintptr_t)stack;stack[5]=locale;stack[6]=(uintptr_t)va;guest_errno=0;
    win64_region_add((uintptr_t)stack,sizeof(stack),1);if(!win64_scan_call(&c,"__stdio_common_vsscanf")||c.rsp!=(uintptr_t)stack)abort();win64_region_remove((uintptr_t)stack);return(int32_t)c.rax;
}
#if defined(SCAN_FIXTURE_MAIN)
#include "scan_cases.h"
int main(void){win64_scan_self_test();unsigned failures=0;for(unsigned i=0;i<SCAN_CASE_COUNT;++i){const scan_case*t=&scan_cases[i];unsigned char output[16][64];memset(output,0xA5,sizeof(output));uint64_t va[32]={0};
    for(unsigned a=0;a<t->argc;++a)va[a]=t->kind[a]?t->value[a]:(uintptr_t)output[t->value[a]];
    win64_region_add((uintptr_t)t->input,t->input_bytes,0);win64_region_add((uintptr_t)t->format,strlen(t->format)+1,0);win64_region_add((uintptr_t)va,sizeof(va),0);win64_region_add((uintptr_t)output,sizeof(output),1);
    int result=scan_run(t->options,t->input,t->count,t->format,0,va);if(result!=t->expected_result||guest_errno!=t->expected_errno||memcmp(output,t->expected,sizeof(output))){fprintf(stderr,"SCAN_CASE_FAIL %u result%d expected%d errno%d expected%d format=%s\n",i,result,t->expected_result,guest_errno,t->expected_errno,t->format);++failures;}
    win64_region_remove((uintptr_t)output);win64_region_remove((uintptr_t)va);win64_region_remove((uintptr_t)t->format);win64_region_remove((uintptr_t)t->input);
    }
    printf("{\"native_UCRT_cases\":%u,\"failures\":%u,\"success\":%s,\"game_boot_proven\":false}\n",SCAN_CASE_COUNT,failures,failures?"false":"true");return failures?1:0;}
#endif
