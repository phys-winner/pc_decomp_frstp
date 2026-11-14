#include <dlfcn.h>
#include <stdio.h>
int main(int argc,char**argv){if(argc!=2)return 2;void*h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!h){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    int(*entry)(int,char**)=dlsym(h,"critical_tid_entry");if(!entry){fprintf(stderr,"dlsym: %s\n",dlerror());return 1;}int status=entry(1,argv);if(dlclose(h))return 1;return status;}
