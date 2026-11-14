/* Exact production provider, core pointer/LastError and extracted import router.
 * Unrelated import providers are mocked to not-handled in this isolated unit. */
#define win64_dispatch_import unused_full_core_dispatch
#include "../android/host/win64.c"
#undef win64_dispatch_import
#include "win64_accessibility.h"
#include <stdatomic.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t off,unsigned bytes){uint64_t v=0;if(off>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-off||bytes>8)abort();memcpy(&v,fixture_teb+off,bytes);return v;}
void frostpunk_teb_write(uint64_t off,uint64_t v,unsigned bytes){if(off>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-off||bytes>8)abort();memcpy(fixture_teb+off,&v,bytes);}
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;return 0;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;return 0;}
static int unrelated_notification(CPU*c){(void)c;return 0;}
#include "accessibility_router.inc"
static atomic_uint checks,calls;
static void check(int yes,const char *reason){atomic_fetch_add(&checks,1);if(!yes){fprintf(stderr,"ACCESSIBILITY_FIXTURE_FAIL %s\n",reason);_Exit(1);}}
static uint32_t invoke(int routed,uint32_t action,uint32_t ui,uint64_t ptr,uint32_t flags,uint32_t error){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=UINT64_C(0xbadadada00000000)|action;c.rdx=UINT64_C(0xfefefefe00000000)|ui;c.r8=ptr;c.r9=UINT64_C(0xdeadbeef00000000)|flags;c.rsp=UINT64_C(0x123456789abcdef0);before=c;
    errno=6731;win64_set_error(UINT32_C(0x51cafe));int handled=routed?win64_dispatch_import(&c,"UsEr32.DlL","SystemParametersInfoA"):win64_accessibility_call(&c,"SystemParametersInfoA");
    check(handled==1,"known action handled");check(errno==6731&&win64_get_error()==error,"native errno/guest LastError");uint32_t result=(uint32_t)c.rax;check(c.rax==result,"BOOL upperbits");c.rax=before.rax;if(routed)before.rsp+=8;check(!memcmp(&c,&before,sizeof(c)),"full CPU/RSP exact commonpop");atomic_fetch_add(&calls,1);return result;
}
static uint32_t action_get(unsigned kind){return kind==0?0x3a:kind==1?0x34:0x32;}
static void policy_tests(void){
    uint32_t record[6],copy[6];win64_region_add((uintptr_t)record,sizeof(record),1);win64_region_add((uintptr_t)copy,sizeof(copy),1);
    const uint32_t masks[]={0xffff01feu,0x3eu,0x7eu};
    for(unsigned kind=0;kind<3;++kind){unsigned size=kind==2?24:8;uint32_t get=action_get(kind);
        for(unsigned n=0;n<128;++n){uint32_t flags=(n*0x13579bdu)&masks[kind];record[0]=size;record[1]=flags;for(unsigned i=2;i<6;++i)record[i]=n^(UINT32_MAX-i*17u);
            memcpy(copy,record,sizeof(copy));check(invoke(1,get+1,n&1?0:size,(uintptr_t)record,0,0x51cafe)==1,"private SET succeeded");check(!memcmp(record,copy,sizeof(record)),"SET input untouched");memset(copy,0x5a,sizeof(copy));copy[0]=size;
            check(invoke(0,get,size,(uintptr_t)copy,0,0x51cafe)==1&&!memcmp(record,copy,size),"complete flags/timing private roundtrip");
            uint32_t expected[6];memcpy(expected,record,sizeof(expected));record[1]|=1;check(invoke(0,get+1,size,(uintptr_t)record,0,50)==0,"feature ON unsupported");
            record[1]=expected[1];check(invoke(0,get+1,size,(uintptr_t)record,1,50)==0,"OS persistence/broadcast unsupported");
            record[1]=kind==0?0x200u:0x80u;check(invoke(0,get+1,size,(uintptr_t)record,0,87)==0,"reserved flags rejected");
            record[1]=expected[1];check(invoke(0,get+1,size+1,(uintptr_t)record,0,87)==0,"bad SET uiParam rejected");
            record[0]=size-1;check(invoke(0,get+1,size,(uintptr_t)record,0,87)==0,"bad SET cbSize rejected");record[0]=size;copy[0]=size;
            check(invoke(0,get,size,(uintptr_t)copy,0,0x51cafe)==1&&!memcmp(expected,copy,size),"failedSET preserves complete prior state");
        }
    }
    win64_region_remove((uintptr_t)copy);win64_region_remove((uintptr_t)record);
}
static void tuple(uint32_t *out,uint32_t n){out[0]=24;out[1]=(n&0x3f)<<1;out[2]=n;out[3]=~n;out[4]=n^0xa5aa5a5au;out[5]=n*3u;}
static void *worker(void *opaque){unsigned index=(unsigned)(uintptr_t)opaque;uint32_t record[6];win64_region_add((uintptr_t)record,sizeof(record),1);
    for(unsigned i=0;i<4000;++i){if(index<2){tuple(record,i+index*10000u);check(invoke(0,0x33,24,(uintptr_t)record,0,0x51cafe)==1,"concurrentprivatewrite");}else{record[0]=24;check(invoke(0,0x32,24,(uintptr_t)record,0,0x51cafe)==1,"concurrentprivateread");uint32_t expected[6];tuple(expected,record[2]);check(!memcmp(expected,record,sizeof(record)),"atomicfullstructpublication");}}
    win64_region_remove((uintptr_t)record);return NULL;
}
int main(int argc,char **argv){win64_accessibility_self_test();_Alignas(16) unsigned char region[96];
    if(argc==3&&!strcmp(argv[1],"--negative")){CPU c={0};c.rcx=0x3a;c.rdx=8;c.r8=(uintptr_t)region;memset(region,0,sizeof(region));uint32_t size=8;memcpy(region,&size,4);
        if(!strcmp(argv[2],"null"))c.r8=0;
        else if(!strcmp(argv[2],"short_cb"))win64_region_add(c.r8,3,1);
        else if(!strcmp(argv[2],"short_get"))win64_region_add(c.r8,7,1);
        else if(!strcmp(argv[2],"readonly_get"))win64_region_add(c.r8,8,0);
        else if(!strcmp(argv[2],"short_set")){c.rcx=0x33;c.rdx=24;size=24;memcpy(region,&size,4);win64_region_add(c.r8,23,0);}
        else return 2;win64_accessibility_call(&c,"SystemParametersInfoA");return 99;
    }
    if(argc!=2)return 3;FILE *file=fopen(argv[1],"rb");if(!file)return 4;uint32_t hdr[6];unsigned vectors=0;
    win64_region_add((uintptr_t)region,sizeof(region),1);
    while(fread(hdr,sizeof(hdr),1,file)==1){unsigned char expected[96];check(fread(region,96,1,file)==1&&fread(expected,96,1,file)==1,"complete nativeGETrecord");check(invoke(vectors&1,hdr[0],hdr[1],(uintptr_t)region+hdr[2],hdr[3],hdr[5])==hdr[4],"nativeGET BOOL");check(!memcmp(region,expected,96),"nativeGET normalizedvalues/entire96Bguards");++vectors;}
    check(feof(file)&&!ferror(file)&&vectors==255,"allnativeGETcorpus");fclose(file);win64_region_remove((uintptr_t)region);
    policy_tests();uint32_t initial[6];tuple(initial,0);win64_region_add((uintptr_t)initial,sizeof(initial),0);check(invoke(0,0x33,24,(uintptr_t)initial,0,0x51cafe)==1,"concurrentinitialtuple");win64_region_remove((uintptr_t)initial);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)check(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i),"threadcreate");for(unsigned i=0;i<4;++i)check(!pthread_join(threads[i],NULL),"threadjoin");
    CPU c,before;memset(&c,0xa5,sizeof(c));before=c;win64_set_error(0x51cafe);check(!win64_accessibility_call(&c,"SystemParametersInfoW")&&!memcmp(&c,&before,sizeof(c)),"unknownAPI untouched");c.rcx=0x9999;before=c;check(!win64_accessibility_call(&c,"SystemParametersInfoA")&&!memcmp(&c,&before,sizeof(c)),"unknownaction untouched");
    c=before;c.rcx=0x3a;before=c;check(!win64_dispatch_import(&c,"kernel32.dll","SystemParametersInfoA")&&!memcmp(&c,&before,sizeof(c)),"USER32 onlyDLLgate");
    printf("{\"success\":true,\"native_GET_cases\":%u,\"checks\":%u,\"calls\":%u,\"concurrent_calls\":16000,\"threads\":4,\"private_policy\":true,\"no_OS_SET\":true,\"full_CPU_and_RSP\":true,\"one_common_pop\":true,\"DLL_gate\":true,\"actual_game_input_proven\":false}\n",vectors,atomic_load(&checks),atomic_load(&calls));return 0;
}
