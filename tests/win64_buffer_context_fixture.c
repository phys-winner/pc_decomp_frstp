/* Actual Core pointer/region code; section GC removes unrelated providers. */
#include "../android/host/win64.c"
static _Thread_local unsigned char fixture_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes) {
    uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>8||bytes>sizeof(fixture_teb)-offset)abort();
    memcpy(&value,fixture_teb+offset,bytes);return value;
}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes) {
    if(offset>sizeof(fixture_teb)||bytes>8||bytes>sizeof(fixture_teb)-offset)abort();
    memcpy(fixture_teb+offset,&value,bytes);
}
typedef void *(*dso_entry)(void *(*)(uint64_t,size_t,int),uint64_t,size_t,int,unsigned);
static void require(int okay,const char *why) {if(!okay){fprintf(stderr,"BUFFER_FIXTURE_FAIL %s\n",why);abort();}}
int main(int argc,char **argv) {
    require(argc==3,"DSO path and mode");
    void *dso=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);require(dso!=NULL,"dlopen fixture DSO");
    dso_entry entry=(dso_entry)dlsym(dso,"buffer_context_dso_entry");require(entry!=NULL,"DSO entry");
    Dl_info origin={0};require(dladdr((void *)(uintptr_t)entry,&origin)&&origin.dli_fbase,"DSO origin");
    static unsigned char data[256];memset(data,0xa5,sizeof(data));
    win64_region_add((uintptr_t)data,sizeof(data),1);
    if(!strcmp(argv[2],"success")) {
        CPU cpu,before;memset(&cpu,0x5a,sizeof(cpu));before=cpu;
        unsigned calls=0;win64_set_error(0x5a17);crt_errno=2718;errno=1927;
        for(unsigned depth=0;depth<8;++depth)for(unsigned n=0;n<32;++n) {
            require(entry(win64_pointer,(uintptr_t)data+n,n,0,depth)==data+n,"valid DSO pointer");++calls;
            require(win64_pointer((uintptr_t)data+n,n,1)==data+n,"valid direct pointer");++calls;
        }
        require(win64_pointer(0,0,0)==NULL,"unchanged zero-length behavior");++calls;
        require(!memcmp(&cpu,&before,sizeof(cpu))&&errno==1927&&crt_errno==2718&&win64_get_error()==0x5a17,"success CPU/errors");
        win64_region_remove((uintptr_t)data);require(!dlclose(dso),"DSO cleanup");
        printf("{\"success\":true,\"calls\":%u,\"actual_Core\":true,\"success_CPU_errors_preserved\":true,\"game_execution\":false}\n",calls);return 0;
    }
    fprintf(stderr,"BUFFER_FIXTURE_ORIGIN module=%s base=0x%" PRIxPTR " entry_offset=0x%" PRIxPTR "\n",
        origin.dli_fname,(uintptr_t)origin.dli_fbase,(uintptr_t)entry-(uintptr_t)origin.dli_fbase);
    if(!strcmp(argv[2],"direct"))win64_pointer(1,8,0);
    else if(!strcmp(argv[2],"dso"))entry(win64_pointer,1,8,0,2);
    else if(!strcmp(argv[2],"bounded"))entry(win64_pointer,1,8,0,32);
    else if(!strcmp(argv[2],"readonly")) {
        win64_region_add((uintptr_t)data,sizeof(data),0);entry(win64_pointer,(uintptr_t)data,8,1,1);
    } else if(!strcmp(argv[2],"overflow"))entry(win64_pointer,UINT64_MAX-3,8,0,1);
    else if(!strcmp(argv[2],"huge"))entry(win64_pointer,UINT64_C(0x721ffaa000),UINT64_C(27818509434880),0,1);
    else require(0,"known mode");
    return 99;
}
