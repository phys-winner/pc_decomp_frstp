/* Windows x64 system ABI over actual Android kernel measurements. CPU family
 * and address-space bounds describe the translated guest's declared policy. */
#define _GNU_SOURCE
#include "win64_system.h"
#include "win64.h"
#include "win64_time.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define GUEST_MIN UINT64_C(0x10000)
#define GUEST_END UINT64_C(0x800000000000)
typedef struct {
    uint16_t architecture,reserved;
    uint32_t page_size;
    uint64_t minimum_address,maximum_address,active_processor_mask;
    uint32_t processors,processor_type,allocation_granularity;
    uint16_t processor_level,processor_revision;
} guest_system_info;
typedef struct {
    uint32_t length,load;
    uint64_t total_phys,available_phys,total_pagefile,available_pagefile,total_virtual,available_virtual,available_extended;
} guest_memory_status;
_Static_assert(sizeof(guest_system_info)==48 && offsetof(guest_system_info,processors)==32 &&
               offsetof(guest_system_info,processor_revision)==46,"Win64 SYSTEM_INFO ABI");
_Static_assert(sizeof(guest_memory_status)==64 && offsetof(guest_memory_status,total_virtual)==40,"Win64 MEMORYSTATUSEX ABI");
/* Existing kernel guest alias maps to the writable profile anchor. GetTempPath
 * chooses a directory, and does not create or test that directory. */
static const char temp_path[]="C:\\Users\\Android\\AppData\\Roaming\\11bitstudios\\Frostpunk\\";
extern void frostpunk_cpuid(CPU *cpu);
static _Noreturn void system_stop(const char *reason) {
    fprintf(stderr,"STOP Win64 system %s\n",reason);exit(73);
}
static _Noreturn void console_stop(const char *reason){fprintf(stderr,"STOP Win64 console %s\n",reason);fflush(stderr);_Exit(73);}
static pthread_mutex_t console_mutex=PTHREAD_MUTEX_INITIALIZER;
static uint16_t console_unit(uint64_t address,unsigned index){
    if(address>UINT64_MAX-(uint64_t)index*2)console_stop("UTF16 address overflow");
    uint16_t value;memcpy(&value,win64_pointer(address+(uint64_t)index*2,2,0),2);return value;
}
static void console_write(const char *text,size_t bytes){
    while(bytes){ssize_t n=write(STDERR_FILENO,text,bytes);if(n>0){text+=n;bytes-=(size_t)n;}else if(n<0&&errno==EINTR)continue;else break;}
}
/* shortcut: Android has no Win32 console/debugger. Valid UTF16 debug messages
 * use the native diagnostic fd, without replacing CRT streams or std handles.
 * The bounded 32767-unit profile rejects malformed UTF16 instead of guessing a
 * debugger-dependent Windows encoding conversion. NULL is a measured no-op. */
static void debug_string_w(uint64_t address){
    if(!address)return;
    unsigned length=0;
    for(;length<32768;++length)if(!console_unit(address,length))break;
    if(length==32768)console_stop("debug string exceeds 32767 UTF16 units");
    size_t used=12;char *buffer=malloc((size_t)length*3+13);
    if(!buffer)console_stop("debug encoding buffer allocation");memcpy(buffer,"GUEST_DEBUG ",12);
    for(unsigned i=0;i<length;++i){uint32_t c=console_unit(address,i);
        if(c>=0xd800&&c<=0xdbff){
            if(++i==length){free(buffer);console_stop("unpaired UTF16 high surrogate");}
            unsigned low=console_unit(address,i);if(low<0xdc00||low>0xdfff){free(buffer);console_stop("unpaired UTF16 high surrogate");}
            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);
        }else if(c>=0xdc00&&c<=0xdfff){free(buffer);console_stop("unpaired UTF16 low surrogate");}
        if(c<0x80)buffer[used++]=(char)c;
        else if(c<0x800){buffer[used++]=(char)(0xc0|(c>>6));buffer[used++]=(char)(0x80|(c&63));}
        else if(c<0x10000){buffer[used++]=(char)(0xe0|(c>>12));buffer[used++]=(char)(0x80|((c>>6)&63));buffer[used++]=(char)(0x80|(c&63));}
        else{buffer[used++]=(char)(0xf0|(c>>18));buffer[used++]=(char)(0x80|((c>>12)&63));buffer[used++]=(char)(0x80|((c>>6)&63));buffer[used++]=(char)(0x80|(c&63));}
    }
    buffer[used++]='\n';pthread_mutex_lock(&console_mutex);console_write(buffer,used);pthread_mutex_unlock(&console_mutex);free(buffer);
}
static uint64_t minimum(uint64_t a,uint64_t b) { return a<b?a:b; }
static uint64_t add_checked(uint64_t a,uint64_t b) {
    if (a>UINT64_MAX-b) system_stop("kernel measurement overflow");return a+b;
}
static uint32_t bit_count(uint64_t mask) { uint32_t n=0;while(mask){mask&=mask-1;++n;}return n; }
static uint64_t processor_mask(void) {
    FILE *f=fopen("/sys/devices/system/cpu/online","r");
    if (f) {
        char line[4096];char *read=fgets(line,sizeof(line),f);fclose(f);
        if (read) {
            char *p=line;uint64_t result=0;
            while (*p && *p!='\n') {
                char *end;errno=0;unsigned long low=strtoul(p,&end,10);
                if (end==p || errno || low>=64) system_stop("CPU group larger than supported 64 logical processors");
                p=end;unsigned long high=low;
                if (*p=='-') { high=strtoul(p+1,&end,10);if(end==p+1 || errno || high<low || high>=64)system_stop("CPU online range");p=end; }
                unsigned bits=(unsigned)(high-low+1);result|=(bits==64?UINT64_MAX:((UINT64_C(1)<<bits)-1)<<low);
                if (*p==',')++p;else if(*p && *p!='\n')system_stop("CPU online syntax");
            }
            if (result) return result;
        }
    }
    cpu_set_t cpus;CPU_ZERO(&cpus);
    if (sched_getaffinity(0,sizeof(cpus),&cpus)) system_stop("CPU affinity measurement unavailable");
    uint64_t result=0;
    for (unsigned i=0;i<CPU_SETSIZE;++i) if(CPU_ISSET(i,&cpus)){if(i>=64)system_stop("CPU group larger than supported 64 logical processors");result|=UINT64_C(1)<<i;}
    if(!result)system_stop("empty CPU affinity");return result;
}
static guest_system_info measure_system(void) {
    long page=sysconf(_SC_PAGESIZE),online=sysconf(_SC_NPROCESSORS_ONLN);
    if(page<=0 || page>UINT32_MAX || (page&(page-1)) || online<=0)system_stop("sysconf CPU/page measurement");
    uint64_t mask=processor_mask();
    CPU cpu={0};cpu.rax=1;frostpunk_cpuid(&cpu);uint32_t signature=(uint32_t)cpu.rax;
    unsigned family=(signature>>8)&15,model=(signature>>4)&15;
    if(family==6 || family==15)model|=((signature>>16)&15)<<4;
    if(family==15)family+=(signature>>20)&255;
    guest_system_info info={0};info.architecture=9;info.page_size=(uint32_t)page;
    info.minimum_address=GUEST_MIN;info.maximum_address=GUEST_END-1;info.active_processor_mask=mask;
    info.processors=bit_count(mask);info.processor_type=8664;info.allocation_granularity=65536;
    info.processor_level=(uint16_t)family;info.processor_revision=(uint16_t)((model<<8)|(signature&15));
    return info;
}
static uint64_t mapped_bytes(void) {
    FILE *f=fopen("/proc/self/maps","r");if(!f)system_stop("process VA map unavailable");
    char *line=NULL;size_t capacity=0;uint64_t total=0;
    while(getline(&line,&capacity,f)>=0) {
        uint64_t low,high;
        if(sscanf(line,"%" SCNx64 "-%" SCNx64,&low,&high)!=2)continue;
        if(high<low)system_stop("process VA map range");
        if(low<GUEST_END && high>GUEST_MIN)total=add_checked(total,minimum(high,GUEST_END)-(low>GUEST_MIN?low:GUEST_MIN));
    }
    free(line);if(ferror(f)){fclose(f);system_stop("process VA map read");}fclose(f);return total;
}
static guest_memory_status measure_memory(void) {
    FILE *f=fopen("/proc/meminfo","r");if(!f)system_stop("physical memory measurement unavailable");
    uint64_t total=0,available=0,swap=0,swap_free=0;unsigned found=0;char line[256];
    while(fgets(line,sizeof(line),f)) {
        char key[64],unit[16]={0};uint64_t value;
        if(sscanf(line,"%63[^:]: %" SCNu64 " %15s",key,&value,unit)<2)continue;
        unsigned bit=!strcmp(key,"MemTotal")?1:!strcmp(key,"MemAvailable")?2:!strcmp(key,"SwapTotal")?4:!strcmp(key,"SwapFree")?8:0;
        if(!bit)continue;
        if(strcmp(unit,"kB") || value>UINT64_MAX/1024)system_stop("physical memory units");value*=1024;found|=bit;
        if(bit==1)total=value;else if(bit==2)available=value;else if(bit==4)swap=value;else swap_free=value;
    }
    if(ferror(f)){fclose(f);system_stop("physical memory read");}fclose(f);
    if(found!=15 || !total || available>total || swap_free>swap)system_stop("physical memory geometry");
    uint64_t address_limit=GUEST_END-GUEST_MIN,mapped=mapped_bytes();struct rlimit limit;
    if(getrlimit(RLIMIT_AS,&limit))system_stop("address-space resource limit unavailable");
    if(limit.rlim_cur!=RLIM_INFINITY)address_limit=minimum(address_limit,(uint64_t)limit.rlim_cur);
    guest_memory_status info={0};info.length=64;info.total_phys=total;info.available_phys=available;
    info.load=(uint32_t)(100.0*(double)(total-available)/(double)total);
    /* Android has no Windows page file: commit capacity is the measured RAM +
     * swap pool, bounded by this process's finite RLIMIT_AS when present. */
    info.total_pagefile=minimum(add_checked(total,swap),address_limit);
    info.available_pagefile=minimum(add_checked(available,swap_free),info.total_pagefile);
    info.total_virtual=address_limit;info.available_virtual=address_limit>mapped?address_limit-mapped:0;
    info.available_pagefile=minimum(info.available_pagefile,info.available_virtual);
    return info;
}
static uint32_t temporary_path(uint32_t capacity,uint64_t buffer,int wide) {
    uint32_t length=(uint32_t)strlen(temp_path),required=length+1;
    if(capacity) {
        uint32_t copied=capacity<required?capacity-1:length;
        if(wide) { uint16_t *out=(uint16_t*)win64_pointer(buffer,((size_t)copied+1)*2,1);for(unsigned i=0;i<copied;++i)out[i]=(unsigned char)temp_path[i];out[copied]=0; }
        else { char *out=(char*)win64_pointer(buffer,(size_t)copied+1,1);memcpy(out,temp_path,copied);out[copied]=0; }
    }
    return capacity<required?required:length;
}
int win64_system_call(CPU *c,const char *name) {
    if(!strcmp(name,"GetTickCount")){
        int saved_errno=errno;
        c->rax=(uint32_t)(win64_game_clock_ns(CLOCK_BOOTTIME)/1000000);errno=saved_errno;return 1;
    }
    if(!strcmp(name,"AttachConsole")){
        int saved_errno=errno;
        if((uint32_t)c->rcx!=UINT32_MAX)console_stop("AttachConsole supports only ATTACH_PARENT_PROCESS");
        /* NativeActivity and its parent provide no Windows console object. */
        win64_set_error(6);c->rax=0;errno=saved_errno;return 1;
    }
    if(!strcmp(name,"OutputDebugStringW")){int saved_errno=errno;debug_string_w(c->rcx);errno=saved_errno;return 1;}
    if(!strcmp(name,"GetSystemInfo") || !strcmp(name,"GetNativeSystemInfo")) {
        guest_system_info info=measure_system();memcpy(win64_pointer(win64_argument(c,0),sizeof(info),1),&info,sizeof(info));return 1;
    }
    if(!strcmp(name,"GlobalMemoryStatusEx")) {
        uint64_t out=win64_argument(c,0);uint32_t length;memcpy(&length,win64_pointer(out,4,0),4);
        if(length!=64){win64_set_error(87);c->rax=0;return 1;}
        guest_memory_status info=measure_memory();memcpy(win64_pointer(out,sizeof(info),1),&info,sizeof(info));c->rax=1;return 1;
    }
    if(!strcmp(name,"GetTempPathA") || !strcmp(name,"GetTempPathW") || !strcmp(name,"GetTempPath2A") || !strcmp(name,"GetTempPath2W")) {
        c->rax=temporary_path((uint32_t)win64_argument(c,0),win64_argument(c,1),name[strlen(name)-1]=='W');return 1;
    }
    return 0;
}
static void check(int result,const char *reason) { if(!result)system_stop(reason); }
void win64_system_self_test(void) {
    uint64_t out=win64_alloc(1024,1);CPU c={0};c.rsp=UINT64_C(0x12345678);c.rcx=out;
    check(win64_system_call(&c,"GetSystemInfo") && c.rsp==UINT64_C(0x12345678),"SYSTEM_INFO return protocol");
    guest_system_info info;memcpy(&info,win64_pointer(out,sizeof(info),0),sizeof(info));
    check(info.page_size==(uint32_t)sysconf(_SC_PAGESIZE) && info.architecture==9 && info.processors==bit_count(info.active_processor_mask) &&
          info.minimum_address==GUEST_MIN && info.maximum_address==GUEST_END-1 && info.allocation_granularity==65536,"SYSTEM_INFO measurements");
    check(win64_system_call(&c,"GetNativeSystemInfo") && c.rsp==UINT64_C(0x12345678),"native SYSTEM_INFO protocol");
    memset(win64_pointer(out,1024,1),0xa5,1024);uint32_t length=63;memcpy(win64_pointer(out,4,1),&length,4);
    check(win64_system_call(&c,"GlobalMemoryStatusEx") && !c.rax && win64_get_error()==87 && *(uint8_t*)win64_pointer(out+4,1,0)==0xa5,"MEMORYSTATUSEX invalid length");
    length=64;memcpy(win64_pointer(out,4,1),&length,4);check(win64_system_call(&c,"GlobalMemoryStatusEx") && c.rax==1,"MEMORYSTATUSEX success");
    guest_memory_status mem;memcpy(&mem,win64_pointer(out,sizeof(mem),0),sizeof(mem));
    check(mem.total_phys && mem.available_phys<=mem.total_phys && mem.available_pagefile<=mem.total_pagefile && mem.available_virtual<=mem.total_virtual &&
          mem.total_virtual<=GUEST_END-GUEST_MIN && !mem.available_extended && mem.load<=100 && *(uint8_t*)win64_pointer(out+64,1,0)==0xa5,"MEMORYSTATUSEX data/bounds");
    const char *names[]={"GetTempPathA","GetTempPathW","GetTempPath2A","GetTempPath2W"};unsigned temp_cases=0;
    for(unsigned n=0;n<4;++n)for(unsigned cap=0;cap<=strlen(temp_path)+2;++cap) {
        memset(win64_pointer(out,1024,1),0xa5,1024);c.rcx=cap;c.rdx=out;
        check(win64_system_call(&c,names[n]) && c.rsp==UINT64_C(0x12345678),"temp path protocol");
        unsigned length=(unsigned)strlen(temp_path),copied=cap?minimum(cap-1,length):0,wide=names[n][strlen(names[n])-1]=='W';
        check(c.rax==(cap<=length?length+1:length),"temp path required size");
        if(cap)for(unsigned i=0;i<=copied;++i) {
            unsigned ch=wide?((uint16_t*)win64_pointer(out,(copied+1)*2,0))[i]:((uint8_t*)win64_pointer(out,copied+1,0))[i];
            check(ch==(i==copied?0:(unsigned char)temp_path[i]),"temp path content/UTF16");
        }
        unsigned touched=cap?(copied+1)*(wide?2:1):0;check(*(uint8_t*)win64_pointer(out+touched,1,0)==0xa5,"temp path output canary");++temp_cases;
    }
    check(!win64_system_call(&c,"UnknownSystemFunction"),"unknown import");win64_free(out);
    fprintf(stderr,"WIN64_SYSTEM_SELF_TEST SYSTEM_INFO=48 MEMORYSTATUSEX=64 processors=%u mask=0x%" PRIx64 " page=%u RAM=%" PRIu64 " available=%" PRIu64 " temp_cases=%u profile_anchor=1 no_rsp_pop=1 passed\n",info.processors,info.active_processor_mask,info.page_size,mem.total_phys,mem.available_phys,temp_cases);
}
