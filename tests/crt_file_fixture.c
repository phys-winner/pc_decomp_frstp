/* Production pointer registry, CRT errno, FILE state and anchored kernel. */
#include "../android/host/win64.c"
#include "../android/host/win64_stdio.c"
#include "crt_file_vectors.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/wait.h>
static _Thread_local unsigned char fixture_teb[0x100];
uint64_t frostpunk_teb_read(uint64_t off,unsigned n){uint64_t v=0;if(off+n>sizeof(fixture_teb)||n>8)abort();memcpy(&v,fixture_teb+off,n);return v;}
void frostpunk_teb_write(uint64_t off,uint64_t v,unsigned n){if(off+n>sizeof(fixture_teb)||n>8)abort();memcpy(fixture_teb+off,&v,n);}
static atomic_uint checks;
#define CHECK(x) do{atomic_fetch_add(&checks,1);if(!(x)){fprintf(stderr,"CRT_FILE_FAIL line=%d errno=%d error=%u\n",__LINE__,crt_errno,win64_get_error());abort();}}while(0)
static uint64_t call(const char *name,const uint64_t a[8],int kernel){
 uint64_t stack[10]={0};win64_region_add((uintptr_t)stack,sizeof(stack),1);CPU c;memset(&c,0xa5,sizeof(c));c.rsp=(uintptr_t)stack;c.rcx=a[0];c.rdx=a[1];c.r8=a[2];c.r9=a[3];for(unsigned i=4;i<8;++i)stack[i+1]=a[i];CPU before=c;
 CHECK(kernel?win64_kernel_call(&c,name):win64_stdio_call(&c,name));uint64_t result=c.rax;c.rax=before.rax;CHECK(!memcmp(&c,&before,sizeof(c)));win64_region_remove((uintptr_t)stack);return result;
}
#define API(name,...) call(name,(uint64_t[8]){__VA_ARGS__},0)
#define KERNEL(name,...) call(name,(uint64_t[8]){__VA_ARGS__},1)
static uint64_t open_file(const char *name,int writing,int expected){
 uint16_t wide[512],mode[3]={(uint16_t)(writing?'w':'r'),'b',0};size_t len=strlen(name);CHECK(len<512);for(size_t i=0;i<=len;++i)wide[i]=(unsigned char)name[i];
 struct{uint64_t a,value,b;}out={0xaabbccdd,UINT64_MAX,0x11223344};win64_region_add((uintptr_t)&out,sizeof(out),1);win64_region_add((uintptr_t)wide,(len+1)*2,0);win64_region_add((uintptr_t)mode,sizeof(mode),0);
 crt_errno=1927;win64_set_error(0x5a17);uint64_t actual=API("_wfopen_s",(uintptr_t)&out.value,(uintptr_t)wide,(uintptr_t)mode);if(actual!=(unsigned)expected)fprintf(stderr,"OPEN_MISMATCH path=%s writing=%d actual=%llu expected=%d\n",name,writing,(unsigned long long)actual,expected);CHECK(actual==(unsigned)expected);
 CHECK(crt_errno==(expected?expected:1927)&&win64_get_error()==0x5a17&&out.a==0xaabbccdd&&out.b==0x11223344);CHECK(expected?!out.value:out.value!=0);
 win64_region_remove((uintptr_t)mode);win64_region_remove((uintptr_t)wide);win64_region_remove((uintptr_t)&out);return out.value;
}
static void close_file(uint64_t file){crt_errno=1927;CHECK(API("fclose",file)==0&&crt_errno==1927);}
static const unsigned char binary_bytes[]={0x41,0,0xff,0x0d,0x0a,0x1a,0x5a};
static char game_path[1024],profile_path[1024],base_path[1024];
static const char existing[]="C:/FrostpunkProfile/input.bin",written[]="C:/FrostpunkProfile/config.bin";
static atomic_int writer_done,closer_done;
static atomic_int stderr_ready;
static void *locked_stderr(void *unused){(void)unused;uint64_t file=API("__acrt_iob_func",2);API("_lock_file",file);atomic_store(&stderr_ready,1);while(atomic_load(&stderr_ready)!=2)sched_yield();struct timespec pause={0,100000000};nanosleep(&pause,NULL);API("_unlock_file",file);return NULL;}
static void *blocked_writer(void *p){uint64_t f=*(uint64_t*)p;CHECK(API("fwrite",(uintptr_t)binary_bytes,7,1,f)==1);atomic_store(&writer_done,1);return NULL;}
static void *blocked_closer(void *p){close_file(*(uint64_t*)p);atomic_store(&closer_done,1);return NULL;}
static void wait_state(uint64_t guest,unsigned refs,int state){
 for(unsigned n=0;n<100000;++n){registry_lock();file_state *s=stream_find(guest);int ready=s&&s->refs==refs&&s->state==state;registry_unlock();if(ready)return;sched_yield();}CHECK(0);
}
static void *stress(void *arg){unsigned index=(unsigned)(uintptr_t)arg;char path[128];snprintf(path,sizeof(path),"C:/FrostpunkProfile/stress%u.bin",index);
 unsigned char bytes[32],back[32];win64_region_add((uintptr_t)bytes,sizeof(bytes),0);win64_region_add((uintptr_t)back,sizeof(back),1);
 for(unsigned n=0;n<128;++n){for(unsigned i=0;i<32;++i)bytes[i]=(unsigned char)(index*31+n+i);uint64_t f=open_file(path,1,0);CHECK(API("fwrite",(uintptr_t)bytes,4,8,f)==8);close_file(f);f=open_file(path,0,0);CHECK(API("fread",(uintptr_t)back,4,8,f)==8&&!memcmp(bytes,back,32));close_file(f);}
 win64_region_remove((uintptr_t)back);win64_region_remove((uintptr_t)bytes);return NULL;
}
static void negative(unsigned which,uint64_t file){
 pid_t child=fork();CHECK(child>=0);if(!child){uint16_t mode[]={'r','b',0},empty[]={0};uint64_t output=1;win64_region_add((uintptr_t)mode,sizeof(mode),0);win64_region_add((uintptr_t)empty,sizeof(empty),0);win64_region_add((uintptr_t)&output,8,1);
  if(which==0)API("_wfopen_s",0,(uintptr_t)mode,(uintptr_t)mode);
  else if(which==1)API("_wfopen_s",(uintptr_t)&output,0,(uintptr_t)mode);
  else if(which==2)API("_wfopen_s",(uintptr_t)&output,(uintptr_t)mode,0);
  else if(which==3)API("_wfopen_s",(uintptr_t)&output,(uintptr_t)mode,(uintptr_t)empty);
  else if(which==4)API("fclose",0);
  else if(which==5)API("_fseeki64",file,0,3);
  else if(which==6)API("fread",(uintptr_t)binary_bytes,1,1,0);
  else if(which==7)API("fwrite",0,1,1,file);
  else if(which==8)API("fclose",file+1);
  else if(which==9){API("_lock_file",file);API("fclose",file);}
  else if(which==10)API("_unlock_file",file);
  else if(which==11){API("fclose",file);API("fclose",file);}
  else if(which==12){alarm(3);pthread_t locker;if(pthread_create(&locker,NULL,locked_stderr,NULL))_Exit(97);while(!atomic_load(&stderr_ready))sched_yield();atomic_store(&stderr_ready,2);API("fclose",file+1);}
  else API(which==13?"fread":"fwrite",(uintptr_t)binary_bytes,2,UINT64_MAX,file);
  _Exit(99);
 }int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==73);
}
int main(void){
 win64_kernel_roots_self_test();char temp[]="/data/local/tmp/frostpunk/crt-file-XXXXXX";CHECK(mkdtemp(temp));snprintf(base_path,sizeof(base_path),"%s",temp);snprintf(game_path,sizeof(game_path),"%s/game",temp);snprintf(profile_path,sizeof(profile_path),"%s/profile",temp);CHECK(!mkdir(game_path,0700)&&!mkdir(profile_path,0700));CHECK(win64_kernel_configure_roots(game_path,profile_path));
 char host[1200];snprintf(host,sizeof(host),"%s/input.bin",profile_path);int fd=open(host,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0&&write(fd,binary_bytes,7)==7&&!close(fd));
 snprintf(host,sizeof(host),"%s/asset.bin",game_path);fd=open(host,O_CREAT|O_EXCL|O_WRONLY,0600);CHECK(fd>=0&&write(fd,binary_bytes,7)==7&&!close(fd));
 win64_region_add((uintptr_t)binary_bytes,7,0);win64_region_add((uintptr_t)existing,sizeof(existing),0);win64_region_add((uintptr_t)written,sizeof(written),0);unsigned char buffer[64];win64_region_add((uintptr_t)buffer,sizeof(buffer),1);
 uint64_t file=open_file(existing,0,0);CHECK(file);close_file(file);open_file("C:/FrostpunkProfile/missing.bin",0,2);open_file("C:/FrostpunkProfile/missing/file",0,2);open_file("C:/FrostpunkProfile",0,13);open_file("",0,22);
 open_file("C:/Program Files (x86)/Steam/steamapps/common/Frostpunk/asset.bin",1,13);file=open_file("C:/Program Files (x86)/Steam/steamapps/common/Frostpunk/asset.bin",0,0);close_file(file);
 open_file("C:/FrostpunkProfile/../game/asset.bin",0,13);open_file("C:asset.bin",0,2);
 snprintf(host,sizeof(host),"%s/link",profile_path);CHECK(!symlink(game_path,host));open_file("C:/FrostpunkProfile/link/asset.bin",0,2);open_file("C:/FrostpunkProfile/link",0,13);CHECK(!unlink(host));
 snprintf(host,sizeof(host),"%s/fifo",profile_path);CHECK(!mkfifo(host,0600));open_file("C:/FrostpunkProfile/fifo",0,13);open_file("C:/FrostpunkProfile/fifo",1,13);CHECK(!unlink(host));
 for(unsigned i=0;i<sizeof(native_reads)/sizeof(native_reads[0]);++i){const native_read *v=&native_reads[i];file=open_file(existing,0,0);memset(buffer,0xa5,sizeof(buffer));crt_errno=1927;
  CHECK(API("fread",(uintptr_t)buffer,v->size,v->count,file)==v->result&&crt_errno==v->error);
  CHECK(API("_ftelli64",file)==v->position&&API("feof",file)==v->eof&&API("ferror",file)==v->failure&&!memcmp(buffer,v->bytes,24));
  crt_errno=1927;CHECK(API("fread",(uintptr_t)buffer,1,1,file)==v->next_result&&crt_errno==1927);CHECK(API("feof",file)==v->next_eof);
  API("clearerr",file);CHECK(!API("feof",file)&&!API("ferror",file));close_file(file);
 }
 file=open_file(existing,0,0);
 for(unsigned i=0;i<sizeof(native_seeks)/sizeof(native_seeks[0]);++i){const native_seek *v=&native_seeks[i];crt_errno=1927;uint64_t r=API(v->name,file,v->offset,v->origin);CHECK(r==v->result&&crt_errno==v->error);}
 close_file(file);
 for(unsigned n=0;n<2;++n){CHECK(API("fread",0,n?1:0,n?0:1,0)==0);CHECK(API("fwrite",0,n?1:0,n?0:1,0)==0);}
 file=open_file(existing,0,0);crt_errno=1927;CHECK(API("fwrite",(uintptr_t)binary_bytes,1,1,file)==0&&crt_errno==9&&API("ferror",file)==1);API("clearerr",file);CHECK(!API("ferror",file));close_file(file);
 file=open_file(written,1,0);crt_errno=1927;CHECK(API("fread",(uintptr_t)buffer,1,1,file)==0&&crt_errno==1927&&API("ferror",file)==1);API("clearerr",file);CHECK(API("fwrite",(uintptr_t)binary_bytes,7,1,file)==1);CHECK(API("fflush",file)==0&&API("fflush",0)==0);close_file(file);
 snprintf(host,sizeof(host),"%s/config.bin",profile_path);fd=open(host,O_RDONLY);CHECK(fd>=0&&read(fd,buffer,64)==7&&!memcmp(buffer,binary_bytes,7)&&!close(fd));
 /* Sharing failure must precede O_TRUNC, and retained owner must survive the
  * temporary Win32 handle closing inside stream_open. */
 file=open_file(existing,0,0);uint64_t another=open_file(existing,0,0);open_file(existing,1,13);CHECK(KERNEL("CreateFileA",(uintptr_t)existing,0x40000000,7,0,2,0x80,0)==UINT64_MAX&&win64_get_error()==32);close_file(another);CHECK(API("fread",(uintptr_t)buffer,7,1,file)==1&&!memcmp(buffer,binary_bytes,7));close_file(file);
 file=open_file(written,1,0);open_file(written,0,13);open_file(written,1,13);close_file(file);file=open_file(written,0,0);close_file(file);
 file=open_file(existing,0,0);for(unsigned i=0;i<15;++i)negative(i,file);close_file(file);
 /* Writer has acquired its operation reference before blocking on the native
  * recursive FILE lock; closer must wait for both lock and queued writer. */
 file=open_file(written,1,0);API("_lock_file",file);API("_lock_file",file);pthread_t writer,closer;CHECK(!pthread_create(&writer,NULL,blocked_writer,&file));wait_state(file,3,1);CHECK(!pthread_create(&closer,NULL,blocked_closer,&file));wait_state(file,3,3);CHECK(!atomic_load(&writer_done)&&!atomic_load(&closer_done));API("_unlock_file",file);CHECK(!atomic_load(&writer_done)&&!atomic_load(&closer_done));API("_unlock_file",file);CHECK(!pthread_join(writer,NULL)&&!pthread_join(closer,NULL)&&atomic_load(&writer_done)&&atomic_load(&closer_done));
 pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,stress,(void*)(uintptr_t)i));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
 uint64_t all[509];for(unsigned i=0;i<509;++i)all[i]=open_file(existing,0,0);open_file(existing,0,24);for(unsigned i=0;i<509;++i)close_file(all[i]);file=open_file(existing,0,0);close_file(file);
 win64_stdio_self_test();win64_kernel_self_test();
 const char *names[]={"input.bin","config.bin","stress0.bin","stress1.bin","stress2.bin","stress3.bin"};for(unsigned i=0;i<6;++i){snprintf(host,sizeof(host),"%s/%s",profile_path,names[i]);CHECK(!unlink(host));}snprintf(host,sizeof(host),"%s/asset.bin",game_path);CHECK(!unlink(host));CHECK(!rmdir(game_path)&&!rmdir(profile_path)&&!rmdir(base_path));
 win64_region_remove((uintptr_t)buffer);win64_region_remove((uintptr_t)binary_bytes);win64_region_remove((uintptr_t)existing);win64_region_remove((uintptr_t)written);
 printf("{\"success\":true,\"checks\":%u,\"native_read_vectors\":6,\"native_seek_vectors\":%zu,\"negative73\":15,\"threads\":4,\"file_cycles\":1024,\"max_dynamic_files\":509,\"secure_sharing\":true,\"real_binary_IO\":true,\"close_waits_for_explicit_locks_and_inflight_writer\":true,\"kernel_and_roots_regression\":true,\"stdio_selftest\":44,\"full_CPU_RSP_canaries_errno\":true}\n",atomic_load(&checks),sizeof(native_seeks)/sizeof(native_seeks[0]));return 0;
}
