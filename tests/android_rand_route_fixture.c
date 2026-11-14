/* The real utility providers have independent TLS state in the two objects. */
#include "route_support.inc"
#include <pthread.h>
#include <time.h>
void dispatch(CPU *c,uint64_t target){(void)c;(void)target;abort();}
static CPU random_input(uint64_t seed){CPU c;memset(&c,0x5a,sizeof(c));c.rsp=UINT64_C(0x123456789abcd000);c.rcx=seed;return c;}
static uint64_t random_cases;
static void random_pair(const char *name,uint64_t seed,uint64_t expected){
    CPU c=random_input(seed);Result a=invoke(0,"api-ms-win-crt-utility-l1-1-0.dll",name,c),b=invoke(1,"api-ms-win-crt-utility-l1-1-0.dll",name,c);
    CPU wanted=c;wanted.rsp+=8;wanted.rax=expected;
    check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU))&&!memcmp(&b.cpu,&wanted,sizeof(CPU)),"rand/srand whole CPU");
    check(a.native_errno==b.native_errno&&a.crt_errno==b.crt_errno&&a.error==b.error,"random error state");++random_cases;
    if(random_cases==1)check(a.registration_count==4&&b.registration_count==4,"random cold FILE registration");
}
static void *thread_random(void *argument){
    uintptr_t salt=(uintptr_t)argument;CPU a=random_input(0),b=a;
    check(baseline_utility_call(&a,"rand")&&win64_utility_call(&b,"rand")&&a.rax==41&&b.rax==41,"new-thread default random state");
    for(unsigned round=0;round<256;++round){uint32_t state=(uint32_t)(round*UINT32_C(1664525)+salt);a=random_input(state);b=a;
        check(baseline_utility_call(&a,"srand")&&win64_utility_call(&b,"srand"),"thread seed");
        for(unsigned i=0;i<32;++i){state=state*UINT32_C(214013)+UINT32_C(2531011);a=random_input(i);b=a;
            check(baseline_utility_call(&a,"rand")&&win64_utility_call(&b,"rand"),"thread random");
            check(a.rax==((state>>16)&32767u)&&!memcmp(&a,&b,sizeof(a)),"thread-local independent sequence");}
    }
    return NULL;
}
int main(void){
    win64_region_add((uintptr_t)destination,sizeof(destination),1);win64_region_add((uintptr_t)source,sizeof(source),1);
    for(unsigned seed=0;seed<1024;++seed){uint32_t state=seed*UINT32_C(0x9e3779b9);
        random_pair("srand",UINT64_C(0xffffffff00000000)|state,0);
        for(unsigned i=0;i<64;++i){state=state*UINT32_C(214013)+UINT32_C(2531011);random_pair("rand",i,(state>>16)&32767u);}
    }
    pthread_t threads[8];for(uintptr_t i=0;i<8;++i)check(!pthread_create(&threads[i],NULL,thread_random,(void*)(i+1)),"random thread starts");
    for(unsigned i=0;i<8;++i)check(!pthread_join(threads[i],NULL),"random thread joins");
    for(unsigned variant=0;variant<2;++variant){CPU c=random_input(17),before=c;
        int yes=variant?candidate_route(&c,"kernel32.dll","rand"):baseline_route(&c,"kernel32.dll","rand");
        check(!yes&&!memcmp(&c,&before,sizeof(c)),"wrong random DLL declines");}
    printf("{\"success\":true,\"random_pairs\":%"PRIu64",\"thread_random_cases\":65536,\"whole_CPU\":true,\"tls_independent\":true,\"pure_declines_stubbed\":true}\n",random_cases);
    return 0;
}
