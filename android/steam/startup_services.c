#include "startup_services.h"
#include "bridge.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>

int frostpunk_startup_services_init(const char *runtime,const char *profile,const char *game) {
    static void *backend;
    if (backend || !runtime || !profile || !game) return 0;
    char path[PATH_MAX];int count=snprintf(path,sizeof(path),"%s/libfrostpunk_offline_services.so",runtime);
    if (count<0 || (size_t)count>=sizeof(path)) return 0;
    void *handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if (!handle) { fprintf(stderr,"LOCAL_SERVICES dlopen failed: %s\n",dlerror());return 0; }
    int (*configure)(const char*,const char*)=dlsym(handle,"FrostpunkOffline_Configure");
    if (!configure || !configure(profile,game) || !win64_steam_configure(handle)) { dlclose(handle);return 0; }
    backend=handle;
    fprintf(stderr,"LOCAL_SERVICES_READY native_aarch64=1 local_files=1 network=0 cloud=0 authenticated_session=0\n");
    return 1;
}
