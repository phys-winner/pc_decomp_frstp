/* Real production pointer registry, errno and environment implementation. */
#include "../android/host/win64.c"
#include "win64_environment.h"
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"ENV_FIXTURE_FAIL line=%d\n",__LINE__);return 2;}}while(0)
static char names[11][64]={"FROST_ENV_CASE","frost_env_case","FrOsT_EnV_CaSe","FROST_ENV_EMPTY","FROST_ENV_EQUAL","FROST_ENV_UTF8","FROST_ENV_MISSING","","FROST_ENV_CASE=x","__ALSOFT_HALF_ANGLE_CONES","LD_LIBRARY_PATH"};
static uint16_t wide_names[11][64];
static const char *expected[]={"CaseValue","CaseValue","CaseValue","","A=B","\xc3\xa9\xf0\x9f\x8c\xa8",NULL,NULL,NULL,NULL,NULL};
static uint64_t invoke(const char *api,uint64_t name){CPU cpu,before;memset(&cpu,0xa5,sizeof(cpu));cpu.rcx=name;cpu.rsp=0x12345678;before=cpu;
    if(!win64_environment_call(&cpu,api))abort();uint64_t result=cpu.rax;cpu.rax=before.rax;if(memcmp(&cpu,&before,sizeof(cpu)))abort();return result;}
static int wide_equal(uint64_t address,const char *value){uint16_t expected_wide[16];size_t n=0;
    if(!strcmp(value,"\xc3\xa9\xf0\x9f\x8c\xa8")){expected_wide[n++]=0xe9;expected_wide[n++]=0xd83c;expected_wide[n++]=0xdf28;}
    else while(value[n]){expected_wide[n]=(unsigned char)value[n];++n;}
    expected_wide[n]=0;return !memcmp(win64_pointer(address,(n+1)*2,0),expected_wide,(n+1)*2);}
static void *reader(void *unused){(void)unused;for(unsigned j=0;j<2000;++j)for(unsigned i=0;i<11;++i){
    uint64_t a=invoke("getenv",(uintptr_t)names[i]),w=invoke("_wgetenv",(uintptr_t)wide_names[i]);
    if((a!=0)!=(expected[i]!=NULL)||(w!=0)!=(expected[i]!=NULL)||(a&&strcmp(win64_pointer(a,strlen(expected[i])+1,0),expected[i]))||(w&&!wide_equal(w,expected[i])))abort();}return NULL;}
int main(int argc,char **argv){win64_environment_self_test();
    win64_region_add((uintptr_t)names,sizeof(names),0);for(unsigned i=0;i<11;++i)for(unsigned j=0;j<64;++j)wide_names[i][j]=(unsigned char)names[i][j];win64_region_add((uintptr_t)wide_names,sizeof(wide_names),0);
    const win64_environment_entry values[]={{"FROST_ENV_CASE","CaseValue"},{"FROST_ENV_EMPTY",""},{"FROST_ENV_EQUAL","A=B"},{"FROST_ENV_UTF8","\xc3\xa9\xf0\x9f\x8c\xa8"}};
    if(argc>1&&!strcmp(argv[1],"--unconfigured")){invoke("getenv",(uintptr_t)names[0]);return 99;}
    CHECK(win64_environment_configure(NULL,0));CHECK(win64_environment_configure(values,4));
    if(argc>1){static char overlong[32768];static uint16_t overlong_wide[32768];memset(overlong,'A',32767);for(unsigned i=0;i<32767;++i)overlong_wide[i]='A';win64_region_add((uintptr_t)overlong,sizeof(overlong),0);win64_region_add((uintptr_t)overlong_wide,sizeof(overlong_wide),0);
        if(!strcmp(argv[1],"--null"))invoke("getenv",0);else if(!strcmp(argv[1],"--wide-null"))invoke("_wgetenv",0);else if(!strcmp(argv[1],"--overlong"))invoke("getenv",(uintptr_t)overlong);else if(!strcmp(argv[1],"--wide-overlong"))invoke("_wgetenv",(uintptr_t)overlong_wide);else if(!strcmp(argv[1],"--invalid-pointer"))invoke("getenv",1);else return 3;return 99;}
    const win64_environment_entry duplicate[]={{"TEMP","a"},{"temp","b"}},invalid[]={{"A=B","x"}},bad_utf8[]={{"A","\xed\xa0\x80"}},empty_name[]={{"","x"}};
    CHECK(!win64_environment_configure(duplicate,2));CHECK(!win64_environment_configure(invalid,1));CHECK(!win64_environment_configure(bad_utf8,1));CHECK(!win64_environment_configure(empty_name,1));CHECK(!win64_environment_configure(NULL,1));CHECK(!win64_environment_configure(values,65));
    uint64_t stable=invoke("getenv",(uintptr_t)names[0]),wide_stable=invoke("_wgetenv",(uintptr_t)wide_names[0]);CHECK(stable&&wide_stable);
    for(unsigned i=0;i<11;++i){crt_errno=1234;uint64_t a=invoke("getenv",(uintptr_t)names[i]),w=invoke("_wgetenv",(uintptr_t)wide_names[i]);CHECK((a!=0)==(expected[i]!=NULL));CHECK((w!=0)==(expected[i]!=NULL));CHECK(!a||!strcmp(win64_pointer(a,strlen(expected[i])+1,0),expected[i]));CHECK(!w||wide_equal(w,expected[i]));CHECK(crt_errno==1234);CHECK(!strcmp((const char *)(uintptr_t)stable,"CaseValue"));}
    CHECK(invoke("getenv",(uintptr_t)names[1])==stable);CHECK(invoke("_wgetenv",(uintptr_t)wide_names[1])==wide_stable);CHECK(!accessible(stable,1,1)&&accessible(stable,1,0));CHECK(!accessible(wide_stable,2,1)&&accessible(wide_stable,2,0));
    CHECK(!win64_environment_configure(NULL,0));CHECK(invoke("getenv",(uintptr_t)names[0])==stable);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)CHECK(!pthread_create(&threads[i],NULL,reader,NULL));for(unsigned i=0;i<4;++i)CHECK(!pthread_join(threads[i],NULL));
    printf("{\"success\":true,\"checks\":%u,\"concurrent_queries\":176000,\"threads\":4,\"CPU_and_RSP_preserved\":true,\"published_reconfigure_rejected\":true,\"no_host_environment\":true,\"game_boot_proven\":false}\n",checks);return 0;}
