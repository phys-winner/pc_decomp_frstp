#include "win64_rtti.h"
#include "win64.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xmmintrin.h>
#include <malloc.h>
#define TLS __declspec(thread)
#define ALIGN16 __declspec(align(16))
#define strcasecmp _stricmp
#else
#include <pthread.h>
#include <strings.h>
#define TLS _Thread_local
#define ALIGN16 _Alignas(16)
#endif
typedef struct {uint64_t cache;char decorated[512];} descriptor;
typedef struct {uint64_t address;size_t bytes;int write;} region;
static region ranges[32];static unsigned range_count;
static void *allocations[32];static unsigned allocation_count;static int allocation_fail;
static TLS uint32_t last_error,guest_errno;
static descriptor data[14];static char names[11][512];static uint64_t expected_low[11];
static ALIGN16 uint64_t root[2],race_root[2];
static unsigned calls;
static void check(int okay,const char *why){if(!okay){fprintf(stderr,"TYPE_INFO_FIXTURE_FAIL %s\n",why);_Exit(1);}}
static void add(void*p,size_t bytes,int write){check(range_count<32,"region capacity");ranges[range_count++]=(region){(uintptr_t)p,bytes,write};}
void *win64_pointer(uint64_t address,size_t bytes,int writing){
    for(unsigned i=0;i<range_count;++i){region*r=&ranges[i];if(address>=r->address&&address-r->address<=r->bytes&&bytes<=r->bytes-(address-r->address)&&(!writing||r->write))return(void*)(uintptr_t)address;}
    fprintf(stderr,"STOP Win64 RTTI fixture checked span\n");_Exit(73);
}
uint32_t win64_get_error(void){return last_error;}
void win64_set_error(uint32_t value){last_error=value;}
void win64_stdio_set_errno(int value){guest_errno=(uint32_t)value;}
uint64_t win64_alloc(size_t bytes,int zero){
    last_error=8;errno=12;if(allocation_fail)return 0;void*p=NULL;
#if defined(_WIN32)
    p=_aligned_malloc(bytes,16);
#else
    if(posix_memalign(&p,16,bytes))p=NULL;
#endif
    check(p!=NULL&&allocation_count<32,"fixture allocation");if(zero)memset(p,0,bytes);allocations[allocation_count++]=p;return(uintptr_t)p;
}
static void fp_get(uint64_t v[2]){
#if defined(__aarch64__)
    __asm__ volatile("mrs %0,fpcr\n\tmrs %1,fpsr":"=r"(v[0]),"=r"(v[1]));
#else
    v[0]=_mm_getcsr();v[1]=0;
#endif
}
static void fp_set(const uint64_t v[2]){
#if defined(__aarch64__)
    __asm__ volatile("msr fpcr,%0\n\tmsr fpsr,%1"::"r"(v[0]),"r"(v[1]):"memory");
#else
    _mm_setcsr((unsigned)v[0]);
#endif
}
static int imported(CPU*c,const char*dll,const char*name){
#include "rtti_route.inc"
    return 0;
}
static uint64_t invoke(descriptor*d,uint64_t *head,unsigned mode,int route){
    CPU c;unsigned char *p=(unsigned char*)&c;for(unsigned i=0;i<sizeof(c);++i)p[i]=(unsigned char)(i*13+mode*7);
    c.rcx=(uintptr_t)d;c.rdx=(uintptr_t)head;c.rsp=UINT64_C(0x123456781000);CPU expected=c;if(route)expected.rsp+=8;
    uint64_t original[2],wanted[2],after[2];fp_get(original);
#if defined(__aarch64__)
    wanted[0]=(original[0]&~(UINT64_C(3)<<22))|((uint64_t)(mode&3)<<22)|(UINT64_C(1)<<24);wanted[1]=0x15;
#else
    wanted[0]=0x9fd5;wanted[1]=0;
#endif
    fp_set(wanted);last_error=0x5a17;guest_errno=1379;errno=2718;
#if defined(_WIN32)
    SetLastError(0x5a17);
#endif
    int handled=route?imported(&c,"VcRuNtImE140.DlL","__std_type_info_name"):win64_rtti_call(&c,"__std_type_info_name");
    int e=errno;fp_get(after);expected.rax=c.rax;
    check(handled&&!memcmp(&c,&expected,sizeof(c)),"full CPU and single import pop");
    check(e==2718&&last_error==0x5a17,"errno and guest LastError");check(!memcmp(wanted,after,sizeof(wanted)),"native FP state");
    check(guest_errno==(allocation_fail?12u:1379u),"native guest CRT errno success/OOM contract");
#if defined(_WIN32)
    check(GetLastError()==0x5a17,"native LastError");
#endif
    fp_set(original);return c.rax;
}
#if defined(_WIN32)
static DWORD WINAPI worker(void *arg){
#else
static void *worker(void *arg){
#endif
    unsigned mode=(unsigned)(uintptr_t)arg;
    for(unsigned i=0;i<2048;++i){uint64_t result=invoke(&data[13],race_root,mode,i&1);check(result&&strcmp((char*)(uintptr_t)result,names[0])==0,"parallel cached name");}
    return 0;
}
int main(int argc,char**argv){
    if(argc<2)return 2;FILE*f=fopen(argv[1],"rb");check(f!=NULL,"dataset open");uint32_t count;check(fread(&count,4,1,f)==1&&count==11,"dataset count");
    for(unsigned i=0;i<count;++i){uint32_t rva,dc,nc;check(fread(&rva,4,1,f)==1&&fread(&dc,4,1,f)==1&&fread(&nc,4,1,f)==1&&fread(&expected_low[i],8,1,f)==1,"row header");
        check(dc>1&&dc<=512&&nc>1&&nc<=512,"row lengths");check(fread(data[i].decorated,dc,1,f)==1&&fread(names[i],nc,1,f)==1,"row data");check(!data[i].decorated[dc-1]&&!names[i][nc-1],"row terminators");}
    check(fgetc(f)==EOF,"dataset tail");fclose(f);
    add(data,sizeof(data),1);add(root,sizeof(root),1);add(race_root,sizeof(race_root),1);
    if(argc==3){const char *negative=argv[2];descriptor *d=&data[0];uint64_t *head=root;
        if(!strcmp(negative,"unknown"))strcpy(d->decorated,".?AVUnknownFixture@@");
        else if(!strcmp(negative,"unterminated"))memset(d->decorated,'x',sizeof(d->decorated));
        else if(!strcmp(negative,"null_data"))d=NULL;
        else if(!strcmp(negative,"null_root"))head=NULL;
        else if(!strcmp(negative,"unaligned_root"))head=(uint64_t*)((char*)root+8);
        else if(!strcmp(negative,"reserved_root"))root[1]=1;
        else if(!strcmp(negative,"readonly_cache"))ranges[0].write=0;
        else if(!strcmp(negative,"readonly_root"))ranges[1].write=0;
        else return 2;invoke(d,head,0,1);return 99;
    }
    uint64_t previous=0;
    for(unsigned i=0;i<count;++i){descriptor before=data[i];uint64_t result=invoke(&data[i],root,i,i&1);++calls;
        check(result&&data[i].cache==result&&!strcmp((char*)(uintptr_t)result,names[i]),"native oracle name/cache");
        before.cache=result;check(!memcmp(&before,&data[i],sizeof(before)),"descriptor decorated data unchanged");
        uint64_t node=result-16;check(root[0]==expected_low[i]&&root[1]==node&&*(uint64_t*)(uintptr_t)node==previous&&!*(uint64_t*)(uintptr_t)(node+8),"native oracle SLIST depth sequence chain");
        uint64_t before_root[2]={root[0],root[1]};unsigned before_alloc=allocation_count;
        check(invoke(&data[i],NULL,i,1)==result&&!memcmp(root,before_root,16)&&allocation_count==before_alloc,"cached ignores root and does not allocate");++calls;previous=node;
    }
    strcpy(data[11].decorated,data[0].decorated);uint64_t clone=invoke(&data[11],root,1,0);++calls;
    check(clone!=data[0].cache&&!strcmp((char*)(uintptr_t)clone,names[0])&&root[0]==UINT64_C(12)*0x20001,"separate descriptor has separate native cache node");
    strcpy(data[12].decorated,data[0].decorated);allocation_fail=1;uint64_t before_root[2]={root[0],root[1]};
    check(invoke(&data[12],root,2,1)==0&&!data[12].cache&&!memcmp(root,before_root,16)&&allocation_count==12,"allocation failure leaves cache/list unchanged");++calls;allocation_fail=0;
    strcpy(data[13].decorated,data[0].decorated);
#if defined(_WIN32)
    HANDLE threads[8];for(unsigned i=0;i<8;++i){threads[i]=CreateThread(NULL,0,worker,(void*)(uintptr_t)i,0,NULL);check(threads[i]!=NULL,"thread create");}check(WaitForMultipleObjects(8,threads,TRUE,INFINITE)==WAIT_OBJECT_0,"thread wait");for(unsigned i=0;i<8;++i)CloseHandle(threads[i]);
#else
    pthread_t threads[8];for(unsigned i=0;i<8;++i)check(!pthread_create(&threads[i],NULL,worker,(void*)(uintptr_t)i),"thread create");for(unsigned i=0;i<8;++i)check(!pthread_join(threads[i],NULL),"thread join");
#endif
    check(allocation_count==13&&race_root[0]==0x20001&&race_root[1]==data[13].cache-16&&!*(uint64_t*)(uintptr_t)race_root[1],"parallel publication allocates one node");
    CPU c={0},before=c;check(!win64_rtti_call(&c,"__std_type_info_compare")&&!memcmp(&c,&before,sizeof(c)),"other API declines");
    check(!imported(&c,"ucrtbase.dll","__std_type_info_name")&&!memcmp(&c,&before,sizeof(c)),"wrong DLL declines");
    check(!win64_rtti_call(&c,"__STD_TYPE_INFO_NAME")&&!memcmp(&c,&before,sizeof(c)),"case-sensitive API declines");
    for(unsigned i=0;i<allocation_count;++i){
#if defined(_WIN32)
        _aligned_free(allocations[i]);
#else
        free(allocations[i]);
#endif
    }
    printf("{\"success\":true,\"native_names\":11,\"serial_calls\":%u,\"parallel_calls\":16384,\"cache_list_CPU_FP_errno_LastError\":true,\"one_import_pop\":true,\"allocation_failure\":true}\n",calls);return 0;
}
