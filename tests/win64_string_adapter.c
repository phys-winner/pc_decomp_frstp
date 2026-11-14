#include "win64_string.h"
#include "win64.h"
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t p;size_t n;int writing; } test_region;
static test_region regions[64];static unsigned region_count;
static unsigned codepage;static int current_errno;static int fail_allocation;
void win64_region_add(uint64_t p,size_t n,int writing) { if(region_count==64)abort();regions[region_count++]=(test_region){p,n,writing}; }
void win64_region_remove(uint64_t p) { for(unsigned i=0;i<region_count;++i)if(regions[i].p==p){regions[i]=regions[--region_count];return;}abort(); }
void *win64_pointer(uint64_t p,size_t n,int writing) {
    for(unsigned i=0;i<region_count;++i)if(p>=regions[i].p && p-regions[i].p<=regions[i].n &&
        n<=regions[i].n-(size_t)(p-regions[i].p) && (!writing||regions[i].writing))return(void*)(uintptr_t)p;
    abort();
}
__declspec(dllexport) void strings_reset(void) { region_count=0; }
__declspec(dllexport) void strings_add(void *p,size_t n,int writing) { win64_region_add((uintptr_t)p,n,writing); }
__declspec(dllexport) void strings_selftest(void) { win64_string_self_test(); }
uint64_t win64_alloc(size_t n,int zero) { if(fail_allocation)return 0;void *p=zero?calloc(1,n):malloc(n);if(p)win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p; }
void win64_free(uint64_t p) { if(p){win64_region_remove(p);free((void*)(uintptr_t)p);} }
void win64_stdio_set_errno(int value) { current_errno=value; }
int win64_locale_call(CPU *cpu,const char *name) { if(strcmp(name,"___lc_codepage_func"))abort();cpu->rax=codepage;return 1; }
__declspec(dllexport) void strings_codepage(unsigned value) { codepage=value; }
__declspec(dllexport) void strings_release(uint64_t p) { win64_free(p); }
__declspec(dllexport) int strings_errno(void) { return current_errno; }
__declspec(dllexport) void strings_fail_alloc(int value) { fail_allocation=value;current_errno=0; }
__declspec(dllexport) uint64_t strings_call(const char *name,void *dest,size_t size,void *source,size_t count) {
    CPU c={0};c.rsp=0x12345678;c.rcx=(uintptr_t)dest;c.rdx=size;c.r8=(uintptr_t)source;c.r9=count;
    if(!win64_string_call(&c,name)||c.rsp!=0x12345678)abort();return c.rax;
}
__declspec(dllexport) uint64_t strings_args(const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e) {
    CPU c={0};c.rsp=0x12345678;c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;
    if(!win64_string_call(&c,name)||c.rsp!=0x12345678)abort();return c.rax;
}
