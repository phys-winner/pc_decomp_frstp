/* Lifecycle fixture: production dependency/bootstrap helpers, real pthread and
 * private PE data mapping, with compiled trace bodies instead of game code. */
#define _GNU_SOURCE
#include "../android/host/dependencies.c"
#include "../android/host/win64_threads.h"
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
typedef struct fixture_region{uint64_t base;size_t size;int write;struct fixture_region*next;}fixture_region;
static fixture_region *fixture_regions;
static pthread_mutex_t fixture_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint64_t fixture_main,fixture_stack;static unsigned sequence[8],sequence_count;
static _Thread_local unsigned char guest_teb[0x2000];
static unsigned char guest_peb[256];
static _Thread_local uint64_t static_tls_vector[64],stack_lo,stack_hi;
static _Thread_local unsigned call_depth;
static uint32_t fixture_error;
#define STACK_BYTES 4096
typedef struct{const unsigned char*bytes;size_t size;uint64_t base;uint32_t image_size,directory_table,directories;}pe_t;
static uint32_t u32(const void*p){return read32(p);}static uint64_t u64(const void*p){return read64(p);}
static _Noreturn void stop(const char*format,...){va_list a;va_start(a,format);vfprintf(stderr,format,a);va_end(a);fputc('\n',stderr);exit(1);}
static void check(int yes,const char*s){if(!yes)stop("FAIL dependency thread fixture %s",s);}
void win64_region_add(uint64_t p,size_t n,int w){fixture_region*r=malloc(sizeof(*r));check(r!=NULL,"region alloc");pthread_mutex_lock(&fixture_mutex);*r=(fixture_region){p,n,w,fixture_regions};fixture_regions=r;pthread_mutex_unlock(&fixture_mutex);}
void win64_region_remove(uint64_t p){pthread_mutex_lock(&fixture_mutex);fixture_region**r=&fixture_regions;while(*r&&(*r)->base!=p)r=&(*r)->next;check(*r!=NULL,"region remove");fixture_region*old=*r;*r=old->next;free(old);pthread_mutex_unlock(&fixture_mutex);}
void*win64_pointer(uint64_t p,size_t n,int w){pthread_mutex_lock(&fixture_mutex);for(fixture_region*r=fixture_regions;r;r=r->next)if(p>=r->base&&p-r->base<=r->size&&n<=r->size-(p-r->base)&&(!w||r->write)){pthread_mutex_unlock(&fixture_mutex);return(void*)(uintptr_t)p;}stop("FAIL fixture pointer %" PRIx64 " bytes=%zu",p,n);}
uint64_t win64_alloc(size_t n,int zero){void*p=zero?calloc(1,n):malloc(n);check(p!=NULL,"alloc");win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p;}
void win64_free(uint64_t p){win64_region_remove(p);free((void*)(uintptr_t)p);}
void win64_image_add(uint64_t p,size_t n){check(p==DEPENDENCY_BASE&&n==DEPENDENCY_SIZE,"dependency image record");}
int win64_dispatch_import(CPU*c,const char*dll,const char*name){(void)c;(void)dll;(void)name;stop("fixture unexpectedly called an HLE import");}
uint64_t frostpunk_teb_read(uint64_t off,unsigned n){check(off<=sizeof(guest_teb)&&n<=sizeof(guest_teb)-off,"TEB read");uint64_t v=0;memcpy(&v,guest_teb+off,n);return v;}
void frostpunk_teb_write(uint64_t off,uint64_t value,unsigned n){check(off<=sizeof(guest_teb)&&n<=sizeof(guest_teb)-off,"TEB write");memcpy(guest_teb+off,&value,n);}
void win64_set_error(uint32_t value){fixture_error=value;}uint32_t win64_get_error(void){return fixture_error;}
void es3_eh_thread_reset(void){}void win64_thread_crt_attach(void){}void win64_thread_crt_detach(void){}
void win64_threads_configure(const win64_thread_hooks*hooks,size_t bytes){(void)hooks;(void)bytes;}
void win64_threads_adopt_current(CPU*c){(void)c;}void win64_threads_process_ready(void){}
const dependency_entry msvcp140_recomp_dispatch_table[]={{0,NULL}};const uint32_t msvcp140_recomp_dispatch_count=0;
void dispatch(CPU*c,uint64_t target){
    check(c->rsp%16==8,"Win64 callee stack alignment");win64_pointer(c->rsp,40,1);check(c->r8==0,"reserved argument null");
    if(target==DEPENDENCY_ENTRY){check(c->rcx==DEPENDENCY_BASE,"DllMain instance");check(c->rdx>=1&&c->rdx<=3,"DllMain reason");sequence[sequence_count++]=(unsigned)c->rdx;}
    else{check(target==fixture_main+0x100&&c->rcx==fixture_main,"static TLS instance/entry");check(c->rdx==2||c->rdx==3,"TLS reason");sequence[sequence_count++]=(unsigned)c->rdx+10;}
    uint64_t rsp=c->rsp;unsigned reason=(unsigned)c->rdx;memset(c,0xa5,sizeof(*c));c->rax=reason==1?1:0;c->rsp=rsp+8;
}
#include "../android/host/bootstrap_threads.inc"
static void*worker(void*parameter){(void)parameter;CPU c,wanted={0};uint64_t stack=win64_alloc(STACK_BYTES,1);
    check(bootstrap_thread_attach(&c,stack,STACK_BYTES),"production worker attach");wanted.rsp=(stack+STACK_BYTES-64)&~UINT64_C(15);wanted.mxcsr=0x1f80;
    check(!memcmp(&c,&wanted,sizeof(c)),"attach callback CPU/RSP restored");bootstrap_thread_detach(&c);check(!memcmp(&c,&wanted,sizeof(c)),"detach callback CPU/RSP restored");win64_free(stack);return NULL;
}
int main(int argc,char**argv){
    fixture_stack=win64_alloc(STACK_BYTES,1);CPU c={0};c.rsp=(fixture_stack+STACK_BYTES-64)&~UINT64_C(15);c.mxcsr=0x1f80;CPU before=c;
    if(argc==2&&!strcmp(argv[1],"--preinit")){frostpunk_dependencies_thread_notify(&c,2);stop("pre-init notification incorrectly returned");}
    frostpunk_dependencies_init(&c);check(!memcmp(&c,&before,sizeof(c)),"process DllMain CPU/RSP restored");
    fixture_main=win64_alloc(4096,1);uint64_t callback=fixture_main+0x100;memcpy(win64_pointer(fixture_main+0x200,8,1),&callback,8);
    bootstrap_thread_image.image=fixture_main;bootstrap_thread_image.image_bytes=4096;bootstrap_thread_image.callbacks=fixture_main+0x200;
    pthread_t thread;check(!pthread_create(&thread,NULL,worker,NULL),"pthread create");check(!pthread_join(thread,NULL),"pthread join");
    const unsigned expected[]={1,2,12,13,3};check(sequence_count==5&&!memcmp(sequence,expected,sizeof(expected)),"dependency attach before main TLS; detach reverse order");
    printf("{\"pass\":true,\"native_pthread\":true,\"production_loader_helpers\":true,\"sequence\":[1,2,12,13,3],\"balanced_RSP\":true,\"preserved_CPU\":true,\"thread_BOOL_ignored\":true,\"original_DllMain_bodies_executed\":false,\"original_x64_bytes_executed\":false}\n");return 0;
}
