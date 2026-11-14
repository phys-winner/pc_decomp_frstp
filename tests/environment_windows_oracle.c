#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    if(GetACP()!=65001)return 2;
    if(argc>1 && !strcmp(argv[1],"--null-name")) {
        __try {char b[8]={0};SetLastError(0x51cafe);DWORD n=GetEnvironmentVariableA(NULL,b,8);printf("{\"result\":%lu,\"error\":%lu}\n",n,GetLastError());return 0;}
        __except(EXCEPTION_EXECUTE_HANDLER) {puts("{\"native_SEH\":true}");return 86;}
    }
    if(argc==3 && !strcmp(argv[1],"--null-buffer")) {
        __try {SetLastError(0x51cafe);DWORD n=GetEnvironmentVariableA("SDL_AUDIODRIVER",NULL,(DWORD)strtoul(argv[2],NULL,10));printf("{\"result\":%lu,\"error\":%lu}\n",n,GetLastError());return 0;}
        __except(EXCEPTION_EXECUTE_HANDLER) {puts("{\"native_SEH\":true}");return 86;}
    }
    static const char *names[]={"SDL_AUDIODRIVER","sdl_audiodriver","TEMP","EMPTY","FROST_ENV_MISSING","","SDL_AUDIODRIVER=x","UTF8","LD_LIBRARY_PATH"};
    static const DWORD sizes[]={0,1,2,4,5,6,7,8,12,24,25,32,0x7fffffff,0xffffffff};
    static const DWORD errors[]={0,0x51cafe};
    unsigned count=0;puts("{\"ACP\":65001,\"rows\":[");
    for(unsigned k=0;k<sizeof(names)/sizeof(*names);++k)
    for(unsigned s=0;s<sizeof(sizes)/sizeof(*sizes);++s)
    for(unsigned e=0;e<sizeof(errors)/sizeof(*errors);++e) {
        unsigned char bytes[48];memset(bytes,0xa5,sizeof(bytes));errno=31337;SetLastError(errors[e]);
        DWORD value=GetEnvironmentVariableA(names[k],(char *)bytes+8,sizes[s]);DWORD error=GetLastError();int native_errno=errno;
        printf("%s{\"name\":\"%s\",\"capacity\":%lu,\"initial_error\":%lu,\"result\":%lu,\"error\":%lu,\"errno\":%d,\"bytes\":\"",count?",\n":"",names[k],sizes[s],errors[e],value,error,native_errno);
        for(unsigned i=0;i<sizeof(bytes);++i)printf("%02x",bytes[i]);fputs("\"}",stdout);++count;
    }
    printf("\n],\"cases\":%u,\"OS_environment_setters\":0}\n",count);return 0;
}
