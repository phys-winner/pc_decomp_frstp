/* Real metrics-DC registry/CPU ABI fixture, no surface or GDI rendering. */
#include "win64_display.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
static _Thread_local uint32_t last_error;
static unsigned checks;
static uint16_t driver[]={'D','I','S','P','L','A','Y',0};
static void require(int value,const char *why){if(!value){fprintf(stderr,"GETDC_FIXTURE_FAIL %s error=%u\n",why,last_error);fflush(stderr);_Exit(1);}
#ifdef _WIN32
    InterlockedIncrement((volatile LONG*)&checks);
#else
    __atomic_fetch_add(&checks,1,__ATOMIC_RELAXED);
#endif
}
void win64_set_error(uint32_t value){last_error=value;}uint32_t win64_get_error(void){return last_error;}
uint64_t win64_argument(CPU *c,unsigned i){switch(i){case 0:return c->rcx;case 1:return c->rdx;case 2:return c->r8;case 3:return c->r9;default:abort();}}
void *win64_pointer(uint64_t p,size_t n,int w){uint64_t base=(uintptr_t)driver;if(!w && p>=base && p-base<=sizeof(driver) && n<=sizeof(driver)-(p-base))return(void*)(uintptr_t)p;abort();}
void win64_region_add(uint64_t p,size_t n,int w){(void)p;(void)n;(void)w;abort();}void win64_region_remove(uint64_t p){(void)p;abort();}
uint64_t win64_alloc(size_t n,int zero){(void)n;(void)zero;abort();}void win64_free(uint64_t p){(void)p;abort();}void dispatch(CPU *c,uint64_t target){(void)c;(void)target;abort();}
int win64_window_call(CPU *c,const char *name){
    require(!strcmp(name,"IsWindow"),"exact live-HWND validator");
    CPU nested={0};nested.rcx=0;require(win64_display_call(&nested,"GetSystemMetrics") && nested.rax==960,"window validation can reenter display outside lock");
    c->rax=c->rcx==0x1111 || c->rcx==0x2222;return 1;
}
static uint64_t api(const char *name,uint64_t a,uint64_t b){
    CPU c;unsigned char *bytes=(unsigned char*)&c;for(size_t i=0;i<sizeof(c);++i)bytes[i]=(unsigned char)(i*37+19);
    c.rsp=UINT64_C(0x123456789abcdef8);c.rcx=a;c.rdx=b;c.r8=c.r9=0;CPU before=c;
    require(win64_display_call(&c,name),"implemented import");uint64_t result=c.rax;c.rax=before.rax;
    require(!memcmp(&c,&before,sizeof(c)),"full CPU flags XMM and RSP preserved");return result;
}
static void worker(unsigned id){for(unsigned i=0;i<32;++i){last_error=0x5a17;uint64_t h=api("GetDC",id&1?0x1111:0x2222,0);require(h && last_error==0x5a17,"parallel acquisition preserves thread error");require(api("GetDeviceCaps",h,8)==960 && api("GetDeviceCaps",h,10)==540,"parallel actual metrics");require(api("ReleaseDC",id&1?0x1111:0x2222,h)==1 && last_error==0x5a17,"parallel owned release");}}
#ifdef _WIN32
static DWORD WINAPI start(LPVOID p){worker((unsigned)(uintptr_t)p);return 0;}
#else
static void *start(void *p){worker((unsigned)(uintptr_t)p);return NULL;}
#endif
int main(int argc,char **argv){
    win64_display_configure(960,540,60);
    if(argc>1){
        if(!strcmp(argv[1],"wrong-owner")){uint64_t dc=api("GetDC",0x1111,0);api("ReleaseDC",0x2222,dc);}
        else if(!strcmp(argv[1],"private-release")){uint64_t dc=api("CreateDCW",(uintptr_t)driver,0);api("ReleaseDC",0,dc);}
        else if(!strcmp(argv[1],"common-delete")){uint64_t dc=api("GetDC",0,0);api("DeleteDC",dc,0);}
        else if(!strcmp(argv[1],"live-reconfigure")){api("GetDC",0,0);win64_display_configure(960,540,60);}
        else return 2;return 3;
    }
    for(unsigned i=0;i<3;++i){uint64_t owner=i?i==1?0x1111:0x2222:0;last_error=0x5a17;
        uint64_t h=api("GetDC",owner,0);require(h && last_error==0x5a17,"desktop/hidden-window lease");
        require(api("GetDeviceCaps",h,8)==960 && api("GetDeviceCaps",h,90)==96,"common DC truthful virtual metrics");
        require(api("ReleaseDC",owner,h)==1 && last_error==0x5a17,"matching common-DC release");
        require(!api("ReleaseDC",owner,h) && last_error==0x5a17,"repeat release preserves error");
        require(!api("GetDeviceCaps",h,8) && last_error==6,"released DC tombstone");}
    last_error=0x5a17;require(!api("GetDC",0x3333,0) && last_error==1400,"invalid HWND rejects without producing DC");
    last_error=0x5a17;require(!api("ReleaseDC",0x3333,0) && last_error==0x5a17,"NULL DC preserves error");
    require(!api("ReleaseDC",0x1111,0x1234) && last_error==1425,"foreign HDC native error");
    uint64_t a=api("GetDC",0x1111,0),b=api("GetDC",0x1111,0);require(a && b && a!=b,"simultaneous acquisitions have distinct leases");require(api("ReleaseDC",0x1111,a)==1 && api("ReleaseDC",0x1111,b)==1,"independent common releases");
    uint64_t private_dc=api("CreateDCW",(uintptr_t)driver,0);require(private_dc && api("DeleteDC",private_dc,0)==1,"private CreateDC/DeleteDC preserved");
#ifdef _WIN32
    HANDLE threads[4];for(unsigned i=0;i<4;++i){threads[i]=CreateThread(NULL,0,start,(void*)(uintptr_t)i,0,NULL);require(threads[i]!=NULL,"create native fixture worker");}for(unsigned i=0;i<4;++i){require(WaitForSingleObject(threads[i],10000)==WAIT_OBJECT_0,"join parallel workers");CloseHandle(threads[i]);}
#else
    pthread_t threads[4];for(unsigned i=0;i<4;++i)require(!pthread_create(&threads[i],NULL,start,(void*)(uintptr_t)i),"create native fixture worker");for(unsigned i=0;i<4;++i)require(!pthread_join(threads[i],NULL),"join parallel workers");
#endif
    uint64_t leases[256];unsigned count=0;while(count<256){uint64_t h=api("GetDC",0,0);if(!h)break;leases[count++]=h;}
    require(count==122 && last_error==8,"256 distinct lifetime DC capacity and explicit exhaustion");for(unsigned i=0;i<count;++i)require(api("ReleaseDC",0,leases[i])==1,"release all exhausted-pool live leases");
    win64_display_configure(0,0,0);printf("GETDC_FIXTURE_PASS checks=%u parallel_leases=128 lifetime_capacity=256 matching_owner=1 native_wrong_owner_parity=0 full_CPU_RSP=1 window_validator_reentry=1 original_game_exec=0\n",checks);return 0;
}
