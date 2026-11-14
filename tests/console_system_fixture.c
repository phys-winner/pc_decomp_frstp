/* Real core pointer/TEB/errno and production system provider, never the PE. */
#include "../android/host/win64.c"
#include "win64_system.h"
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "console_system_vectors.h"
static _Thread_local unsigned char system_teb[256];
uint64_t frostpunk_teb_read(uint64_t offset,unsigned bytes){uint64_t v=0;if(offset+bytes>sizeof(system_teb))abort();memcpy(&v,system_teb+offset,bytes);return v;}
void frostpunk_teb_write(uint64_t offset,uint64_t value,unsigned bytes){if(offset+bytes>sizeof(system_teb))abort();memcpy(system_teb+offset,&value,bytes);}
void frostpunk_cpuid(CPU *cpu){(void)cpu;abort();} /* Unrelated old measurement is tested by system-arm64. */
static atomic_uint write_mode,write_attempts,interruptions,short_writes,clock_mode,checked_calls;
ssize_t __real_write(int fd,const void *buffer,size_t bytes);
ssize_t __wrap_write(int fd,const void *buffer,size_t bytes){
    unsigned mode=atomic_load(&write_mode);if(fd==2&&mode){
        unsigned attempt=atomic_fetch_add(&write_attempts,1);
        if(mode==2){errno=EBADF;return -1;}
        if(!attempt){atomic_fetch_add(&interruptions,1);errno=EINTR;return -1;}
        if(bytes>7){bytes=7;atomic_fetch_add(&short_writes,1);}
    }return __real_write(fd,buffer,bytes);
}
int __real_clock_gettime(clockid_t clock,struct timespec *out);
int __wrap_clock_gettime(clockid_t clock,struct timespec *out){
    if(clock==CLOCK_BOOTTIME){unsigned mode=atomic_load(&clock_mode);
        if(mode==1){uint64_t ms=UINT64_C(0x100000005);*out=(struct timespec){(time_t)(ms/1000),(long)((ms%1000)*1000000+999999)};return 0;}
        if(mode==2){errno=EINVAL;return -1;}
    }return __real_clock_gettime(clock,out);
}
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"CONSOLE_SYSTEM_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(const char *name,uint64_t argument){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=argument;c.rsp=UINT64_C(0x12345678);before=c;
    crt_errno=1927;win64_set_error(0x5a17);errno=1927;
    if(!win64_system_call(&c,name))abort();uint64_t result=c.rax;
    if(strcmp(name,"OutputDebugStringW"))c.rax=before.rax;
    unsigned expected_error=!strcmp(name,"AttachConsole")?6:0x5a17;
    if(memcmp(&c,&before,sizeof(c))||crt_errno!=1927||win64_get_error()!=expected_error||errno!=1927){fputs("CONSOLE_SYSTEM_CPU_ERRORS_FAIL\n",stderr);abort();}
    atomic_fetch_add(&checked_calls,1);return result;
}
static int capture_begin(int *saved){
    char path[]="/data/local/tmp/frostpunk/console-fixture-XXXXXX";int fd=mkstemp(path);if(fd<0)abort();if(unlink(path))abort();
    *saved=dup(2);if(*saved<0||dup2(fd,2)<0)abort();return fd;
}
static size_t capture_end(int fd,int saved,char *buffer,size_t capacity){
    if(dup2(saved,2)<0||close(saved)||lseek(fd,0,SEEK_SET)<0)abort();
    size_t used=0;for(;;){if(used==capacity)abort();ssize_t n=read(fd,buffer+used,capacity-used);if(n<0&&errno==EINTR)continue;if(n<0)abort();if(!n)break;used+=(size_t)n;}
    if(close(fd))abort();return used;
}
typedef struct{FILE *stream;int fd,fdflags,flags;struct stat stat;}stream_state;
static stream_state stream_get(FILE *stream){stream_state s={.stream=stream,.fd=fileno(stream)};if(s.fd<0||fstat(s.fd,&s.stat))abort();s.fdflags=fcntl(s.fd,F_GETFD);s.flags=fcntl(s.fd,F_GETFL);return s;}
static int stream_same(stream_state a,stream_state b){return a.stream==b.stream&&a.fd==b.fd&&a.fdflags==b.fdflags&&a.flags==b.flags&&a.stat.st_dev==b.stat.st_dev&&a.stat.st_ino==b.stat.st_ino&&a.stat.st_mode==b.stat.st_mode;}
static const uint16_t thread_text[4][3]={{'T','0',0},{'T','1',0},{'T','2',0},{'T','3',0}};
static void *worker(void *opaque){
    unsigned id=(unsigned)(uintptr_t)opaque;for(unsigned i=0;i<2048;++i)if(invoke("AttachConsole",UINT64_C(0xabcdef00ffffffff)))abort();
    for(unsigned i=0;i<64;++i)invoke("OutputDebugStringW",(uint64_t)(uintptr_t)thread_text[id]);return NULL;
}
static uint64_t boottime_ms(void){struct timespec t;if(__real_clock_gettime(CLOCK_BOOTTIME,&t))abort();return(uint64_t)t.tv_sec*1000+(uint64_t)t.tv_nsec/1000000;}
int main(int argc,char **argv){
    if(argc==2){
        if(!strcmp(argv[1],"--pid-zero"))invoke("AttachConsole",0);
        else if(!strcmp(argv[1],"--pid-one"))invoke("AttachConsole",1);
        else if(!strcmp(argv[1],"--pid-max-minus-one"))invoke("AttachConsole",UINT32_MAX-1);
        else if(!strcmp(argv[1],"--pointer"))invoke("OutputDebugStringW",1);
        else if(!strcmp(argv[1],"--clock")){atomic_store(&clock_mode,2);invoke("GetTickCount",0);}
        else{
            uint16_t *text=malloc(32769*2);if(!text)abort();size_t units=2;text[0]=0xd800;text[1]=0;
            if(!strcmp(argv[1],"--low"))text[0]=0xdc00;
            else if(!strcmp(argv[1],"--pair")){text[1]='A';text[2]=0;units=3;}
            else if(!strcmp(argv[1],"--unterminated")){text[0]='A';text[1]='B';}
            else if(!strcmp(argv[1],"--bound")){units=32769;for(unsigned i=0;i<32768;++i)text[i]='A';text[32768]=0;}
            else if(strcmp(argv[1],"--high"))return 3;
            win64_region_add((uint64_t)(uintptr_t)text,units*2,0);invoke("OutputDebugStringW",(uint64_t)(uintptr_t)text);
        }return 99;
    }
    CHECK(argc==1);stream_state streams[3]={stream_get(stdin),stream_get(stdout),stream_get(stderr)};
    for(unsigned i=0;i<4;++i)CHECK(!invoke("AttachConsole",((uint64_t)i<<32)|UINT32_MAX));
    CHECK(stream_same(streams[0],stream_get(stdin))&&stream_same(streams[1],stream_get(stdout))&&stream_same(streams[2],stream_get(stderr)));
    char output[100000];int saved,fd;
    for(unsigned i=0;i<sizeof(console_vectors)/sizeof(console_vectors[0]);++i){const console_vector *v=&console_vectors[i];
        if(v->text)win64_region_add((uint64_t)(uintptr_t)v->text,v->units*2,0);
        fd=capture_begin(&saved);invoke("OutputDebugStringW",(uint64_t)(uintptr_t)v->text);size_t bytes=capture_end(fd,saved,output,sizeof(output));
        CHECK(bytes==v->bytes&&!memcmp(output,v->expected,bytes));if(v->text)win64_region_remove((uint64_t)(uintptr_t)v->text);
    }
    static uint16_t unicode[]={0x416,0xd83d,0xde42,0};win64_region_add((uint64_t)(uintptr_t)unicode,sizeof(unicode),0);
    atomic_store(&write_mode,1);fd=capture_begin(&saved);invoke("OutputDebugStringW",(uint64_t)(uintptr_t)unicode);size_t bytes=capture_end(fd,saved,output,sizeof(output));
    static const char unicode_expected[]="GUEST_DEBUG \xd0\x96\xf0\x9f\x99\x82\n";CHECK(bytes==sizeof(unicode_expected)-1&&!memcmp(output,unicode_expected,bytes));CHECK(atomic_load(&interruptions)==1&&atomic_load(&short_writes)>0);
    atomic_store(&write_mode,2);fd=capture_begin(&saved);invoke("OutputDebugStringW",(uint64_t)(uintptr_t)unicode);CHECK(!capture_end(fd,saved,output,sizeof(output)));atomic_store(&write_mode,0);
    uint16_t *maximum=malloc(32768*2);CHECK(maximum!=NULL);for(unsigned i=0;i<32767;++i)maximum[i]='A';maximum[32767]=0;win64_region_add((uint64_t)(uintptr_t)maximum,32768*2,0);
    fd=capture_begin(&saved);invoke("OutputDebugStringW",(uint64_t)(uintptr_t)maximum);bytes=capture_end(fd,saved,output,sizeof(output));CHECK(bytes==32780&&!memcmp(output,"GUEST_DEBUG ",12)&&output[32779]=='\n');for(unsigned i=12;i<32779;++i)CHECK(output[i]=='A');win64_region_remove((uint64_t)(uintptr_t)maximum);free(maximum);
    win64_region_add((uint64_t)(uintptr_t)thread_text,sizeof(thread_text),0);atomic_store(&write_mode,1);atomic_store(&write_attempts,0);fd=capture_begin(&saved);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    bytes=capture_end(fd,saved,output,sizeof(output));atomic_store(&write_mode,0);CHECK(bytes==256*15);unsigned counts[4]={0};
    for(size_t i=0;i<bytes;i+=15){CHECK(!memcmp(output+i,"GUEST_DEBUG T",13)&&output[i+13]>='0'&&output[i+13]<='3'&&output[i+14]=='\n');++counts[output[i+13]-'0'];}for(unsigned i=0;i<4;++i)CHECK(counts[i]==64);
    for(unsigned i=0;i<16;++i){uint32_t before=(uint32_t)boottime_ms(),tick=(uint32_t)invoke("GetTickCount",UINT64_MAX),after=(uint32_t)boottime_ms();CHECK((uint32_t)(tick-before)<=(uint32_t)(after-before));}
    atomic_store(&clock_mode,1);CHECK(invoke("GetTickCount",UINT64_MAX)==5);atomic_store(&clock_mode,0);
    CPU c,before;memset(&c,0xa5,sizeof(c));before=c;crt_errno=1927;win64_set_error(0x5a17);errno=1927;CHECK(!win64_system_call(&c,"AllocConsole")&&!memcmp(&c,&before,sizeof(c))&&crt_errno==1927&&win64_get_error()==0x5a17&&errno==1927);
    printf("{\"success\":true,\"native_debug_rows\":%u,\"native_parent_rows\":2,\"native_null_rows\":1,\"checks\":%u,\"checked_calls\":%u,\"parallel_attach_calls\":8192,\"parallel_debug_messages\":256,\"native_tick_brackets\":16,\"fullCPU_no_pop\":true,\"low32_parent\":true,\"all_error_contracts\":true,\"standard_streams_unchanged\":true,\"UTF16_maximum_units\":32767,\"message_serialization\":true,\"injected_EINTR\":%u,\"injected_short_writes\":%u,\"injected_permanent_write_drop\":true,\"injected_DWORD_wrap\":5}\n",(unsigned)(sizeof(console_vectors)/sizeof(console_vectors[0]))-1,checks,atomic_load(&checked_calls),atomic_load(&interruptions),atomic_load(&short_writes));return 0;
}
