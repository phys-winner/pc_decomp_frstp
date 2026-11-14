#include "cpu64.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <stdlib.h>
void L_000140E26E40(CPU*);void L_000140E26F30(CPU*);void L_000140E26F90(CPU*);
void plain_140E26E40(CPU*);void plain_140E26F30(CPU*);void plain_140E26F90(CPU*);
int64_t g_image_delta;
void frostpunk_unsupported(uint64_t pc,const char*message){fprintf(stderr,"Unexpected trap %llx %s\n",(unsigned long long)pc,message);abort();}
void dispatch(CPU*c,uint64_t pc){(void)c;(void)pc;assert(0);}
static _Alignas(16) unsigned char queue[0xd0],buffer[48];
static unsigned messages=100000;
static void invoke(void(*fn)(CPU*),CPU*c,unsigned char*stack){c->rcx=(uintptr_t)queue;c->rsp=(uintptr_t)stack+96;fn(c);}
static uint32_t index_at(unsigned offset){return __atomic_load_n((uint32_t*)(queue+offset),__ATOMIC_ACQUIRE);}
static void *writer(void*unused){
    (void)unused;CPU c={0};_Alignas(16) unsigned char stack[128]={0};
    for(uint64_t n=1;n<=messages;++n){
        while(index_at(0x10)!=index_at(0xb0))sched_yield();
        unsigned offset=index_at(0x10);uint64_t payload[2]={n,~n};
        memcpy(buffer+offset+8,payload,16);wr64((uintptr_t)queue+0x18,(uintptr_t)buffer+offset);
        wr32((uintptr_t)queue+0x28,16);invoke(L_000140E26E40,&c,stack);
    }return NULL;
}
int main(void){
    _Alignas(16) unsigned char stack[128]={0};CPU a={0},b={0};unsigned cases=0;
    void(*fixed[])(CPU*)={L_000140E26E40,L_000140E26F30,L_000140E26F90};
    void(*plain[])(CPU*)={plain_140E26E40,plain_140E26F30,plain_140E26F90};
    for(unsigned f=0;f<3;++f)for(unsigned n=0;n<512;++n){
        memset(queue,0,sizeof(queue));wr64((uintptr_t)queue,(uintptr_t)buffer);wr32((uintptr_t)queue+8,48);
        wr64((uintptr_t)queue+0x18,(uintptr_t)buffer);wr64((uintptr_t)queue+0xb8,(uintptr_t)buffer);
        wr32((uintptr_t)queue+0x10,n%2?24:0);wr32((uintptr_t)queue+0xb0,n%3?0:24);
        wr32((uintptr_t)queue+0x28,16);wr32((uintptr_t)buffer+4,16);
        unsigned char initial[sizeof(queue)],expected[sizeof(queue)];memcpy(initial,queue,sizeof(queue));
        memset(&a,0,sizeof(a));a.rcx=(uintptr_t)queue;a.rsp=(uintptr_t)stack+96;a.cf=n&1;a.rax=n;b=a;
        plain[f](&a);memcpy(expected,queue,sizeof(queue));memcpy(queue,initial,sizeof(queue));fixed[f](&b);
        a.rip=b.rip=0;assert(!memcmp(&a,&b,sizeof(a)));assert(!memcmp(expected,queue,sizeof(queue)));++cases;
    }
    memset(queue,0,sizeof(queue));wr64((uintptr_t)queue,(uintptr_t)buffer);wr32((uintptr_t)queue+8,sizeof(buffer));
    pthread_t thread;assert(!pthread_create(&thread,NULL,writer,NULL));CPU c={0};
    for(uint64_t n=1;n<=messages;++n){
        do{invoke(L_000140E26F30,&c,stack);if(!(c.rax&255))sched_yield();}while(!(c.rax&255));
        uint64_t payload[2];memcpy(payload,(void*)(uintptr_t)rd64((uintptr_t)queue+0xc0),16);
        assert(payload[0]==n&&payload[1]==~n);invoke(L_000140E26F90,&c,stack);
    }
    assert(!pthread_join(thread,NULL));printf("{\"success\":true,\"cpu_state_cases\":%u,\"ordered_messages\":%u}\n",cases,messages);
}
