/* Native API contract only. Never loads or executes the original game. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void) {
    char name[257]; DWORD needed=sizeof(name);
    if(!GetUserNameA(name,&needed)||!needed||needed>sizeof(name))return 2;
    for(DWORD i=0;i<needed-1;++i)if((unsigned char)name[i]>=128)return 3;
    printf("{\"success\":true,\"native_required\":%lu,\"native_ASCII\":true,\"name_not_exported\":true,\"rows\":[",(unsigned long)needed);
    unsigned row=0;
    const DWORD sizes[]={0,1,2,7,8,needed-1,needed,needed+1,257,0xffffffffu};
    for(unsigned null_buffer=0;null_buffer<2;++null_buffer)
    for(unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);++i) {
        unsigned char bytes[288];memset(bytes,0xa5,sizeof(bytes));DWORD size=sizes[i];BOOL result=0;DWORD exception=0;
        SetLastError(0x1234);errno=2345;
        __try {result=GetUserNameA(null_buffer?NULL:(char*)bytes+8,&size);}
        __except(EXCEPTION_EXECUTE_HANDLER){exception=GetExceptionCode();}
        DWORD error=GetLastError();int native_errno=errno;
        int unchanged=1,exact=!null_buffer&&result;
        for(unsigned j=0;j<sizeof(bytes);++j)if(bytes[j]!=0xa5)unchanged=0;
        if(exact) {
            exact=!memcmp(bytes+8,name,needed);
            for(unsigned j=0;j<sizeof(bytes);++j)if((j<8||j>=8+needed)&&bytes[j]!=0xa5)exact=0;
        }
        printf("%s{\"null_buffer\":%s,\"capacity\":%lu,\"result\":%d,\"size_after\":%lu,\"error\":%lu,\"errno\":%d,\"exception\":%lu,\"unchanged\":%s,\"exact_name_only\":%s}",row++?",":"",null_buffer?"true":"false",(unsigned long)sizes[i],result!=0,(unsigned long)size,(unsigned long)error,native_errno,(unsigned long)exception,unchanged?"true":"false",exact?"true":"false");
    }
    unsigned char bytes[288];memset(bytes,0xa5,sizeof(bytes));BOOL result=0;DWORD exception=0;SetLastError(0x1234);errno=2345;
    __try {result=GetUserNameA((char*)bytes+8,NULL);}
    __except(EXCEPTION_EXECUTE_HANDLER){exception=GetExceptionCode();}
    printf("],\"null_size\":{\"result\":%d,\"error\":%lu,\"errno\":%d,\"exception\":%lu},\"overlap\":[",result!=0,(unsigned long)GetLastError(),errno,(unsigned long)exception);
    for(unsigned offset=0;offset<12;++offset) {
        unsigned char actual[64],expected[64];memset(actual,0xa5,sizeof(actual));DWORD size=257;memcpy(actual+16+offset,&size,4);memcpy(expected,actual,sizeof(actual));
        SetLastError(0x1234);errno=2345;result=GetUserNameA((char*)actual+16,(DWORD*)(actual+16+offset));
        memcpy(expected+16,name,needed);memcpy(expected+16+offset,&needed,4);
        printf("%s{\"size_offset\":%u,\"result\":%d,\"error\":%lu,\"errno\":%d,\"name_then_size\":%s}",offset?",":"",offset,result!=0,(unsigned long)GetLastError(),errno,!memcmp(actual,expected,sizeof(actual))?"true":"false");
    }
    printf("]}\n");
    return 0;
}
