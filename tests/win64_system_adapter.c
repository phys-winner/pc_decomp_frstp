/* Standalone ARM64 development probe, not a game loader. */
#include "win64_system.h"
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint64_t address;size_t bytes;int writable; } region;
static region regions[32];static unsigned region_count;static uint32_t last_error;
void win64_region_add(uint64_t address,size_t bytes,int writable) {
    if(region_count==32)abort();regions[region_count++]=(region){address,bytes,writable};
}
void win64_region_remove(uint64_t address) {
    for(unsigned i=0;i<region_count;++i)if(regions[i].address==address){regions[i]=regions[--region_count];return;}abort();
}
void *win64_pointer(uint64_t address,size_t bytes,int writable) {
    for(unsigned i=0;i<region_count;++i)if(address>=regions[i].address && address-regions[i].address<=regions[i].bytes &&
       bytes<=regions[i].bytes-(address-regions[i].address) && (!writable || regions[i].writable))return(void*)(uintptr_t)address;
    fputs("system probe pointer validation failed\n",stderr);abort();
}
uint64_t win64_alloc(size_t bytes,int zero) {
    void *p=zero?calloc(1,bytes):malloc(bytes);if(!p)abort();win64_region_add((uintptr_t)p,bytes,1);return(uintptr_t)p;
}
void win64_free(uint64_t address) {win64_region_remove(address);free((void*)(uintptr_t)address);}
uint64_t win64_argument(CPU *cpu,unsigned index) {if(index==0)return cpu->rcx;if(index==1)return cpu->rdx;abort();}
void win64_set_error(uint32_t error) {last_error=error;}
uint32_t win64_get_error(void) {return last_error;}
void frostpunk_cpuid(CPU *cpu) {cpu->rax=(uint32_t)cpu->rax==1?0x206a7:0;}
int main(void) {win64_system_self_test();return 0;}
