/* Loads two isolated region implementations with actual shared-library TLS. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE library;
static library load(const char *p){return LoadLibraryA(p);}
static void *symbol(library h,const char *p){return (void*)GetProcAddress(h,p);}
static int close_library(library h){return !FreeLibrary(h);}
#else
#include <dlfcn.h>
typedef void *library;
static library load(const char *p){return dlopen(p,RTLD_NOW|RTLD_LOCAL);}
static void *symbol(library h,const char *p){return dlsym(h,p);}
static int close_library(library h){return dlclose(h);}
#endif
typedef int(*cycle)(unsigned,unsigned,unsigned,uint64_t*,uint64_t*);
int main(int argc,char **argv){
    if(argc!=3)return 2;library a=load(argv[1]),b=load(argv[2]);if(!a||!b)return 3;
    cycle functions[2]={(cycle)symbol(a,"fp_avl_cycle"),(cycle)symbol(b,"fp_avl_cycle")};
    if(!functions[0]||!functions[1])return 4;
    const unsigned scenarios[][3]={{64,0,20000},{4096,0,20000},{32768,0,20000},{4096,1,10000},{4096,2,10000},{4096,3,4096}};
    printf("{\"success\":true,\"trials\":5,\"whole_add_validate_remove\":true,\"occupancies_synthetic\":true,\"scenarios\":[");
    for(unsigned s=0;s<6;++s){uint64_t elapsed[2][5],expected=0;
        for(unsigned trial=0;trial<5;++trial)for(unsigned order=0;order<2;++order){
            unsigned which=(trial+order)&1;uint64_t digest;
            if(functions[which](scenarios[s][0],scenarios[s][1],scenarios[s][2],&elapsed[which][trial],&digest))return 5;
            if(!trial&&!order)expected=digest;else if(digest!=expected)return 6;
        }
        printf("%s{\"occupancy\":%u,\"mode\":%u,\"cycles\":%u,\"digest\":\"%016"PRIx64"\",\"baseline_ns\":[",s?",":"",scenarios[s][0],scenarios[s][1],scenarios[s][2],expected);
        for(unsigned i=0;i<5;++i)printf("%s%"PRIu64,i?",":"",elapsed[0][i]);
        printf("],\"candidate_ns\":[");for(unsigned i=0;i<5;++i)printf("%s%"PRIu64,i?",":"",elapsed[1][i]);printf("]}");
    }
    printf("],\"game_gain_proven\":false}\n");if(close_library(a)||close_library(b))return 7;return 0;
}
