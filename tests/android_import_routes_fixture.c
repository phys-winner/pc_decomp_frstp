/* Reuses the existing CRT fixture's region, FILE, errno and complete-CPU gates. */
#include "route_support.inc"
#include <time.h>
static uint64_t ns(void){struct timespec t;check(!clock_gettime(CLOCK_MONOTONIC,&t),"clock");return(uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static CPU input(void){CPU c;memset(&c,0xa5,sizeof(c));c.rsp=UINT64_C(0x123456789abcd000);c.rcx=(uintptr_t)destination;c.rdx=(uintptr_t)source;return c;}
int main(int argc,char **argv){
    win64_region_add((uintptr_t)destination,sizeof(destination),1);win64_region_add((uintptr_t)source,sizeof(source),1);
    if(argc==3&&!strcmp(argv[1],"--fault")){
        CPU c=input();c.rcx=UINT64_MAX;c.rdx=1;
        (void)invoke(!strcmp(argv[2],"candidate"),"ucrtbase.dll","strcmp",c);return 99;
    }
    const char *dlls[]={"msvcrt.dll","UCRTBASE.DLL","api-ms-win-crt-string-l1-1-0.dll","vCrUnTiMe140.DlL"};
    for(unsigned d=0;d<4;++d)for(unsigned length=0;length<=64;++length)for(unsigned mode=0;mode<8;++mode){
        for(unsigned i=0;i<256;++i)source[i]=destination[i]=(unsigned char)(33+(i*17+mode*31)%220);
        source[length]=destination[length]=0;
        if(mode&1)destination[length/2]^=0x80;
        if(mode&2)source[length/2]=0;
        if(mode&4)destination[0]=0;
        unsigned char original_source[256],original_destination[256];memcpy(original_source,source,256);memcpy(original_destination,destination,256);
        CPU c=input();Result a=invoke(0,dlls[d],"strcmp",c),b=invoke(1,dlls[d],"strcmp",c);
        check(!memcmp(&a.cpu,&b.cpu,sizeof(CPU)),"strcmp full CPU");
        CPU expected=c;expected.rax=(uint64_t)(int64_t)strcmp((char*)destination,(char*)source);expected.rsp+=8;
        check(!memcmp(&b.cpu,&expected,sizeof(CPU)),"strcmp native value/single pop");
        check(!memcmp(original_source,source,256)&&!memcmp(original_destination,destination,256),"strcmp does not modify strings");
        check(a.native_errno==b.native_errno&&a.crt_errno==b.crt_errno&&a.error==b.error,"error states");
        if(!d&&!length&&!mode)check(a.registration_count==4&&b.registration_count==4,"cold FILE registration");
    }
    const char *names[]={"msvcrt.dll","ucrtbase.dll","api-ms-win-crt-","vcruntime140.dll","kernel32.dll","","api-ms-win-crt-string-l1-1-0.dll","vcruntime140.dll.extra"};
    uint64_t classifications=0;char name[96];
    for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i){
        size_t n=strlen(names[i]);
        for(size_t pos=0;pos<=n;++pos)for(unsigned ch=0;ch<256;++ch){
            memset(name,0,sizeof(name));memcpy(name,names[i],n);name[pos]=(char)ch;
            check(before_is_crt(name)==after_is_crt(name),"CRT exact classification");++classifications;
        }
    }
    uint32_t seed=UINT32_C(0x31415926);
    for(unsigned i=0;i<20000;++i){
        for(unsigned j=0;j<sizeof(name)-1;++j){seed=seed*1664525u+1013904223u;name[j]=(char)(seed>>24);}name[95]=0;
        check(before_is_crt(name)==after_is_crt(name),"random DLL classification");++classifications;
    }
    for(unsigned variant=0;variant<2;++variant){CPU a=input(),b=a;
        int yes=variant?candidate_route(&a,"kernel32.dll","strcmp"):baseline_route(&a,"kernel32.dll","strcmp");
        check(!yes&&!memcmp(&a,&b,sizeof(a)),"wrong DLL declines with CPU unchanged");}
    strcpy((char*)source,"Frostpunk shader name");strcpy((char*)destination,"Frostpunk shader name");
    uint64_t elapsed[2],checksum[2]={0};const unsigned iterations=100000;
    for(unsigned variant=0;variant<2;++variant){uint64_t start=ns();
        for(unsigned i=0;i<iterations;++i){CPU c=input();c.r8=i;int yes=variant?candidate_route(&c,dlls[i&3],"strcmp"):baseline_route(&c,dlls[i&3],"strcmp");check(yes,"bench handles strcmp");checksum[variant]+=c.rax+c.rsp;}
        elapsed[variant]=ns()-start;
    }
    check(checksum[0]==checksum[1],"benchmark checksum");
    printf("{\"success\":true,\"paired_calls\":%"PRIu64",\"classifications\":%"PRIu64",\"whole_CPU\":true,\"cold_FILE\":true,\"baseline_ns\":%"PRIu64",\"candidate_ns\":%"PRIu64",\"pure_declines_stubbed\":true}\n",checks,classifications,elapsed[0],elapsed[1]);
    return 0;
}
