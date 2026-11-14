#include "win64_stdio.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint64_t p;size_t n;int write;} region;
static region regions[1024];static unsigned nregions;static int guest_errno;
void win64_region_add(uint64_t p,size_t n,int write){if(nregions==1024)abort();regions[nregions++]=(region){p,n,write};}
void win64_region_remove(uint64_t p){for(unsigned i=0;i<nregions;++i)if(regions[i].p==p){regions[i]=regions[--nregions];return;}abort();}
void *win64_pointer(uint64_t p,size_t n,int write){for(unsigned i=0;i<nregions;++i)if(p>=regions[i].p&&p-regions[i].p<=regions[i].n&&n<=regions[i].n-(p-regions[i].p)&&(!write||regions[i].write))return(void*)(uintptr_t)p;fprintf(stderr,"bad pointer %llx size%zu write%d\n",(unsigned long long)p,n,write);abort();}
const char *win64_string_a(uint64_t p){for(unsigned i=0;i<16*1024*1024;++i)if(!*(char*)win64_pointer(p+i,1,0))return(const char*)(uintptr_t)p;abort();}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t r[]={c->rcx,c->rdx,c->r8,c->r9};return r[i];}return *(uint64_t*)win64_pointer(c->rsp+0x28+(i-4)*8,8,0);}
uint64_t win64_alloc(size_t n,int zero){void*p=zero?calloc(1,n):malloc(n);if(!p)abort();win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p;}
void win64_free(uint64_t p){win64_region_remove(p);free((void*)(uintptr_t)p);}
void win64_stdio_set_errno(int value){guest_errno=value;}
__declspec(dllexport) int stdio_errno(void){return guest_errno;}
__declspec(dllexport) void stdio_add(void*p,size_t n,int write){win64_region_add((uintptr_t)p,n,write);}
__declspec(dllexport) void stdio_remove(void*p){win64_region_remove((uintptr_t)p);}
__declspec(dllexport) void stdio_selftest(void){win64_stdio_self_test();}
__declspec(dllexport) int stdio_count(int n){CPU c={0};c.rcx=n;win64_stdio_call(&c,"_set_printf_count_output");return(int)c.rax;}
__declspec(dllexport) uint64_t stdio_stream(const char*name,uint64_t a,uint64_t b,uint64_t d,uint64_t e){
    CPU c={0};c.rsp=0x12345678;c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;
    if(!win64_stdio_call(&c,name)||c.rsp!=0x12345678)abort();return c.rax;
}
__declspec(dllexport) int stdio_run(const char*name,uint64_t options,void*buffer,size_t cap,size_t count,const char*fmt,void*va){
    uint64_t stack[32]={0};win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU c={0};c.rsp=(uintptr_t)stack;c.mxcsr=0x1f80;guest_errno=0;
    c.rcx=options;c.rdx=(uintptr_t)buffer;c.r8=cap;c.r9=(uintptr_t)fmt;stack[5]=0;stack[6]=(uintptr_t)va;
    if(!strcmp(name,"__stdio_common_vsnprintf_s")){c.r9=count;stack[5]=(uintptr_t)fmt;stack[6]=0;stack[7]=(uintptr_t)va;}
    if(!strcmp(name,"vsprintf")){c.rcx=(uintptr_t)buffer;c.rdx=(uintptr_t)fmt;c.r8=(uintptr_t)va;}
    if(!win64_stdio_call(&c,name))abort();if(c.rsp!=(uintptr_t)stack)abort();win64_region_remove((uintptr_t)stack);return(int)c.rax;
}
