/* Extracted production onexit implementation; dispatcher records guest callbacks. */
#define _GNU_SOURCE
#include "cpu64.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t *exit_functions,tls_exit_callback;
static size_t exit_count,exit_capacity;
static unsigned locks,seen[32],count;
static uint64_t active_table;
static int append_callback;
static unsigned cleanup_depth;
static void crt_exit_enter(void){++cleanup_depth;}
static void crt_exit_leave(void){assert(cleanup_depth);--cleanup_depth;}
static void fixture_exiting(uint32_t code){fprintf(stderr,"exiting=%u\n",code);}
static void (*frostpunk_android_exiting)(uint32_t)=fixture_exiting;
static void metadata_lock(void){++locks;}
static void metadata_unlock(void){assert(locks);--locks;}
static _Noreturn void invalid(const char *format,...){(void)format;abort();}
void *win64_pointer(uint64_t address,size_t bytes,int write){(void)bytes;(void)write;assert(address);return(void*)(uintptr_t)address;}
uint64_t win64_alloc(size_t bytes,int zero){return(uintptr_t)(zero?calloc(1,bytes):malloc(bytes));}
void win64_free(uint64_t address){free((void*)(uintptr_t)address);}
#include "crt_exit_production.inc"
void dispatch(CPU *cpu,uint64_t target){
    assert(!locks&&count<32);seen[count++]=(unsigned)target;
    if(target==99)assert(!cpu->rcx&&!cpu->rdx&&!cpu->r8);
    if(target==2&&append_callback){append_callback=0;if(active_table)assert(!register_onexit_function(active_table,3));else assert(!crt_atexit_register(3));}
    if(target==7)puts("callback=7");
    cpu->rax=123;cpu->rcx=456;cpu->rbx=789;cpu->xmm[6].u64[0]=999;cpu->rsp+=8;
}
int main(int argc,char **argv){
    uint64_t stack[256]={0};CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rsp=(uintptr_t)&stack[240];before=cpu;
    if(argc==2){assert(!crt_atexit_register(7));cpu.rcx=37;assert(!fixture_exit_route(&cpu,argv[1]));return 2;}
    assert(execute_onexit_table(&cpu,0)==-1);
    assert(!crt_atexit_register(1)&&!crt_atexit_register(0)&&!crt_atexit_register(2));tls_exit_callback=99;append_callback=1;
    assert(fixture_exit_route(&cpu,"_cexit")&&!memcmp(&cpu,&before,sizeof(cpu)));
    const unsigned expected[]={99,2,3,1};assert(count==4&&!memcmp(seen,expected,sizeof(expected))&&!exit_count&&!tls_exit_callback);
    assert(fixture_exit_route(&cpu,"_cexit")&&count==4);assert(!crt_atexit_register(7));assert(fixture_exit_route(&cpu,"_c_exit")&&count==4&&exit_count==1);exit_count=0;
    uint64_t table[3]={0};assert(!initialize_onexit_table((uintptr_t)table));assert(!register_onexit_function((uintptr_t)table,1)&&!register_onexit_function((uintptr_t)table,0)&&!register_onexit_function((uintptr_t)table,2));
    active_table=(uintptr_t)table;append_callback=1;count=0;assert(!execute_onexit_table(&cpu,active_table));const unsigned table_expected[]={2,3,1};assert(count==3&&!memcmp(seen,table_expected,sizeof(table_expected))&&!table[0]&&!table[1]&&!table[2]&&!memcmp(&cpu,&before,sizeof(cpu)));
    assert(!execute_onexit_table(&cpu,active_table)&&count==3&&!cleanup_depth);free(exit_functions);
    puts("{\"success\":true,\"TLS_before_atexit\":true,\"reverse_callbacks\":true,\"callbacks_registered_during_exit\":true,\"onexit_table_reset\":true,\"repeat_cleanup\":true,\"full_CPU_RSP_preserved\":true,\"c_exit_skips_callbacks\":true}");return 0;
}
