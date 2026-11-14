#include "win64_newsfeed.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
typedef struct {uint64_t p;size_t n;int write;} region;
static region regions[128];static unsigned nregions;
void win64_region_add(uint64_t p,size_t n,int write){if(nregions==128)abort();regions[nregions++]=(region){p,n,write};}
void win64_region_remove(uint64_t p){for(unsigned i=0;i<nregions;++i)if(regions[i].p==p){regions[i]=regions[--nregions];return;}abort();}
void *win64_pointer(uint64_t p,size_t n,int write){for(unsigned i=0;i<nregions;++i)if(p>=regions[i].p&&p-regions[i].p<=regions[i].n&&n<=regions[i].n-(p-regions[i].p)&&(!write||regions[i].write))return(void*)(uintptr_t)p;fprintf(stderr,"newsfeed fixture invalid range %llx+%zu write%d\n",(unsigned long long)p,n,write);abort();}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t r[]={c->rcx,c->rdx,c->r8,c->r9};return r[i];}abort();}
static const char *const names[]={
"??0NewsFeedDLL@@QEAA@XZ","??1NewsFeedDLL@@QEAA@XZ","??4NewsFeedDLL@@QEAAAEAV0@$$QEAV0@@Z","?ClearNewses@NewsFeedDLL@@QEAAXXZ","?GetNewsSize@NewsFeedDLL@@QEAAHXZ",
"?GetNews@NewsFeedDLL@@QEAA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z",
"?GetNewsDataField@NewsFeedDLL@@QEBA?AW4NewsFeedError@@HW4NewsDataField@@AEAPEBD@Z",
"?GetLink@NewsFeedDLL@@QEBA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAPEBD@Z",
"?GetBannerPreset@NewsFeedDLL@@QEBA?AW4NewsFeedError@@AEAPEBD@Z",
"?ReadJSONFromURL@NewsFeedDLL@@QEAA?AW4NewsFeedError@@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"};
EXPORT void news_add(void*p,size_t n,int write){win64_region_add((uintptr_t)p,n,write);}
EXPORT void news_remove(void*p){win64_region_remove((uintptr_t)p);}
EXPORT void news_enable(void){win64_newsfeed_enable_offline();}
EXPORT void news_selftest(void){win64_newsfeed_self_test();}
EXPORT uint64_t news_run(unsigned which,void*p,uint64_t arg1,uint64_t arg2,uint64_t arg3){
    if(which>=10)abort();CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)p;c.rdx=arg1;c.r8=arg2;c.r9=arg3;before=c;
    int implemented=win64_newsfeed_call(&c,names[which]);uint64_t result=c.rax;c.rax=before.rax;
    if(memcmp(&c,&before,sizeof(c)))abort();return implemented?result:UINT64_MAX;
}
#if defined(NEWSFEED_FIXTURE_MAIN)
#include "newsfeed_cases.h"
int main(int argc,char**argv){
    unsigned char a[208],b[208],text[32]={0};uint64_t output=UINT64_C(0x123456789abcdef0);text[24]=15;
    news_add(a,sizeof(a),1);news_add(b,sizeof(b),1);news_add(text,sizeof(text),0);news_add(&output,sizeof(output),1);
    if(news_run(0,a,0,0,0)!=UINT64_MAX)abort(); /* gate must be disabled initially */
    news_selftest();news_enable();
    if(argc>1){memset(a,0,sizeof(a));news_run(0,a,0,0,0);if(!strcmp(argv[1],"nonempty")){a[0x18]=1;news_run(4,a,0,0,0);}else if(!strcmp(argv[1],"destroyed")){news_run(1,a,0,0,0);news_run(4,a,0,0,0);}else if(!strcmp(argv[1],"badstring")){text[16]=16;news_run(9,a,(uintptr_t)text,0,0);}else abort();return 99;}
    unsigned failures=0;
    for(unsigned i=0;i<NEWS_CASE_COUNT;++i){const news_case*t=&news_cases[i];if(t->reset){memset(a,t->seed_a,sizeof(a));memset(b,t->seed_b,sizeof(b));}
        void*p=t->object?b:a;uint64_t x=t->arg1,y=t->arg2,z=t->arg3;
        if(t->which==2)x=(uintptr_t)(t->source?b:a);
        if(t->which==5||t->which==7||t->which==9)x=(uintptr_t)text;
        if(t->which==6)z=(uintptr_t)&output;
        if(t->which==7)y=(uintptr_t)&output;
        if(t->which==8)x=(uintptr_t)&output;
        uint64_t value=news_run(t->which,p,x,y,z),expected=t->result_is_this?(uintptr_t)p:t->expected;
        if((t->check_result&&value!=expected)||memcmp(a,t->a,208)||memcmp(b,t->b,208)||output!=UINT64_C(0x123456789abcdef0)){
            fprintf(stderr,"NEWS_CASE_FAIL %u operation%u actual%llx expected%llx\n",i,t->which,(unsigned long long)value,(unsigned long long)expected);++failures;
        }
    }
    printf("{\"original_DLL_cases\":%u,\"failures\":%u,\"success\":%s,\"network_policy\":\"unavailable_error_1\",\"game_boot_proven\":false}\n",NEWS_CASE_COUNT,failures,failures?"false":"true");return failures?1:0;
}
#endif
