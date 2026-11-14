/* Read-only USER32 rectangle predicate; no window, cursor or desktop actions. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef struct {RECT rect;uint32_t error;int32_t crt_error;} input_row;
typedef struct {uint64_t raw_rax;uint32_t error;int32_t crt_error;uint32_t rect_unchanged,canaries_unchanged;} output_row;
typedef uint64_t (WINAPI *raw_predicate)(const RECT *);
typedef int *(__cdecl *errno_query)(void);
_Static_assert(sizeof(RECT)==16 && sizeof(LONG)==4,"Win64 RECT/LONG");
_Static_assert(offsetof(RECT,right)==8 && offsetof(RECT,bottom)==12,"RECT fields");
_Static_assert(sizeof(input_row)==24 && sizeof(output_row)==24,"Oracle row sizes");
int main(int argc,char **argv) {
    HMODULE user=LoadLibraryW(L"C:\\Windows\\System32\\user32.dll");if(!user)return 2;
    FARPROC symbol=GetProcAddress(user,"IsRectEmpty");raw_predicate predicate=NULL;
    _Static_assert(sizeof(predicate)==sizeof(symbol),"Raw return-width function pointer");
    memcpy(&predicate,&symbol,sizeof(predicate));if(!predicate)return 3;
    HMODULE ucrt=LoadLibraryW(L"C:\\Windows\\System32\\ucrtbase.dll");if(!ucrt)return 8;
    FARPROC errno_symbol=GetProcAddress(ucrt,"_errno");errno_query query=NULL;
    memcpy(&query,&errno_symbol,sizeof(query));if(!query)return 9;int *native_errno=query();
    if(argc==2 && !strcmp(argv[1],"--memory")) {
        SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
        unsigned char *memory=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!memory)return 10;
        DWORD old;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old))return 11;
        const RECT rectangles[]={{0,0,960,540},{1,0,0,1},{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX}};
        unsigned rows=0;putchar('[');
        for(unsigned mode=0;mode<3;++mode)for(unsigned k=0;k<3;++k) {
            if(!VirtualProtect(memory,page,PAGE_READWRITE,&old))return 12;
            memset(memory,0xa5,page);size_t offset=mode==2?page-sizeof(RECT):mode==1?7:0;
            memcpy(memory+offset,&rectangles[k],sizeof(RECT));
            if(!VirtualProtect(memory,page,PAGE_READONLY,&old))return 13;
            SetLastError(0x5a17);*native_errno=1927;uint64_t result=predicate((const RECT *)(memory+offset));
            int crt_error=*native_errno;DWORD error=GetLastError();
            printf("%s{\"mode\":%u,\"offset\":%zu,\"raw_rax\":%llu,\"LastError\":%lu,\"errno\":%d,\"rect_unchanged\":%s}",
                rows++?",":"",mode,offset,(unsigned long long)result,(unsigned long)error,crt_error,
                memcmp(memory+offset,&rectangles[k],sizeof(RECT))?"false":"true");
        }
        puts("]");if(!VirtualFree(memory,0,MEM_RELEASE))return 14;return 0;
    }
    if(argc==2 && !strcmp(argv[1],"--null")) {
        uint64_t result=0;uint32_t exception=0;SetLastError(0x5a17);*native_errno=1927;
        __try {result=predicate(NULL);} __except(EXCEPTION_EXECUTE_HANDLER) {exception=GetExceptionCode();}
        int crt_error=*native_errno;DWORD error=GetLastError();
        printf("{\"returned\":%s,\"raw_rax\":%llu,\"exception_code\":%u,\"LastError\":%lu,\"errno\":%d}\n",
            exception?"false":"true",(unsigned long long)result,exception,(unsigned long)error,crt_error);return 0;
    }
    if(argc!=3)return 4;
    FILE *input=NULL,*output=NULL;if(fopen_s(&input,argv[1],"rb")||fopen_s(&output,argv[2],"wb"))return 5;
    input_row row;unsigned rows=0;
    while(fread(&row,sizeof(row),1,input)==1) {
        struct {uint64_t before;RECT rect;uint64_t after;} guarded={UINT64_C(0x123456789abcdef0),{0},UINT64_C(0xfedcba9876543210)};
        guarded.rect=row.rect;SetLastError(row.error);*native_errno=row.crt_error;
        uint64_t result=predicate(&guarded.rect);int crt_error=*native_errno;DWORD error=GetLastError();
        output_row observed={result,error,crt_error,!memcmp(&guarded.rect,&row.rect,sizeof(RECT)),
            guarded.before==UINT64_C(0x123456789abcdef0)&&guarded.after==UINT64_C(0xfedcba9876543210)};
        if(fwrite(&observed,sizeof(observed),1,output)!=1)return 6;++rows;
    }
    if(ferror(input)||fclose(input)||fclose(output))return 7;
    printf("{\"rows\":%u,\"RECT_bytes\":16,\"LONG_bits\":32,\"raw_RAX_capture\":true}\n",rows);return 0;
}
