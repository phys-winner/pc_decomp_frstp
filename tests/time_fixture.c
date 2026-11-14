/* Real production registry/errno; only Windows synthetic calendar data is oracle. */
#include "../android/host/win64.c"
#include "win64_environment.h"
#include "win64_time.h"
#include <dlfcn.h>
#include <time.h>
#include <sys/system_properties.h>
typedef struct {int32_t sec,min,hour,mday,mon,year,wday,yday,isdst;} test_tm;
typedef struct {int64_t seconds;int32_t result,error;test_tm tm;uint32_t padding;} time_record;
typedef struct {uint32_t format,capacity,result,error,callbacks;test_tm tm;unsigned char text[256];} format_record;
_Static_assert(sizeof(test_tm)==36,"tm size");
_Static_assert(sizeof(time_record)==56,"time record");
_Static_assert(sizeof(format_record)==312,"format record");
static const char *const formats[]={"%d-%m-%Y %H-%M-%S.png","%F %T","%d/%m/%Y %H:%M","%d-%m-%Y_%H-%M-%S",""};
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"TIME_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static uint64_t invoke(const char *name,uint64_t a,uint64_t b,uint64_t d,uint64_t e){CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=a;cpu.rdx=b;cpu.r8=d;cpu.r9=e;cpu.rsp=0x12345678;before=cpu;
    if(!win64_time_call(&cpu,name))abort();uint64_t result=cpu.rax;cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return result;}
static void register_formats(void){for(unsigned i=0;i<5;++i)win64_region_add((uintptr_t)formats[i],strlen(formats[i])+1,0);}
static void *reader(void *context){time_record *records=context;test_tm tm;win64_region_add((uintptr_t)&tm,sizeof(tm),1);
    for(unsigned i=0;i<2000;++i){time_record *r=&records[i%128];crt_errno=1234;if(invoke("_localtime64_s",(uintptr_t)&tm,(uintptr_t)&r->seconds,0,0)!=(uint32_t)r->result||memcmp(&tm,&r->tm,36)||crt_errno!=r->error)abort();
        if(!(i%31))invoke("_tzset",0,0,0,0);}
    win64_region_remove((uintptr_t)&tm);return NULL;}
static int system_test(void){
    typedef timezone_t (*alloc_fn)(const char*);typedef struct tm *(*local_fn)(timezone_t,const time_t*,struct tm*);typedef void(*free_fn)(timezone_t);
    alloc_fn allocate=(alloc_fn)dlsym(RTLD_DEFAULT,"tzalloc");local_fn local=(local_fn)dlsym(RTLD_DEFAULT,"localtime_rz");free_fn release=(free_fn)dlsym(RTLD_DEFAULT,"tzfree");CHECK(allocate&&local&&release);
    CHECK(!setenv("TZ","America/Los_Angeles",1));tzset();CHECK(win64_environment_configure(NULL,0));
    int before_errno=errno=1927;invoke("_tzset",0,0,0,0);CHECK(errno==before_errno);
    timezone_t system=allocate(NULL);CHECK(system);char property[PROP_VALUE_MAX]={0};__system_property_get("persist.sys.timezone",property);
    int64_t values[]={0,1719573225,1770000000,32536799999};test_tm actual;win64_region_add((uintptr_t)values,sizeof(values),0);win64_region_add((uintptr_t)&actual,sizeof(actual),1);
    for(unsigned i=0;i<4;++i){struct tm native;time_t seconds=(time_t)values[i];CHECK(local(system,&seconds,&native));test_tm expected={native.tm_sec,native.tm_min,native.tm_hour,native.tm_mday,native.tm_mon,native.tm_year,native.tm_wday,native.tm_yday,native.tm_isdst};
        crt_errno=1234;errno=1927;CHECK(!invoke("_localtime64_s",(uintptr_t)&actual,(uintptr_t)&values[i],0,0));CHECK(!memcmp(&actual,&expected,36));CHECK(crt_errno==1234&&errno==1927);}
    release(system);printf("{\"success\":true,\"checks\":%u,\"private_API35_symbols\":true,\"system_timezone\":\"%s\",\"host_TZ_ignored\":\"America/Los_Angeles\",\"Win64_tm_bytes\":36}\n",checks,property);return 0;
}
int main(int argc,char **argv){win64_time_self_test();register_formats();
    if(argc==2&&!strcmp(argv[1],"--system"))return system_test();
    CHECK(argc>=2);const win64_environment_entry entry={"TZ",!strcmp(argv[1],"--unsupported-tz")?"UTC0DST":argc==3?argv[1]:"UTC0"};CHECK(win64_environment_configure(&entry,1));
    if(argc==2){test_tm tm={3,2,1,2,0,124,2,1,0};int64_t seconds=0;char text[80];static const char unknown[]="%z";win64_region_add((uintptr_t)&tm,sizeof(tm),1);win64_region_add((uintptr_t)&seconds,8,0);win64_region_add((uintptr_t)text,sizeof(text),1);win64_region_add((uintptr_t)unknown,sizeof(unknown),0);
        if(!strcmp(argv[1],"--null-output"))invoke("_localtime64_s",0,(uintptr_t)&seconds,0,0);
        else if(!strcmp(argv[1],"--null-input"))invoke("_localtime64_s",(uintptr_t)&tm,0,0,0);
        else if(!strcmp(argv[1],"--null-destination"))invoke("strftime",0,80,(uintptr_t)formats[1],(uintptr_t)&tm);
        else if(!strcmp(argv[1],"--zero-capacity"))invoke("strftime",(uintptr_t)text,0,(uintptr_t)formats[1],(uintptr_t)&tm);
        else if(!strcmp(argv[1],"--null-format"))invoke("strftime",(uintptr_t)text,80,0,(uintptr_t)&tm);
        else if(!strcmp(argv[1],"--null-tm"))invoke("strftime",(uintptr_t)text,80,(uintptr_t)formats[1],0);
        else if(!strcmp(argv[1],"--invalid-field")){tm.mon=12;invoke("strftime",(uintptr_t)text,80,(uintptr_t)formats[1],(uintptr_t)&tm);}
        else if(!strcmp(argv[1],"--unknown-format"))invoke("strftime",(uintptr_t)text,80,(uintptr_t)unknown,(uintptr_t)&tm);
        else if(!strcmp(argv[1],"--unsupported-tz"))invoke("_tzset",0,0,0,0);
        else if(!strcmp(argv[1],"--invalid-pointer"))invoke("_localtime64_s",1,(uintptr_t)&seconds,0,0);
        else return 3;return 99;
    }
    FILE *file=fopen(argv[2],"rb");CHECK(file);uint32_t header[4];CHECK(fread(header,sizeof(header),1,file)==1&&header[0]==0x454d4954);
    size_t bytes=(size_t)header[1]*sizeof(time_record)+(size_t)header[2]*sizeof(format_record);unsigned char *data=malloc(bytes);CHECK(data);CHECK(fread(data,bytes,1,file)==1);CHECK(fgetc(file)==EOF);fclose(file);win64_region_add((uintptr_t)data,bytes,0);
    time_record *times=(time_record*)data;format_record *strings=(format_record*)(data+(size_t)header[1]*sizeof(time_record));
    unsigned char tm_buffer[44];test_tm *tm=(test_tm*)tm_buffer;unsigned char text[256];win64_region_add((uintptr_t)tm_buffer,sizeof(tm_buffer),1);win64_region_add((uintptr_t)text,sizeof(text),1);
    errno=1927;invoke("_tzset",0,0,0,0);CHECK(errno==1927);
    for(unsigned i=0;i<header[1];++i){memset(tm_buffer,0xa5,sizeof(tm_buffer));crt_errno=1234;errno=1927;uint64_t result=invoke("_localtime64_s",(uintptr_t)tm,(uintptr_t)&times[i].seconds,0,0);
        if(result!=(uint32_t)times[i].result||memcmp(tm,&times[i].tm,36)||crt_errno!=times[i].error||errno!=1927){fprintf(stderr,"time case%u t=%lld got=%llu errno=%d expected=%d/%d\n",i,(long long)times[i].seconds,(unsigned long long)result,crt_errno,times[i].result,times[i].error);return 2;}
        CHECK(!memcmp(tm_buffer+36,"\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5",8));}
    unsigned compared=0,skipped=0;
    for(unsigned i=0;i<header[2];++i){format_record *r=&strings[i];if(r->callbacks){++skipped;continue;}CHECK(r->format<5&&r->capacity<=256);memset(text,0xa5,sizeof(text));crt_errno=1234;errno=1927;
        uint64_t result=invoke("strftime",(uintptr_t)text,r->capacity,(uintptr_t)formats[r->format],(uintptr_t)&r->tm);
        if(result!=r->result||crt_errno!=(int)r->error||memcmp(text,r->text,256)||errno!=1927){fprintf(stderr,"strftime case%u f%u cap%u got=%llu/%d expected=%u/%u\n",i,r->format,r->capacity,(unsigned long long)result,crt_errno,r->result,r->error);return 2;}++compared;}
    memset(tm_buffer,0,sizeof(tm_buffer));crt_errno=1234;CHECK(invoke("_localtime64_s",(uintptr_t)tm,(uintptr_t)tm,0,0)==22&&crt_errno==22);for(unsigned i=0;i<36;++i)CHECK(tm_buffer[i]==0xff);
    CHECK(header[1]>128);pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,reader,times));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    printf("{\"success\":true,\"checks\":%u,\"time_cases\":%u,\"strftime_compared\":%u,\"native_invalid_callback_boundaries\":%u,\"concurrent_time_calls\":8000,\"threads\":4,\"CPU_RSP_errno_preserved\":true,\"Win64_tm_bytes\":36}\n",checks,header[1],compared,skipped);return 0;
}
