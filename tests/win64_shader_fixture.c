#include "win64_shader.h"
#include "win64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dxbc_checksum.inc"
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#include <pthread.h>
#define EXPORT __attribute__((visibility("default")))
#endif
typedef struct {uint64_t p;size_t n;int write;} region;
static region regions[256];static unsigned nregions;static unsigned calls;
void win64_region_add(uint64_t p,size_t n,int write){if(nregions==256)abort();regions[nregions++]=(region){p,n,write};}
void win64_region_remove(uint64_t p){for(unsigned i=0;i<nregions;++i)if(regions[i].p==p){regions[i]=regions[--nregions];return;}abort();}
void *win64_pointer(uint64_t p,size_t n,int write){for(unsigned i=0;i<nregions;++i)if(p>=regions[i].p&&p-regions[i].p<=regions[i].n&&n<=regions[i].n-(p-regions[i].p)&&(!write||regions[i].write))return(void*)(uintptr_t)p;fprintf(stderr,"shader fixture invalid range %llx+%zu write%d\n",(unsigned long long)p,n,write);exit(74);}
uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t r[]={c->rcx,c->rdx,c->r8,c->r9};return r[i];}abort();}
uint64_t win64_alloc(size_t n,int zero){void*p=zero?calloc(1,n):malloc(n);if(p)win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p;}
void win64_free(uint64_t p){win64_region_remove(p);free((void*)(uintptr_t)p);}
EXPORT void shader_add(void*p,size_t n,int write){win64_region_add((uintptr_t)p,n,write);}
EXPORT void shader_remove(void*p){win64_region_remove((uintptr_t)p);}
EXPORT uint64_t shader_run(unsigned method,uint64_t a,uint64_t b,uint64_t d,uint64_t e){
    CPU c,before;memset(&c,0xa5,sizeof(c));c.rcx=a;c.rdx=b;c.r8=d;c.r9=e;before=c;
    int hit=method==UINT32_MAX?win64_shader_call(&c,"D3DReflect"):win64_shader_dispatch(&c,WIN64_SHADER_SENTINEL_BASE+method*8);
    if(!hit)abort();uint64_t result=c.rax;c.rax=before.rax;if(memcmp(&c,&before,sizeof(c)))abort();
#if defined(_WIN32)
    ++calls;
#else
    __atomic_fetch_add(&calls,1,__ATOMIC_RELAXED);
#endif
    return result;
}
EXPORT void shader_selftest(void){win64_shader_self_test();}
EXPORT void shader_rehash(void*p,uint32_t n){uint32_t h[4];if(!shader_dxbc_checksum(p,n,h))abort();memcpy((unsigned char*)p+4,h,16);}
#if defined(SHADER_FIXTURE_MAIN)
static void check(int condition,const char*what){if(!condition){fprintf(stderr,"SHADER_CASE_FAIL %s calls%u\n",what,calls);exit(1);}}
static uint32_t read32(FILE*f){uint32_t x;check(fread(&x,4,1,f)==1,"fixture DWORD");return x;}
static void read_bytes(FILE*f,void*p,size_t n){check(!n||fread(p,1,n,f)==n,"fixture bytes");}
static char *read_string(FILE*f){uint32_t n=read32(f);check(n<65536,"fixture string size");char*s=malloc((size_t)n+1);check(s!=NULL,"fixture string allocation");read_bytes(f,s,n);s[n]=0;return s;}
static uint64_t get64(const void*p){uint64_t v;memcpy(&v,p,8);return v;}
static uint32_t get32(const void*p){uint32_t v;memcpy(&v,p,4);return v;}
static void set32(void*p,uint32_t v){memcpy(p,&v,4);}
#if !defined(_WIN32)
typedef struct {uint64_t object;unsigned char desc[160],sig[40];} parallel_case;
static void *parallel_worker(void*arg){parallel_case*t=arg;
    for(unsigned i=0;i<256;++i){check(shader_run(1,t->object,0,0,0)>=2,"parallel AddRef");check(!shader_run(3,t->object,(uintptr_t)t->desc,0,0),"parallel GetDesc");check(!shader_run(7,t->object,0,(uintptr_t)t->sig,0),"parallel signature");check(shader_run(2,t->object,0,0,0)>=1,"parallel Release");}
    return NULL;
}
static void parallel_test(uint64_t object){parallel_case cases[4];pthread_t ids[4];
    for(unsigned i=0;i<4;++i){cases[i].object=object;shader_add(cases[i].desc,160,1);shader_add(cases[i].sig,40,1);}
    for(unsigned i=0;i<4;++i)check(!pthread_create(&ids[i],NULL,parallel_worker,&cases[i]),"pthread create");
    for(unsigned i=0;i<4;++i)check(!pthread_join(ids[i],NULL),"pthread join");
    for(unsigned i=0;i<4;++i){check(get32(cases[i].desc+28)==5,"parallel full descriptor");shader_remove(cases[i].desc);shader_remove(cases[i].sig);}
}
#endif
static unsigned malformed_tests(unsigned char*bytes,uint32_t size,unsigned char*iid,uint64_t*out){
    unsigned char*copy=malloc(size);check(copy!=NULL,"malformed copy allocation");shader_add(copy,size,0);unsigned tests=0;
    for(unsigned i=4;i<20;++i){memcpy(copy,bytes,size);copy[i]^=1;check(shader_run(UINT32_MAX,(uintptr_t)copy,size,(uintptr_t)iid,(uintptr_t)out)==UINT32_C(0x80004005)&&*out==UINT64_C(0x123456789abcdef0),"checksum rejection output untouched");++tests;}
    uint32_t count=get32(bytes+28),first=get32(bytes+32),isgn=0,rdef=0;
    for(uint32_t i=0;i<count;++i){uint32_t off=get32(bytes+32+i*4);if(!memcmp(bytes+off,"ISGN",4))isgn=off;if(!memcmp(bytes+off,"RDEF",4))rdef=off;}
    check(isgn&&rdef,"malformed fixture chunk presence");
    for(unsigned i=0;i<9;++i){memcpy(copy,bytes,size);
        if(i==0)set32(copy+28,UINT32_MAX);
        if(i==1)set32(copy+32,size-4);
        if(i==2)set32(copy+first+4,UINT32_MAX);
        if(i==3)set32(copy+36,first);
        if(i==4)set32(copy+isgn+8,UINT32_MAX);
        if(i==5)set32(copy+isgn+16,UINT32_MAX);
        if(i==6)set32(copy+rdef+8+24,UINT32_MAX);
        if(i==7)set32(copy+rdef+8,UINT32_MAX);
        if(i==8){uint32_t name=get32(copy+isgn+16);memset(copy+isgn+8+name,'X',get32(copy+isgn+4)-name);}
        shader_rehash(copy,size);check(shader_run(UINT32_MAX,(uintptr_t)copy,size,(uintptr_t)iid,(uintptr_t)out)==UINT32_C(0x8876086c)&&*out==UINT64_C(0x123456789abcdef0),"bounded malformed checksum-valid input");++tests;
    }
    shader_remove(copy);free(copy);return tests;
}
int main(int argc,char**argv){
    check(argc>=2,"fixture path argument");FILE*f=fopen(argv[1],"rb");check(f!=NULL,"fixture open");check(read32(f)==UINT32_C(0x58494653),"fixture magic");uint32_t count=read32(f);check(count==1227,"fixture1227 count");shader_selftest();
    unsigned total_inputs=0,texcoord1=0,malformed=0,parallel=0;
    for(uint32_t i=0;i<count;++i){
        uint32_t size=read32(f);unsigned char expected[160];read_bytes(f,expected,160);char*creator=read_string(f);uint32_t count_inputs=read32(f);
        unsigned char*expected_input=malloc((size_t)count_inputs*40+1);char**names=calloc((size_t)count_inputs+1,sizeof(char*));check(expected_input&&names,"expected signatures allocation");
        for(uint32_t j=0;j<count_inputs;++j){read_bytes(f,expected_input+(size_t)j*40,40);names[j]=read_string(f);}
        unsigned char*bytes=malloc(size);check(bytes!=NULL,"bytecode allocation");read_bytes(f,bytes,size);
        unsigned char iid[16]={0xa1,0x6c,0x53,0x8d,0xca,0x0c,0x56,0x49,0xa8,0x37,0x78,0x69,0x63,0x75,0x55,0x84};
        unsigned char unk[16]={0,0,0,0,0,0,0,0,0xc0,0,0,0,0,0,0,0x46};
        unsigned char desc[176],sig[56];uint64_t object=UINT64_C(0x123456789abcdef0),query=0;
        shader_add(bytes,size,0);shader_add(iid,16,0);shader_add(unk,16,0);shader_add(&object,8,1);shader_add(&query,8,1);shader_add(desc,sizeof(desc),1);shader_add(sig,sizeof(sig),1);
        if(!i){
            iid[0]^=1;check(shader_run(UINT32_MAX,(uintptr_t)bytes,size,(uintptr_t)iid,(uintptr_t)&object)==UINT32_C(0x80070057)&&object==UINT64_C(0x123456789abcdef0),"bad IID output untouched");iid[0]^=1;
            for(unsigned n=0;n<32;++n)check(shader_run(UINT32_MAX,(uintptr_t)bytes,n,(uintptr_t)iid,(uintptr_t)&object)==UINT32_C(0x8876086c)&&object==UINT64_C(0x123456789abcdef0),"truncated header");
            malformed=malformed_tests(bytes,size,iid,&object);
        }
        check(shader_run(UINT32_MAX,(uintptr_t)bytes,size,(uintptr_t)iid,(uintptr_t)&object)==0,"original DXBC reflect");
        shader_remove(bytes);memset(bytes,0xdd,size);free(bytes); /* object must own all returned strings */
        if(argc>2){if(!strcmp(argv[2],"released")){shader_run(2,object,0,0,0);shader_run(3,object,(uintptr_t)desc,0,0);}else if(!strcmp(argv[2],"foreign"))shader_run(3,object+8,(uintptr_t)desc,0,0);else if(!strcmp(argv[2],"unknownslot"))shader_run(4,object,0,0,0);else abort();return 99;}
        check(shader_run(0,object,(uintptr_t)unk,(uintptr_t)&query,0)==0&&query==object,"IUnknown canonical identity");
        check(shader_run(2,query,0,0,0)==1,"QI reference release");check(shader_run(1,object,0,0,0)==2,"AddRef count");check(shader_run(2,object,0,0,0)==1,"Release count");
#if !defined(_WIN32)
        if(!i){parallel_test(object);parallel=4096;}
#endif
        memset(desc,0xa5,sizeof(desc));check(shader_run(3,object,(uintptr_t)desc,0,0)==0,"GetDesc HRESULT");
        uint64_t ptr=get64(desc+8);check(!strcmp(win64_pointer(ptr,strlen(creator)+1,0),creator),"Creator full string");memset(desc+8,0,8);memset(desc+4,0,4);
        if(memcmp(desc,expected,160)){for(unsigned k=0;k<160;k+=4)if(memcmp(desc+k,expected+k,4))fprintf(stderr,"record%u descOffset%u actual%u expected%u\n",i,k,*(uint32_t*)(desc+k),*(uint32_t*)(expected+k));check(0,"all GetDesc fields");}
        for(unsigned k=160;k<sizeof(desc);++k)check(desc[k]==0xa5,"GetDesc destination canary");
        check(shader_run(3,object,0,0,0)==UINT32_C(0x80004005),"GetDesc null output");
        int has=0;for(uint32_t j=0;j<count_inputs;++j){memset(sig,0xa5,sizeof(sig));check(shader_run(7,object,j,(uintptr_t)sig,0)==0,"GetInputParameterDesc HRESULT");ptr=get64(sig);check(!strcmp(win64_pointer(ptr,strlen(names[j])+1,0),names[j]),"SemanticName full string");memset(sig,0,8);check(!memcmp(sig,expected_input+(size_t)j*40,40),"all signature fields");for(unsigned k=40;k<sizeof(sig);++k)check(sig[k]==0xa5,"signature destination canary");if(!strcmp(names[j],"TEXCOORD")&&*(uint32_t*)(sig+8)==1)has=1;}
        memset(sig,0xa5,sizeof(sig));check(shader_run(7,object,count_inputs,(uintptr_t)sig,0)==UINT32_C(0x80070057),"out-of-range index");for(unsigned k=0;k<sizeof(sig);++k)check(sig[k]==0xa5,"invalid index output untouched");
        check(shader_run(2,object,0,0,0)==0,"final Release zero");total_inputs+=count_inputs;texcoord1+=has;
        shader_remove(sig);shader_remove(desc);shader_remove(&query);shader_remove(&object);shader_remove(unk);shader_remove(iid);
        free(creator);for(uint32_t j=0;j<count_inputs;++j)free(names[j]);free(names);free(expected_input);
    }
    check(fgetc(f)==EOF,"fixture trailing bytes");fclose(f);
    printf("{\"success\":true,\"unique_shaders\":%u,\"input_parameters\":%u,\"unique_TEXCOORD1\":%u,\"Win64_calls\":%u,\"malformed_cases\":%u,\"parallel_calls\":%u,\"game_boot_proven\":false}\n",count,total_inputs,texcoord1,calls,malformed,parallel);return 0;
}
#endif
