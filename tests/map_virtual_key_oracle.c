/* Query only newly compiled Windows API calls; never executes game code. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
static unsigned checks;
static UINT query(UINT code,HKL layout) {
    SetLastError(0x5a17);errno=2718;
    UINT value=MapVirtualKeyExA(code,MAPVK_VK_TO_VSC_EX,layout);
    if(GetLastError()!=0x5a17 || errno!=2718) {
        fprintf(stderr,"MAPVK_ORACLE_FAIL code=%u error=%lu errno=%d\n",code,GetLastError(),errno);ExitProcess(1);
    }
    ++checks;return value;
}
int main(void) {
    HKL current=GetKeyboardLayout(0),layouts[64],us=NULL;
    int count=GetKeyboardLayoutList(64,layouts),owned=0;
    for(int i=0;i<count && i<64;++i)if((uintptr_t)layouts[i]==UINT32_C(0x04090409))us=layouts[i];
    if(!us){us=LoadKeyboardLayoutW(L"00000409",0);owned=1;}
    if(!us){fprintf(stderr,"MAPVK_ORACLE_FAIL US layout error=%lu\n",GetLastError());return 1;}
    UINT table[256];unsigned null_differences=0;
    for(unsigned i=0;i<256;++i){table[i]=query(i,us);if(query(i,NULL)!=table[i])++null_differences;}
    unsigned unmapped=0;
    for(UINT i=256;i<65536;++i){if(query(i,us)){fprintf(stderr,"MAPVK_ORACLE_FAIL high16=%u\n",i);return 1;}++unmapped;}
    uint32_t random=UINT32_C(0x847359a1);
    for(unsigned i=0;i<8192;++i){random=random*UINT32_C(1664525)+UINT32_C(1013904223);UINT code=random|UINT32_C(0x10000);if(query(code,us)){fprintf(stderr,"MAPVK_ORACLE_FAIL high32=%u\n",code);return 1;}++unmapped;}
    UINT edge[]={UINT32_C(0xffffffff),UINT32_C(0x80000000),UINT32_C(0x1000000d),UINT32_C(0x10000041)};
    for(unsigned i=0;i<sizeof(edge)/sizeof(edge[0]);++i){if(query(edge[i],us))return 1;++unmapped;}
    int layout_unchanged=GetKeyboardLayout(0)==current;
    printf("{\"success\":true,\"layout\":\"00000409\",\"type\":4,\"checks\":%u,\"unmapped_high_codes\":%u,\"null_layout_differences\":%u,\"current_layout_unchanged\":%s,\"LastError\":23063,\"errno\":2718,\"table\":[",checks,unmapped,null_differences,layout_unchanged?"true":"false");
    for(unsigned i=0;i<256;++i)printf("%s%u",i?",":"",table[i]);
    printf("]}\n");if(owned)UnloadKeyboardLayout(us);return !layout_unchanged;
}
