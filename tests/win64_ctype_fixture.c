/* Real locale provider and core pointer/TEB helpers; no original guest code. */
#include "../android/host/win64.c"
#include "ctype_vectors.h"

static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){
    uint64_t result=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&result,fixture_teb+offset,bytes);return result;
}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){
    if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);
}
typedef struct{unsigned calls,failed;} ctype_worker;
static const char locale_c[]="C",locale_utf8[]="en-US.UTF8";
static void set_mode(unsigned mode){
    CPU c={0};c.rcx=1;if(!win64_locale_call(&c,"_configthreadlocale"))abort();
    c.rcx=2;c.rdx=(uintptr_t)(mode?locale_utf8:locale_c);if(!win64_locale_call(&c,"setlocale")||!c.rax)abort();
}
static int invoke(const char*name,uint64_t argument,uint32_t result,int handled){
    CPU c,expected;memset(&c,0xa5,sizeof(c));c.rcx=argument;c.rsp=UINT64_C(0x1234567812345678);expected=c;
    if(handled)expected.rax=result;
    win64_set_error(0x5a17);win64_stdio_set_errno(2718);errno=1927;
    return win64_locale_call(&c,name)==handled&&!memcmp(&c,&expected,sizeof(c))&&
        win64_get_error()==0x5a17&&crt_errno==2718&&errno==1927;
}
static void*thread_cases(void*opaque){
    ctype_worker*w=opaque;
    win64_locale_thread_attach();
    for(unsigned repeat=0;repeat<2;++repeat)for(unsigned mode=0;mode<2;++mode){
        set_mode(mode);
        for(unsigned i=0;i<sizeof(ctype_vectors)/sizeof(*ctype_vectors);++i){
            const ctype_vector*v=&ctype_vectors[i];if(v->locale!=mode)continue;
            if(!invoke(v->name,UINT64_C(0x9876543200000000)|(uint32_t)v->character,(uint32_t)v->result,1))w->failed=1;
            ++w->calls;
        }
    }
    return NULL;
}
int main(void){
    win64_region_add((uintptr_t)locale_c,sizeof(locale_c),0);win64_region_add((uintptr_t)locale_utf8,sizeof(locale_utf8),0);
    /* First classifier initializes real locale data while preserving errno. */
    if(!invoke("isdigit",'0',4,1))return 1;
    win64_locale_self_test();unsigned calls=1;
    const uint64_t high[]={0,UINT64_C(0x1234567800000000),UINT64_C(0xffffffff00000000)};
    for(unsigned mode=0;mode<2;++mode){
        set_mode(mode);
        for(unsigned i=0;i<sizeof(ctype_vectors)/sizeof(*ctype_vectors);++i){
            const ctype_vector*v=&ctype_vectors[i];if(v->locale!=mode)continue;
            for(unsigned h=0;h<3;++h){if(!invoke(v->name,high[h]|(uint32_t)v->character,(uint32_t)v->result,1))return 2;++calls;}
        }
    }
    const char*classifiers[]={"isupper","islower","isalpha","isalnum","iscntrl","isspace","isdigit","isxdigit","ispunct"};
    const int32_t outside[]={INT32_MIN,-128,-2,256,INT32_MAX};unsigned policy=0;
    for(unsigned i=0;i<9;++i)for(unsigned n=0;n<5;++n)for(unsigned h=0;h<3;++h){
        if(!invoke(classifiers[i],high[h]|(uint32_t)outside[n],0,1))return 3;++policy;
    }
    const char*unknown[]={"isdigit_l","IsDigit","isgraph","isprint"};
    for(unsigned i=0;i<4;++i)if(!invoke(unknown[i],UINT64_MAX,0,0))return 4;
    pthread_t threads[4];ctype_worker workers[4]={0};unsigned parallel=0;
    for(unsigned i=0;i<4;++i)if(pthread_create(&threads[i],NULL,thread_cases,&workers[i]))return 5;
    for(unsigned i=0;i<4;++i){if(pthread_join(threads[i],NULL)||workers[i].failed)return 6;parallel+=workers[i].calls;}
    printf("{\"success\":true,\"native_vectors\":%zu,\"CPU_error_argument_cases\":%u,\"parallel_calls\":%u,\"out_of_domain_policy_cases\":%u,\"private_locale_threads\":4,\"full_CPU\":true,\"no_RSP_pop\":true,\"native_errno\":true,\"guest_errno\":true,\"real_core_LastError\":true,\"new_mask_functions\":3,\"old_functions\":8,\"original_game_executed\":false}\n",sizeof(ctype_vectors)/sizeof(*ctype_vectors),calls,parallel,policy);
    return 0;
}
