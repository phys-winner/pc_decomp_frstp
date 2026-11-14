/* Load a real PIC region fixture so Bionic must resolve DSO TLS dynamically. */
#include <dlfcn.h>
#include <stdio.h>
int main(int argc,char**argv){
 if(argc<2){fprintf(stderr,"usage: region-layout-loader shared-library [fixture-argument]\n");return 64;}
 void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!library){fprintf(stderr,"REGION_LAYOUT_DLOPEN %s\n",dlerror());return 1;}
 int(*entry)(int,char**)=(int(*)(int,char**))dlsym(library,"region_layout_entry");
 if(!entry){fprintf(stderr,"REGION_LAYOUT_SYMBOL %s\n",dlerror());return 1;}
 int result=entry(argc-1,argv+1);dlclose(library);return result;
}
