/* Actual core validator/LastError; exact extracted route with unrelated mocks. */
#define win64_dispatch_import unused_full_core_dispatch
#include "../android/host/win64.c"
#undef win64_dispatch_import
#include "win64_shell.h"
static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t off,unsigned n){uint64_t v=0;if(off>sizeof(fixture_teb)||n>sizeof(fixture_teb)-off||n>8)abort();memcpy(&v,fixture_teb+off,n);return v;}
void frostpunk_teb_write(uint64_t off,uint64_t v,unsigned n){if(off>sizeof(fixture_teb)||n>sizeof(fixture_teb)-off||n>8)abort();memcpy(fixture_teb+off,&v,n);}
static unsigned filesystem_calls;
void win64_window_drag_accept_files(CPU*c){(void)c;abort();} /* Not used by this icon-only fixture. */
int win64_kernel_call(CPU*c,const char*n){(void)c;(void)n;++filesystem_calls;return 0;}
static int unrelated_import(CPU*c,const char*d,const char*n){(void)c;(void)d;(void)n;return 0;}
static int unrelated_api(CPU*c,const char*n){(void)c;(void)n;return 0;}
static int unrelated_notification(CPU*c){(void)c;return 0;}
#include "shell_router.inc"
static _Thread_local unsigned checks,calls;
static void check(int ok,const char *reason){++checks;if(!ok){fprintf(stderr,"EXTRACT_ICON_FIXTURE_FAIL %s\n",reason);_Exit(1);}}
typedef struct{uint64_t stack[8];unsigned char icons[48];unsigned char filename[520];} workspace;
static void setup(workspace*w,unsigned length){
    memset(w,0xa5,sizeof(*w));for(unsigned i=0;i<length;++i){uint16_t ch=i&1?0xd800:'x';memcpy(w->filename+i*2,&ch,2);}uint16_t zero=0;memcpy(w->filename+length*2,&zero,2);w->stack[5]=UINT64_C(0xfeca123400000001);
}
static void request(workspace*w,unsigned outputs,int route,int alias){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=(uintptr_t)w->filename;c.rdx=UINT64_C(0xfeca123400000000);c.r8=outputs&1?(uintptr_t)(w->icons+9):0;c.r9=outputs&2?(uintptr_t)(w->icons+(alias?9:25)):0;c.rsp=(uintptr_t)w->stack;before=c;
    unsigned char expected[48];memcpy(expected,w->icons,sizeof(expected));if(c.r8)memset(expected+9,0,8);if(c.r9)memset(expected+(alias?9:25),0,8);
    win64_set_error(0x5a17);win64_stdio_set_errno(4321);errno=31337;
    int handled=route?win64_dispatch_import(&c,route==1?"SHELL32.DLL":"shell32.dll","ExtractIconExW"):win64_shell_call(&c,"ExtractIconExW");
    check(handled&&c.rax==UINT32_MAX,"genuine UINT32 failure/zeroextend");check(win64_get_error()==50&&crt_errno==4321&&errno==31337,"error50/errno");check(!memcmp(w->icons,expected,sizeof(expected)),"NULLiconoutputs/guards/alias");
    c.rax=before.rax;if(route)before.rsp+=8;check(!memcmp(&c,&before,sizeof(c)),"fullCPU/singlepop");++calls;
}
static void *worker(void *unused){(void)unused;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);for(unsigned n=0;n<256;++n){setup(&w,n%3);request(&w,n%4,n%3,n&1);}win64_region_remove((uintptr_t)&w);return(void*)(uintptr_t)checks;}
int main(int argc,char**argv){
    win64_shell_self_test();workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);setup(&w,1);
    if(argc>1){CPU c={0};c.rcx=(uintptr_t)w.filename;c.rdx=0;c.r8=(uintptr_t)(w.icons+9);c.r9=(uintptr_t)(w.icons+25);c.rsp=(uintptr_t)w.stack;w.stack[5]=1;
        if(!strcmp(argv[1],"--index"))c.rdx=UINT32_MAX;else if(!strcmp(argv[1],"--count"))w.stack[5]=2;
        else if(!strcmp(argv[1],"--bad-path"))c.rcx=1;else if(!strcmp(argv[1],"--long-path"))memset(w.filename,'x',sizeof(w.filename));
        else if(!strcmp(argv[1],"--bad-large"))c.r8=1;else if(!strcmp(argv[1],"--short-output")){unsigned char bytes[7];win64_region_add((uintptr_t)bytes,7,1);c.r8=(uintptr_t)bytes;}
        else if(!strcmp(argv[1],"--readonly")){unsigned char bytes[8];win64_region_add((uintptr_t)bytes,8,0);c.r8=(uintptr_t)bytes;}else return 2;
        win64_shell_call(&c,"ExtractIconExW");return 99;
    }
    for(unsigned length=0;length<3;++length)for(unsigned outputs=0;outputs<4;++outputs)for(unsigned route=0;route<3;++route)for(int alias=0;alias<2;++alias){setup(&w,length==2?259:length);request(&w,outputs,route,alias);}
    CPU c,b;memset(&c,0xa5,sizeof(c));b=c;win64_set_error(17);win64_stdio_set_errno(4321);errno=31337;
    check(!win64_dispatch_import(&c,"user32.dll","ExtractIconExW")&&!memcmp(&c,&b,sizeof(c))&&win64_get_error()==17,"DLLscope");
    check(!win64_shell_call(&c,"ExtractIconExA")&&!memcmp(&c,&b,sizeof(c)),"Wonly/unknown untouched");
    pthread_t threads[4];for(unsigned n=0;n<4;++n)check(!pthread_create(&threads[n],NULL,worker,NULL),"threadcreate");unsigned parallel_checks=0;for(unsigned n=0;n<4;++n){void*v;check(!pthread_join(threads[n],&v),"threadjoin");parallel_checks+=(unsigned)(uintptr_t)v;}
    check(filesystem_calls==0,"no filesystem operation/provider bypass");win64_region_remove((uintptr_t)&w);
    printf("{\"success\":true,\"checks\":%u,\"calls\":%u,\"parallel_calls\":1024,\"parallel_checks\":%u,\"no_filesystem_calls\":true,\"full_CPU_single_pop\":true,\"explicit_unavailable50\":true,\"game_boot_proven\":false}\n",checks,calls,parallel_checks);return 0;
}
