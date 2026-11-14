/* Independent bounded ABI adapter for Frostpunk's optional online news feed.
 * Local mode never opens a network session: ReadJSONFromURL returns the
 * original DLL's InternetOpenW-failed error (1), not successful empty JSON.
 * Only constructed, empty offline objects are supported. No game/news data
 * or successful network response is manufactured here. */
#include "win64_newsfeed.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static SRWLOCK lock=SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
#include <pthread.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif
typedef struct object_record {uint64_t address;struct object_record *next;} object_record;
static object_record *objects;
static int offline_enabled;
static const unsigned strings[]={8,0x40,0x60,0x98};
static const unsigned vectors[]={0x28,0x80,0xb8};
static const char *const names[]={
    "??0NewsFeedDLL@@QEAA@XZ",
    "??1NewsFeedDLL@@QEAA@XZ",
    "??4NewsFeedDLL@@QEAAAEAV0@$$QEAV0@@Z",
    "?ClearNewses@NewsFeedDLL@@QEAAXXZ",
    "?GetNewsSize@NewsFeedDLL@@QEAAHXZ",
    "?GetNews@NewsFeedDLL@@QEAA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
    "?GetNewsDataField@NewsFeedDLL@@QEBA?AW4NewsFeedError@@HW4NewsDataField@@AEAPEBD@Z",
    "?GetLink@NewsFeedDLL@@QEBA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAPEBD@Z",
    "?GetBannerPreset@NewsFeedDLL@@QEBA?AW4NewsFeedError@@AEAPEBD@Z",
    "?ReadJSONFromURL@NewsFeedDLL@@QEAA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"
};
static void stop(const char *why){fprintf(stderr,"STOP Win64 offline newsfeed %s\n",why);exit(73);}
static uint64_t get64(const unsigned char*p){uint64_t v;memcpy(&v,p,8);return v;}
static void put64(unsigned char*p,uint64_t v){memcpy(p,&v,8);}
static object_record **find(uint64_t address){object_record **p=&objects;while(*p&&(*p)->address!=address)p=&(*p)->next;return p;}
static void empty_check(const unsigned char*p){
    for(unsigned i=0;i<4;++i){unsigned o=strings[i];if(p[o]||get64(p+o+16)||get64(p+o+24)!=15)stop("nonempty/unsupported string state");}
    for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)if(get64(p+vectors[i]+j*8))stop("nonempty/unsupported vector state");
}
static unsigned char *existing(uint64_t address,int write){
    if(!*find(address))stop("unconstructed or destroyed object");
    unsigned char*p=win64_pointer(address,208,write);empty_check(p);return p;
}
static void construct(unsigned char*p){
    /* Preserve original uninitialized DWORD/padding and string buffer tail. */
    static const unsigned zeros[]={8,0x18,0x28,0x30,0x38,0x40,0x50,0x60,0x70,0x80,0x88,0x90,0x98,0xa8,0xb8,0xc0,0xc8};
    for(unsigned i=0;i<sizeof(zeros)/sizeof(zeros[0]);++i)put64(p+zeros[i],0);
    for(unsigned i=0;i<4;++i)put64(p+strings[i]+24,15);
}
static void reset_strings(unsigned char*p){for(unsigned i=0;i<4;++i){unsigned o=strings[i];p[o]=0;put64(p+o+16,0);put64(p+o+24,15);}}
static void validate_string(uint64_t address){
    const unsigned char*p=win64_pointer(address,32,0);uint64_t size=get64(p+16),cap=get64(p+24);
    if(size>cap||size>1024*1024||cap>INT64_MAX)stop("unsupported input std::string bounds");
    uint64_t data=cap<16?address:get64(p);const unsigned char*s=win64_pointer(data,(size_t)size+1,0);
    if(s[size])stop("input std::string lacks terminating NUL");
}
void win64_newsfeed_enable_offline(void){LOCK();offline_enabled=1;UNLOCK();}
int win64_newsfeed_call(CPU*c,const char*name){
    unsigned which;for(which=0;which<sizeof(names)/sizeof(names[0]);++which)if(!strcmp(name,names[which]))break;
    if(which==sizeof(names)/sizeof(names[0]))return 0;
    LOCK();if(!offline_enabled){UNLOCK();return 0;}
    uint64_t address=win64_argument(c,0);unsigned char*p;
    if(which==0){
        p=win64_pointer(address,208,1);object_record **slot=find(address);
        if(*slot)empty_check(p);else{object_record*r=malloc(sizeof(*r));if(!r)stop("native object registry allocation");r->address=address;r->next=objects;objects=r;}
        construct(p);c->rax=address;
    }else{
        p=existing(address,which==1||which==2||which==3);
        switch(which){
        case 1:{reset_strings(p);object_record **slot=find(address),*old=*slot;*slot=old->next;free(old);c->rax=0;break;}
        case 2:{uint64_t source=win64_argument(c,1);unsigned char*q=existing(source,1);
            if(source!=address){memcpy(p,q,4);for(unsigned i=0;i<4;++i)memcpy(p+strings[i],q+strings[i],32);reset_strings(q);/* all vectors already zero */}
            c->rax=address;break;}
        case 3:put64(p+0xc0,get64(p+0xb8));c->rax=0;break;
        case 4:c->rax=0;break;
        case 5:c->rax=5;break;
        case 6:c->rax=6;break;
        case 7:c->rax=8;break;
        case 8:c->rax=9;break;
        case 9:validate_string(win64_argument(c,1));c->rax=1;break;
        }
    }
    UNLOCK();return 1;
}
void win64_newsfeed_self_test(void){
    unsigned char a[208],b[208],text[32]={0};CPU c={0};int before;
    memset(a,0xa5,sizeof(a));memset(b,0x5a,sizeof(b));put64(text+24,15);
    win64_region_add((uintptr_t)a,sizeof(a),1);win64_region_add((uintptr_t)b,sizeof(b),1);win64_region_add((uintptr_t)text,sizeof(text),0);
    LOCK();before=offline_enabled;UNLOCK();win64_newsfeed_enable_offline();
    c.rcx=(uintptr_t)a;c.rsp=0x12345678;c.rbx=0xabcdef;c.cf=1;
    if(!win64_newsfeed_call(&c,names[0])||c.rax!=(uintptr_t)a||a[0]!=0xa5||a[4]!=0xa5||a[16]!=0xa5)stop("constructor ABI self-test");
    c.rcx=(uintptr_t)b;if(!win64_newsfeed_call(&c,names[0]))stop("second constructor");
    for(unsigned i=3;i<10;++i){c.rcx=(uintptr_t)a;c.rdx=(uintptr_t)text;c.r8=0;c.r9=0;uint64_t expected[]={0,0,5,6,8,9,1};
        if(!win64_newsfeed_call(&c,names[i])||c.rax!=expected[i-3])stop("empty/error self-test");}
    c.rcx=(uintptr_t)b;c.rdx=(uintptr_t)a;if(!win64_newsfeed_call(&c,names[2])||c.rax!=(uintptr_t)b||b[0]!=0xa5||b[4]!=0x5a||b[16]!=0xa5)stop("empty move self-test");
    c.rdx=(uintptr_t)b;if(!win64_newsfeed_call(&c,names[2])||c.rax!=(uintptr_t)b)stop("self move self-test");
    c.rcx=(uintptr_t)a;if(!win64_newsfeed_call(&c,names[1]))stop("first destructor");c.rcx=(uintptr_t)b;if(!win64_newsfeed_call(&c,names[1]))stop("second destructor");
    if(c.rsp!=0x12345678||c.rbx!=0xabcdef||!c.cf||win64_newsfeed_call(&c,"unknown_newsfeed"))stop("CPU preservation self-test");
    LOCK();offline_enabled=before;UNLOCK();win64_region_remove((uintptr_t)text);win64_region_remove((uintptr_t)b);win64_region_remove((uintptr_t)a);
    fputs("WIN64_NEWSFEED_SELF_TEST explicit_offline_gate=1 object208=1 empty_move=1 network_unavailable_error=1 no_rsp_pop=1 passed\n",stderr);
}
