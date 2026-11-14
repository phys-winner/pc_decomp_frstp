/* Real Win64 pointer/LastError ABI and real bounded kernel filesystem. */
#include "../android/host/win64.c"
#include <fcntl.h>
#include <sys/stat.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&v,fixture_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t v,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&v,bytes);}
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"DRIVE_CURRENT_FAIL line=%d error=%u\n",__LINE__,win64_get_error());return 2;}}while(0)
static uint64_t api(const char *name,const uint64_t values[8]){
 uint64_t stack[10]={0};win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=values[0];c.rdx=values[1];c.r8=values[2];c.r9=values[3];for(unsigned i=4;i<8;++i)stack[i+1]=values[i];CPU saved=c;
 if(!win64_kernel_call(&c,name))abort();uint64_t result=c.rax;c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))abort();win64_region_remove((uintptr_t)stack);return result;
}
#define API(name,...) api((name),(uint64_t[8]){__VA_ARGS__})
int main(void){
 win64_kernel_roots_self_test();char base[]="/data/local/tmp/frostpunk/drive-c-test-XXXXXX",game[PATH_MAX],profile[PATH_MAX],native_file[PATH_MAX];CHECK(mkdtemp(base));
 snprintf(game,sizeof(game),"%s/game",base);snprintf(profile,sizeof(profile),"%s/profile",base);CHECK(!mkdir(game,0700)&&!mkdir(profile,0700));CHECK(win64_kernel_configure_roots(game,profile));
 snprintf(native_file,sizeof(native_file),"%s/game/fixture.bin",base);unsigned char bytes[]={0,1,0x1a,0xff},back[4]={0};int fd=open(native_file,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0&&write(fd,bytes,sizeof(bytes))==sizeof(bytes)&&!close(fd));
 uint16_t drive[]={'C',':',0},lower[]={'c',':',0};const char canonical[]="C:/Program Files (x86)/Steam/steamapps/common/Frostpunk",file_name[]="C:/Program Files (x86)/Steam/steamapps/common/Frostpunk/fixture.bin";
 win64_region_add((uintptr_t)drive,sizeof(drive),0);win64_region_add((uintptr_t)lower,sizeof(lower),0);win64_region_add((uintptr_t)canonical,sizeof(canonical),0);win64_region_add((uintptr_t)file_name,sizeof(file_name),0);win64_region_add((uintptr_t)back,sizeof(back),1);
 win64_set_error(0x5a17);uint64_t attributes=API("GetFileAttributesW",(uintptr_t)drive);CHECK(attributes!=UINT32_MAX&&(attributes&0x10)&&win64_get_error()==0x5a17);
 CHECK(API("GetFileAttributesA",(uintptr_t)canonical)==attributes&&API("GetFileAttributesW",(uintptr_t)lower)==attributes);
 const char *rejected[]={"C:fixture.bin","c:fixture.bin","C:.","C:..","C:/","C:\\","C:/fixture.bin","D:","//?/C:"};
 for(unsigned i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i){const char *path=rejected[i];win64_region_add((uintptr_t)path,strlen(path)+1,0);CHECK(API("GetFileAttributesA",(uintptr_t)path)==UINT32_MAX&&win64_get_error()==3);win64_region_remove((uintptr_t)path);}
 CHECK(!API("CreateDirectoryW",(uintptr_t)drive,0)&&win64_get_error()==5);
 CHECK(API("CreateFileA",(uintptr_t)file_name,0x40000000,7,0,3,0x80,0)==UINT64_MAX&&win64_get_error()==5);
 uint64_t file=API("CreateFileA",(uintptr_t)file_name,0x80000000,7,0,3,0x80,0);CHECK(file!=UINT64_MAX);uint32_t transferred=0;win64_region_add((uintptr_t)&transferred,4,1);
 CHECK(API("ReadFile",file,(uintptr_t)back,sizeof(back),(uintptr_t)&transferred,0)&&transferred==sizeof(back)&&!memcmp(bytes,back,sizeof(back)));CHECK(API("CloseHandle",file));
 CPU c={0},saved=c;CHECK(!win64_kernel_call(&c,"SetCurrentDirectoryW")&&!memcmp(&c,&saved,sizeof(c)));
 /* Complete existing kernel tests keep profile aliases, no-follow, mappings,
  * events and anchored write policies covered on this exact changed source. */
 win64_kernel_self_test();CHECK(API("GetFileAttributesW",(uintptr_t)drive)==attributes);
 CHECK(!unlink(native_file)&&!rmdir(game)&&!rmdir(profile)&&!rmdir(base));
 win64_region_remove((uintptr_t)&transferred);win64_region_remove((uintptr_t)back);win64_region_remove((uintptr_t)file_name);win64_region_remove((uintptr_t)canonical);win64_region_remove((uintptr_t)lower);win64_region_remove((uintptr_t)drive);
 printf("{\"success\":true,\"checks\":%u,\"actual_configured_game_directory\":true,\"exact_C_only\":true,\"drive_relative_and_root_rejected\":true,\"game_writes_denied\":true,\"canonical_game_read\":true,\"kernel_and_roots_regression\":true,\"CPU_RSP_LastError\":true}\n",checks);return 0;
}
