/* Real production pointer validation, allocators, LastError and kernel paths. */
#include "../android/host/win64.c"
#include "win64_shell.h"
#include <fcntl.h>
#include <sys/stat.h>
void win64_window_drag_accept_files(CPU*c){(void)c;abort();} /* Unrelated registration is tested with the real window provider separately. */
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t value=0;if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(&value,fixture_teb+offset,bytes);return value;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset>sizeof(fixture_teb)||bytes>sizeof(fixture_teb)-offset||bytes>8)abort();memcpy(fixture_teb+offset,&value,bytes);}
static _Atomic unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"SHELL_FIXTURE_FAIL line=%d LastError=%u\n",__LINE__,win64_get_error());return 2;}}while(0)
typedef struct{uint64_t stack[9];uint16_t out[266];} workspace;
static uint32_t folder(workspace *w,uint32_t csidl,uint64_t token,uint32_t type,uint64_t output){
 CPU c;memset(&c,0xa5,sizeof(c));c.rcx=0;c.rdx=csidl;c.r8=token;c.r9=type;c.rsp=(uintptr_t)w->stack;w->stack[5]=output;CPU saved=c;
 if(!win64_shell_call(&c,"SHGetFolderPathW"))abort();uint64_t result=c.rax;c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c))||result>UINT32_MAX)abort();return(uint32_t)result;
}
static uint64_t fs(workspace *w,const char *name,const uint64_t args[8]){
 CPU c={0};c.rsp=(uintptr_t)w->stack;c.rcx=args[0];c.rdx=args[1];c.r8=args[2];c.r9=args[3];for(unsigned i=4;i<8;++i)w->stack[i+1]=args[i];CPU saved=c;
 if(!win64_kernel_call(&c,name))abort();uint64_t result=c.rax;c.rax=saved.rax;if(memcmp(&c,&saved,sizeof(c)))abort();return result;
}
#define FS(w,n,...) fs((w),(n),(uint64_t[8]){__VA_ARGS__})
static size_t ascii_wide(uint16_t *out,const char *text){size_t n=strlen(text);for(size_t i=0;i<=n;++i)out[i]=(unsigned char)text[i];return n;}
static int checkpath(workspace *w,uint32_t id){
 memset(w->out,0xa5,sizeof(w->out));win64_set_error(0x5a17);crt_errno=1234;
 CHECK(folder(w,id,0,0,(uintptr_t)w->out)==0&&win64_get_error()==0x5a17&&crt_errno==1234);
 const char *wanted=(id&255)==26?"C:\\FrostpunkProfile\\AppData":"C:\\FrostpunkProfile\\Pictures";size_t n=strlen(wanted);
 for(size_t i=0;i<=n;++i)CHECK(w->out[i]==(unsigned char)wanted[i]);for(size_t i=n+1;i<266;++i)CHECK(w->out[i]==0xa5a5);
 CHECK(w->out[n-1]!='\\');return 0;
}
static void *query_worker(void *unused){(void)unused;workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
 for(unsigned i=0;i<1000;++i)if(checkpath(&w,(i&1)?0x801a:0x27))abort();win64_region_remove((uintptr_t)&w);return NULL;}
int main(int argc,char **argv){
 win64_shell_self_test();workspace w;win64_region_add((uintptr_t)&w,sizeof(w),1);
 if(argc>1){if(!strcmp(argv[1],"bad-output"))folder(&w,0x801a,0,0,1);else if(!strcmp(argv[1],"short-output")){uint16_t small[259];win64_region_add((uintptr_t)small,sizeof(small),1);folder(&w,0x27,0,0,(uintptr_t)small);}else return 3;return 99;}
 win64_kernel_roots_self_test();
 char root[]="/data/local/tmp/frostpunk/shell-test-XXXXXX",game[PATH_MAX],profile[PATH_MAX],name[PATH_MAX];CHECK(mkdtemp(root));snprintf(game,sizeof(game),"%s/game",root);snprintf(profile,sizeof(profile),"%s/profile",root);
 CHECK(!mkdir(game,0700)&&!mkdir(profile,0700));CHECK(win64_kernel_configure_roots(game,profile));
 /* No-CREATE query before explicit provisioning must not invent a directory. */
 memset(w.out,0xa5,sizeof(w.out));win64_set_error(0x5a17);CHECK(folder(&w,0x27,0,0,(uintptr_t)w.out)==0x80070002&&w.out[0]==0&&w.out[1]==0xa5a5&&win64_get_error()==0x5a17);
 snprintf(name,sizeof(name),"%s/Pictures",profile);CHECK(access(name,F_OK)&&errno==ENOENT);
 CHECK(win64_shell_provision());CHECK(win64_shell_provision());CHECK(!checkpath(&w,0x801a)&&!checkpath(&w,0x27));
 uint32_t invalid[]={0x1001a,0x7fff,0xffff};for(unsigned i=0;i<3;++i){memset(w.out,0xa5,sizeof(w.out));CHECK(folder(&w,invalid[i],0,0,(uintptr_t)w.out)==0x80070057&&w.out[0]==0xa5a5);}
 CHECK(folder(&w,0x801a,0,0,0)==0x80070057);
 memset(w.out,0xa5,sizeof(w.out));CHECK(folder(&w,0x1a,1,0,(uintptr_t)w.out)==0x80004001&&w.out[0]==0&&w.out[1]==0xa5a5);
 CHECK(folder(&w,0x1a,0,1,(uintptr_t)w.out)==0x80004001&&folder(&w,5,0,0,(uintptr_t)w.out)==0x80004001);
 /* Real game appends vendor/project, then real kernel directory and byte I/O. */
 const char *relative[]={"AppData/11bitstudios","AppData/11bitstudios/Frostpunk","Pictures/Frostpunk"};
 for(unsigned i=0;i<3;++i){snprintf(name,sizeof(name),"C:\\FrostpunkProfile\\%s",relative[i]);ascii_wide(w.out,name);CHECK(FS(&w,"CreateDirectoryW",(uintptr_t)w.out,0));}
 ascii_wide(w.out,"C:\\FrostpunkProfile\\AppData\\11bitstudios\\Frostpunk\\shell-fixture.bin");uint64_t file=FS(&w,"CreateFileW",(uintptr_t)w.out,0xc0000000,7,0,1,0x80,0);CHECK(file!=UINT64_MAX);
 unsigned char payload[]={0,1,0x1a,0xff,0x80,7},back[sizeof(payload)]={0};uint32_t bytes=0;win64_region_add((uintptr_t)payload,sizeof(payload),0);win64_region_add((uintptr_t)back,sizeof(back),1);win64_region_add((uintptr_t)&bytes,4,1);
 CHECK(FS(&w,"WriteFile",file,(uintptr_t)payload,sizeof(payload),(uintptr_t)&bytes,0)&&bytes==sizeof(payload));CHECK(FS(&w,"SetFilePointerEx",file,0,0,0));
 CHECK(FS(&w,"ReadFile",file,(uintptr_t)back,sizeof(back),(uintptr_t)&bytes,0)&&bytes==sizeof(payload)&&!memcmp(back,payload,sizeof(payload)));CHECK(FS(&w,"CloseHandle",file));
 snprintf(name,sizeof(name),"%s/AppData/11bitstudios/Frostpunk/shell-fixture.bin",profile);int fd=open(name,O_RDONLY|O_CLOEXEC);CHECK(fd>=0&&read(fd,back,sizeof(back))==sizeof(back)&&!memcmp(back,payload,sizeof(back)));CHECK(!close(fd));
 /* Alias boundary, traversal and read-only game anchor remain enforced. */
 ascii_wide(w.out,"C:\\FrostpunkProfileEvil\\escape.bin");CHECK(FS(&w,"CreateFileW",(uintptr_t)w.out,0x40000000,7,0,1,0x80,0)==UINT64_MAX&&win64_get_error()==3);
 ascii_wide(w.out,"C:\\FrostpunkProfile\\..\\escape.bin");CHECK(FS(&w,"CreateFileW",(uintptr_t)w.out,0x40000000,7,0,1,0x80,0)==UINT64_MAX&&win64_get_error()==5);
 /* A deleted non-CREATE directory remains absent; CREATE restores it. */
 snprintf(name,sizeof(name),"%s/Pictures/Frostpunk",profile);CHECK(!rmdir(name));snprintf(name,sizeof(name),"%s/Pictures",profile);CHECK(!rmdir(name));
 CHECK(folder(&w,0x27,0,0,(uintptr_t)w.out)==0x80070002&&w.out[0]==0&&access(name,F_OK));CHECK(!checkpath(&w,0x8027));
 pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,query_worker,NULL));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
 /* Replace the known directory with a file: never report that as a folder. */
 CHECK(!rmdir(name));fd=open(name,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0&&!close(fd));CHECK(folder(&w,0x27,0,0,(uintptr_t)w.out)==0x8007010b&&w.out[0]==0);CHECK(!unlink(name));
 CHECK(!symlink(game,name));CHECK(folder(&w,0x27,0,0,(uintptr_t)w.out)==0x80070005&&w.out[0]==0);
 CHECK(folder(&w,0x8027,0,0,(uintptr_t)w.out)==0x80070005&&w.out[0]==0);CHECK(!win64_shell_provision()&&win64_get_error()==5);CHECK(!unlink(name));
 snprintf(name,sizeof(name),"%s/escape",profile);CHECK(!symlink(game,name));ascii_wide(w.out,"C:\\FrostpunkProfile\\escape\\outside.bin");
 CHECK(FS(&w,"CreateFileW",(uintptr_t)w.out,0x40000000,7,0,1,0x80,0)==UINT64_MAX&&win64_get_error()==3);
 CHECK(!unlink(name));snprintf(name,sizeof(name),"%s/outside.bin",game);CHECK(access(name,F_OK)&&errno==ENOENT);
 snprintf(name,sizeof(name),"%s/AppData/11bitstudios/Frostpunk/shell-fixture.bin",profile);CHECK(!unlink(name));
 for(unsigned i=2;i>0;--i){snprintf(name,sizeof(name),"%s/%s",profile,relative[i-1]);CHECK(!rmdir(name));}
 snprintf(name,sizeof(name),"%s/AppData",profile);CHECK(!rmdir(name));CHECK(!rmdir(game)&&!rmdir(profile)&&!rmdir(root));
 win64_region_remove((uintptr_t)&bytes);win64_region_remove((uintptr_t)back);win64_region_remove((uintptr_t)payload);win64_region_remove((uintptr_t)&w);
 printf("{\"success\":true,\"checks\":%u,\"concurrent_queries\":4000,\"profile_provision_real\":true,\"no_CREATE_query_readonly\":true,\"derived_binary_IO\":true,\"alias_boundary_traversal\":true,\"regular_file_and_symlink_rejected\":true,\"CPU_RSP_LastError\":true,\"output_canary\":true,\"no_original_execution\":true}\n",checks);return 0;
}
