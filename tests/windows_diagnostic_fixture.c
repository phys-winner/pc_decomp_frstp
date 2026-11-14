#define wmain fp_unused_host_wmain
#include "../native/windows/host.c"
#undef wmain
#include <assert.h>
#include "windows_static_dispatch_cases.inc"

static unsigned cases;
static void check(CPU *c,const char *name,int gfx_errno) {
    CPU before=*c;SetLastError(0xe5ef1234);errno=EDOM;
    native_diagnostic(c,name);
    assert(!memcmp(c,&before,sizeof(*c)) && GetLastError()==0xe5ef1234);
    if(gfx_errno)assert(errno==EDOM);++cases;
}
int wmain(int argc,wchar_t **argv) {
    unsigned char *area=VirtualAlloc((void *)(uintptr_t)UINT64_C(0x500000000),65536,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);assert(area);
    fp_native_validation_init();CPU c;memset(&c,0x5a,sizeof(c));c.rsp=(uintptr_t)(area+128);
    wr64(c.rsp,UINT64_C(0xe5efcafe));
    if(argc==2 && !wcsncmp(argv[1],L"--static-",9))return static_boundary_cases(argv[1],area);
    if(argc==2) {
        if(!wcscmp(argv[1],L"--invalid-debug-a")){c.rcx=1;native_diagnostic(&c,"OutputDebugStringA");}
        else if(!wcscmp(argv[1],L"--invalid-debug-w")){c.rcx=1;native_diagnostic(&c,"OutputDebugStringW");}
        else if(!wcscmp(argv[1],L"--invalid-exit")){c.rsp=1;native_diagnostic(&c,"exit");}
        else if(!wcscmp(argv[1],L"--invalid-cleanup")){c.rsp=1;native_diagnostic(&c,"_cexit");}
        assert(!"invalid guest pointer accepted");
    }
    const char *quiet[]={"","CreateFileW_extra","OutputDebugString","OutputDebugStringA_extra","exit_extra","_cexit_extra","GetFileAttributesExW_extra","_wfopen_s_extra","fopen_s_extra","GetModuleHandleW","GetProcAddress","Present"};
    c.rcx=c.rdx=c.rsp=1;
    for(unsigned i=0;i<sizeof(quiet)/sizeof(*quiet);++i)check(&c,quiet[i],1);
    CPU before=c;SetLastError(0xe5ef1234);errno=EDOM;gfx_file_diagnostic(&c,"OutputDebugStringA");
    assert(!memcmp(&c,&before,sizeof(c)) && GetLastError()==0xe5ef1234 && errno==EDOM);
    c.rsp=(uintptr_t)(area+128);
    char *ansi=(char *)(area+4096);wchar_t *wide=(wchar_t *)(area+8192);
    strcpy(ansi,"diagnostic ANSI");wcscpy(wide,L"diagnostic Unicode \u0416");
    c.rcx=(uintptr_t)ansi;check(&c,"OutputDebugStringA",0);
    c.rcx=(uintptr_t)wide;check(&c,"OutputDebugStringW",0);
    c.rcx=0;check(&c,"OutputDebugStringA",0);check(&c,"OutputDebugStringW",0);
    ansi[0]=0;wide[0]=0;c.rcx=(uintptr_t)ansi;check(&c,"OutputDebugStringA",0);
    c.rcx=(uintptr_t)wide;check(&c,"OutputDebugStringW",0);
    memset(ansi,'x',4096);for(unsigned i=0;i<4096;++i)wide[i]=L'y';
    c.rcx=(uintptr_t)ansi;check(&c,"OutputDebugStringA",0);
    c.rcx=(uintptr_t)wide;check(&c,"OutputDebugStringW",0);
    c.rcx=UINT64_C(0x100000011);check(&c,"ExitProcess",0);check(&c,"exit",0);check(&c,"_exit",0);check(&c,"_cexit",0);
    c.rcx=c.rdx=1;check(&c,"CreateFileW",1);check(&c,"fopen_s",1);
    strcpy(ansi,"C:\\own\\ordinary.ini");wcscpy(wide,L"C:\\own\\ordinary.ini");
    c.rcx=(uintptr_t)wide;c.rdx=(uintptr_t)ansi;check(&c,"CreateFileW",1);check(&c,"fopen_s",1);
    strcpy(ansi,"C:\\own\\prefixGfXcOnFiGsuffix.ini");wcscpy(wide,L"C:\\own\\prefixGfXcOnFiGsuffix.ini");
    const char *gfx[]={"CreateFileW","_wfopen","GetFileAttributesW","GetFileAttributesExW","fopen_s","_wfopen_s"};
    for(unsigned i=0;i<6;++i){c.rcx=i==5?1:(uintptr_t)wide;c.rdx=(uintptr_t)(i==5?(void *)wide:(void *)ansi);check(&c,gfx[i],1);}
    for(unsigned i=0;i<1024;++i)wide[i]=L'z';memcpy(wide,L"gfxconfig",9*sizeof(wchar_t));
    c.rcx=(uintptr_t)wide;check(&c,"CreateFileW",1);
    wcscpy(wide,L"C:\\own\\GfxConfig.ini");check(&c,"CreateFileW",1);check(&c,"CreateFileW",1);check(&c,"CreateFileW",1);
    printf("{\"success\":true,\"cases\":%u,\"full_CPU_LastError\":true,\"gfx_errno\":true}\n",cases);
    assert(VirtualFree(area,0,MEM_RELEASE));return 0;
}
