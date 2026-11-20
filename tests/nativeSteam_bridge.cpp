#include "bridge.h"
#include "win64.h"
#include "steam_metadata.h"
#include "isteamuserstats.h"
#include "isteamremotestorage.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dlfcn.h>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <unistd.h>
namespace{
struct Region{uint64_t p;size_t n;int write;};std::vector<Region>regions;std::recursive_mutex lock;
thread_local uint64_t test_stack;uint64_t apps_proxy;std::atomic<unsigned> invocations{0};unsigned context_calls,callback_calls,result_calls;uint64_t expected_async;
int concurrency_mode;std::atomic<bool>callback_entered{false},worker_done{false},shutdown_started{false},shutdown_finished{false};
unsigned achievement_cases;
[[noreturn]]void fail(const char*s){fprintf(stderr,"FAIL Win64 local Steam bridge %s\n",s);exit(1);}
void require(bool ok,const char*s){if(!ok)fail(s);}
uint64_t read(uint64_t p){uint64_t x;memcpy(&x,win64_pointer(p,8,0),8);return x;}
void write(uint64_t p,uint64_t x){memcpy(win64_pointer(p,8,1),&x,8);}
uint64_t text(const char*s){uint64_t p=win64_alloc(strlen(s)+1,0);memcpy(win64_pointer(p,strlen(s)+1,1),s,strlen(s)+1);return p;}
CPU cpu(std::initializer_list<uint64_t>a){CPU c={};c.rsp=test_stack+3072;c.mxcsr=0x1f80;uint64_t*r[]={&c.rcx,&c.rdx,&c.r8,&c.r9};unsigned i=0;for(uint64_t v:a){if(i<4)*r[i]=v;else write(c.rsp+0x28+(i-4)*8,v);++i;}return c;}
CPU imported(const char*name,std::initializer_list<uint64_t>a={}){CPU c=cpu(a);uint64_t rsp=c.rsp;require(win64_steam_import(&c,"steam_api64.dll",name),name);require(c.rsp==rsp,"flat import changed RSP");++invocations;return c;}
unsigned slot(Kind k,const char*name,unsigned occurrence=0){for(unsigned i=0;i<slot_counts[unsigned(k)];++i)if(!strcmp(slot_names[unsigned(k)][i],name)){if(!occurrence--)return i;}fail("fixture method absent from exact SDK");}
CPU method(uint64_t object,Kind k,const char*name,std::initializer_list<uint64_t>a={},unsigned occurrence=0){CPU c=cpu({object});unsigned i=1;for(uint64_t v:a){if(i==1)c.rdx=v;else if(i==2)c.r8=v;else if(i==3)c.r9=v;else write(c.rsp+0x28+(i-4)*8,v);++i;}uint64_t rsp=c.rsp;require(win64_steam_dispatch(&c,read(read(object)+8*slot(k,name,occurrence))),name);require(c.rsp==rsp,"interface method changed RSP");++invocations;return c;}
uint64_t factory(Kind k,uint64_t user){uint64_t v=text(versions[unsigned(k)]);CPU c=k==Kind::ISteamClient?imported("SteamInternal_CreateInterface",{v}):imported("SteamInternal_FindOrCreateUserInterface",{user,v});win64_free(v);require(c.rax!=0,"version factory");return c.rax;}
uint64_t callback(int id,int bytes){uint64_t table=win64_alloc(24,1);write(table,0x20010020);write(table+8,0x20010030);write(table+16,0x20010010);uint64_t p=win64_alloc(24,1);write(p,table);memcpy(win64_pointer(p+12,4,1),&id,4);memcpy(win64_pointer(p+20,4,1),&bytes,4);return p;}
void achievement_settings(const char *profile){
    static const char contents[]="[{\"name\":\"FROSTPUNK_BRIDGE_PROBE_ACHIEVEMENT\",\"displayName\":\"Fixture\",\"description\":\"Fixture\",\"hidden\":\"0\"}]\n";
    std::string path=std::string(profile)+"/steam_settings/achievements.json";FILE *file=fopen(path.c_str(),"wx");
    if(file){require(fwrite(contents,1,sizeof(contents)-1,file)==sizeof(contents)-1&&fclose(file)==0,"own achievement fixture config");}
    else{require(errno==EEXIST,"exclusive achievement fixture config create");file=fopen(path.c_str(),"rb");char saved[sizeof(contents)];require(file&&fread(saved,1,sizeof(saved),file)==sizeof(contents)-1&&feof(file)&&!memcmp(saved,contents,sizeof(contents)-1)&&fclose(file)==0,"preserve/reuse only exact own achievement fixture config");}
}
void achievement_read_cases(void *dso,uint64_t user,uint64_t stats){
    auto find=reinterpret_cast<void*(*)(int32,const char*)>(dlsym(dso,"SteamInternal_FindOrCreateUserInterface"));require(find!=nullptr,"native stats factory oracle");
    auto *native=static_cast<ISteamUserStats*>(find((int32)user,"STEAMUSERSTATS_INTERFACE_VERSION012"));require(native!=nullptr,"native stats oracle object");
    require(native->GetNumAchievements()!=0&&!strcmp(native->GetAchievementName(0),"FROSTPUNK_BRIDGE_PROBE_ACHIEVEMENT"),"real known achievement fixture DB loaded");
    const char *names[]={"FROSTPUNK_BRIDGE_PROBE_ACHIEVEMENT","frostpunk_bridge_probe_achievement","FROSTPUNK_BRIDGE_PROBE_UNKNOWN","","probe:achievement","/probe","\\probe","../probe","C:\\probe",nullptr};
    for(unsigned index=0;index<sizeof(names)/sizeof(*names);++index)for(unsigned seed=0;seed<2;++seed){
        struct Output{unsigned char before[16];bool value;unsigned char after[16];};static_assert(sizeof(Output)==33,"one-byte native bool/output canaries");
        Output expected,actual;memset(&expected,0xa5,sizeof(expected));expected.value=seed!=0;actual=expected;
        bool result=native->GetAchievement(names[index],&expected.value);require(result==(index<2),"native known/unknown achievement result");
        if(!result)require(expected.value==(seed!=0),"native unknown achievement leaves output untouched");
        uint64_t name=names[index]?text(names[index]):0;win64_region_add((uintptr_t)&actual.value,1,1);
        CPU c;memset(&c,0xa7,sizeof(c));c.rsp=test_stack+3072;c.rcx=stats;c.rdx=name;c.r8=(uintptr_t)&actual.value;CPU before=c;before.rax=result?1:0;
        uint64_t stack_before[8];memcpy(stack_before,win64_pointer(c.rsp,sizeof(stack_before),0),sizeof(stack_before));errno=1927;
        require(win64_steam_dispatch(&c,read(read(stats)+8*slot(Kind::ISteamUserStats,"GetAchievement"))),"typed GetAchievement route");
        require(!memcmp(&actual,&expected,sizeof(actual)),"native/typed exact bool and32guard bytes");
        require(!memcmp(&c,&before,sizeof(c))&&!memcmp(stack_before,win64_pointer(c.rsp,sizeof(stack_before),0),sizeof(stack_before)),"GetAchievement full CPU/RSP/flags/XMM/stack unchanged");
        require(errno==1927,"GetAchievement native errno unchanged");
        win64_region_remove((uintptr_t)&actual.value);if(name)win64_free(name);++achievement_cases;
    }
}
}
extern "C" void win64_region_add(uint64_t p,size_t n,int w){std::lock_guard g(lock);regions.push_back({p,n,w});}
extern "C" void win64_region_remove(uint64_t p){std::lock_guard g(lock);auto it=std::find_if(regions.begin(),regions.end(),[&](const Region&r){return r.p==p;});require(it!=regions.end(),"remove unknown region");regions.erase(it);}
extern "C" void*win64_pointer(uint64_t p,size_t n,int w){std::lock_guard g(lock);for(auto&r:regions)if(p>=r.p&&p-r.p<=r.n&&n<=r.n-(p-r.p)&&(!w||r.write))return(void*)(uintptr_t)p;fail("invalid guest pointer/range/write permission");}
extern "C" const char*win64_string_a(uint64_t p){for(size_t n=0;n<32768;++n)if(!*(char*)win64_pointer(p+n,1,0))return(const char*)(uintptr_t)p;fail("string bound");}
extern "C" uint64_t win64_argument(CPU*c,unsigned i){if(i<4){uint64_t a[]={c->rcx,c->rdx,c->r8,c->r9};return a[i];}return read(c->rsp+0x28+(i-4)*8);}
extern "C" uint64_t win64_alloc(size_t n,int zero){void*p=zero?calloc(1,n):malloc(n);require(p!=nullptr,"allocation");win64_region_add((uintptr_t)p,n,1);return(uintptr_t)p;}
extern "C" void win64_free(uint64_t p){win64_region_remove(p);free((void*)(uintptr_t)p);}
extern "C" void dispatch(CPU*c,uint64_t target){
    if(target==0x20010000){write(c->rcx,apps_proxy);++context_calls;c->rax=1;}
    else if(target==0x20010010){int bytes;memcpy(&bytes,win64_pointer(c->rcx+20,4,0),4);c->rax=(uint32_t)bytes;}
    else if(target==0x20010020){require(read(c->rdx)==323190,"callback game id");int result;memcpy(&result,win64_pointer(c->rdx+8,4,0),4);require(result==1&&read(c->rdx+12)!=0,"callback exact packed result/id");++callback_calls;
        if(concurrency_mode==2)imported("SteamAPI_Shutdown");
        if(concurrency_mode==1){callback_entered=true;for(unsigned i=0;i<2000&&(!worker_done||!shutdown_started);++i)usleep(1000);require(worker_done&&shutdown_started,"callback worker service call must complete during pending shutdown");require(!shutdown_finished,"shutdown must retain native backend until callback returns");}
    }
    else if(target==0x20010030){require(!c->r8&&c->r9==expected_async,"callresult Win64 IO/handle arguments");require(read(c->rdx)==expected_async,"callresult original handle");uint32_t bytes;memcpy(&bytes,win64_pointer(c->rdx+16,4,0),4);require(bytes==8,"callresult original byte count");++result_calls;}
    else fail("non-static guest callback target");c->rsp+=8;
}
int main(int argc,char**argv){
    require(argc==4||argc==5,"usage <local-backend-DSO> <existing-private-profile> <game-directory> [negative-test]");
    void*dso=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!dso)fprintf(stderr,"dlopen=%s\n",dlerror());require(dso!=nullptr,"native DSO load");
    auto configure=reinterpret_cast<int(*)(const char*,const char*)>(dlsym(dso,"FrostpunkOffline_Configure"));require(configure&&configure(argv[2],argv[3]),"local profile configuration");require(win64_steam_configure(dso),"typed bridge configure");
    achievement_settings(argv[2]);test_stack=win64_alloc(4096,1);require(!imported("SteamAPI_RestartAppIfNecessary",{323190}).rax,"local backend needs no Steam launcher");require(imported("SteamAPI_Init").rax==1,"real local backend initialized");uint64_t user=imported("SteamAPI_GetHSteamUser").rax;
    uint64_t p[unsigned(Kind::Count)];for(unsigned k=0;k<unsigned(Kind::Count);++k){p[k]=factory(Kind(k),user);require(p[k]==factory(Kind(k),user),"canonical interface identity");}
    apps_proxy=p[unsigned(Kind::ISteamApps)];uint64_t user_proxy=p[unsigned(Kind::ISteamUser021)],stats=p[unsigned(Kind::ISteamUserStats)],storage=p[unsigned(Kind::ISteamRemoteStorage)];
    if(argc==5&&!strcmp(argv[4],"shutdown-race"))concurrency_mode=1;
    else if(argc==5&&!strcmp(argv[4],"shutdown-self"))concurrency_mode=2;
    else if(argc==5){
        if(!strcmp(argv[4],"path-traversal"))method(storage,Kind::ISteamRemoteStorage,"FileRead",{text("../outside"),win64_alloc(1,1),1});
        else if(!strcmp(argv[4],"absolute-path"))method(storage,Kind::ISteamRemoteStorage,"FileRead",{text("/sdcard/outside"),win64_alloc(1,1),1});
        else if(!strcmp(argv[4],"size-overrun"))method(storage,Kind::ISteamRemoteStorage,"FileWrite",{text("negative-test.bin"),win64_alloc(1,1),8});
        else if(!strcmp(argv[4],"negative-file-count"))method(storage,Kind::ISteamRemoteStorage,"FileRead",{text("negative-test.bin"),win64_alloc(1,1),UINT32_MAX});
        else if(!strcmp(argv[4],"oversized-file-count"))method(storage,Kind::ISteamRemoteStorage,"FileWrite",{text("negative-test.bin"),win64_alloc(1,1),100u*1024u*1024u+1});
        else if(!strcmp(argv[4],"negative-stream-count"))method(storage,Kind::ISteamRemoteStorage,"FileWriteStreamWriteChunk",{1,win64_alloc(1,1),UINT32_MAX});
        else if(!strcmp(argv[4],"oversized-async-count"))method(storage,Kind::ISteamRemoteStorage,"FileReadAsyncComplete",{1,win64_alloc(1,1),100u*1024u*1024u+1});
        else if(!strcmp(argv[4],"async-size-overrun"))method(storage,Kind::ISteamRemoteStorage,"FileReadAsyncComplete",{1,win64_alloc(1,1),8});
        else if(!strcmp(argv[4],"wrong-kind")){CPU c=cpu({user_proxy});win64_steam_dispatch(&c,read(read(apps_proxy)+8*slot(Kind::ISteamApps,"GetCurrentGameLanguage")));}
        else if(!strcmp(argv[4],"unknown-slot"))method(user_proxy,Kind::ISteamUser021,"GetAuthSessionTicket");
        else if(!strcmp(argv[4],"callback-size"))imported("SteamAPI_RegisterCallback",{callback(1101,8),1101});
        fail("negative contract unexpectedly returned");
    }
    require(method(user_proxy,Kind::ISteamUser021,"GetHSteamUser").rax==user,"native user handle");require(!method(user_proxy,Kind::ISteamUser021,"BLoggedOn").rax,"offline connection status");
    uint64_t id=win64_alloc(8,1),owner=win64_alloc(8,1);require(method(user_proxy,Kind::ISteamUser021,"GetSteamID",{id}).rax==id&&read(id)!=0,"MSVC hidden return buffer/RAX");require(method(apps_proxy,Kind::ISteamApps,"GetAppOwner",{owner}).rax==owner&&read(owner)==read(id),"app owner hidden return");
    uint64_t install_path=win64_alloc(4096,1),user_path=win64_alloc(4096,1);require(method(apps_proxy,Kind::ISteamApps,"GetAppInstallDir",{323190,install_path,4096}).rax!=0,"native install path");require(method(user_proxy,Kind::ISteamUser021,"GetUserDataFolder",{user_path,4096}).rax==1,"native user data path");
    require(method(p[unsigned(Kind::ISteamUtils)],Kind::ISteamUtils,"GetAppID").rax==323190,"app id");
    require(!method(p[unsigned(Kind::ISteamUtils)],Kind::ISteamUtils,"IsOverlayEnabled").rax,"offline overlay disabled");
    uint64_t monument_url=text("https://store.steampowered.com/publisher/11bitstudios?utm_source=frostpunk&utm_medium=ingame&utm_campaign=monument");
    method(p[unsigned(Kind::ISteamFriends)],Kind::ISteamFriends,"ActivateGameOverlayToWebPage",{monument_url,0});win64_free(monument_url);
    method(p[unsigned(Kind::ISteamFriends)],Kind::ISteamFriends,"ActivateGameOverlayToStore",{1125020,0});  // menu link with a numeric app id
    const uint32_t dlc_ids[]={1125020,1146960,1147010};
    const char *dlc_names[]={"Frostpunk: The Rifts","Frostpunk: The Last Autumn","Frostpunk: On The Edge"};
    require(method(apps_proxy,Kind::ISteamApps,"GetDLCCount").rax==3,"DLC count");
    uint64_t dlc_id=win64_alloc(4,1),dlc_available=win64_alloc(1,1),dlc_name=win64_alloc(64,1);
    for(unsigned i=0;i<3;++i){
        require(method(apps_proxy,Kind::ISteamApps,"BIsDlcInstalled",{dlc_ids[i]}).rax==1&&method(apps_proxy,Kind::ISteamApps,"BIsSubscribedApp",{dlc_ids[i]}).rax==1,"configured DLC ownership through Win64 bridge");
        require(method(apps_proxy,Kind::ISteamApps,"BGetDLCDataByIndex",{i,dlc_id,dlc_available,dlc_name,64}).rax==1&&*(uint32_t*)win64_pointer(dlc_id,4,0)==dlc_ids[i]&&*(bool*)win64_pointer(dlc_available,1,0)&&!strcmp(win64_string_a(dlc_name),dlc_names[i]),"configured DLC metadata through Win64 bridge");
    }
    require(!method(apps_proxy,Kind::ISteamApps,"BIsDlcInstalled",{867400}).rax&&!method(apps_proxy,Kind::ISteamApps,"BIsSubscribedApp",{867400}).rax&&!method(apps_proxy,Kind::ISteamApps,"BGetDLCDataByIndex",{3,dlc_id,dlc_available,dlc_name,64}).rax,"unknown DLC rejected through Win64 bridge");
    win64_free(dlc_id);win64_free(dlc_available);win64_free(dlc_name);
    require(!method(storage,Kind::ISteamRemoteStorage,"IsCloudEnabledForAccount").rax&&!method(storage,Kind::ISteamRemoteStorage,"IsCloudEnabledForApp").rax,"cloud disabled");
    achievement_read_cases(dso,user,stats);
    uint64_t name=text("bridge_probe.bin"),payload=win64_alloc(8,0),result=win64_alloc(8,1);const uint8_t bytes[]={0,1,0x1a,0xff,42,0,7,9};memcpy(win64_pointer(payload,8,1),bytes,8);
    require(method(storage,Kind::ISteamRemoteStorage,"FileWrite",{name,payload,8}).rax==1,"typed binary write");require(method(storage,Kind::ISteamRemoteStorage,"FileRead",{name,result,8}).rax==8&&!memcmp(win64_pointer(result,8,0),bytes,8),"typed binary read");
    if(argc==4){
        const size_t n=24u*1024u*1024u;uint64_t file=text("bridge_large_probe.bin"),input=win64_alloc(n,0),output=win64_alloc(n,1);
        auto *pattern=static_cast<unsigned char*>(win64_pointer(input,n,1));for(size_t i=0;i<n;++i)pattern[i]=(unsigned char)(i*37u+(i>>17));
        require(method(storage,Kind::ISteamRemoteStorage,"FileWrite",{file,input,n}).rax==1,"24 MiB save write within SDK file cap");
        require(method(storage,Kind::ISteamRemoteStorage,"FileRead",{file,output,n}).rax==n&&!memcmp(pattern,win64_pointer(output,n,0),n),"24 MiB save byte-exact read");
        uint64_t utils=p[unsigned(Kind::ISteamUtils)],failed=win64_alloc(1,1),call_data=win64_alloc(sizeof(RemoteStorageFileReadAsyncComplete_t),1);
        uint64_t write_call=method(storage,Kind::ISteamRemoteStorage,"FileWriteAsync",{file,input,n}).rax;require(write_call!=0,"24 MiB async write handle");
        auto fetch=[&](uint64_t handle,int size,int kind){
            // Fetch the typed result directly; polling has a separate backend handle-1 workaround.
            for(unsigned attempt=0;attempt<100;++attempt){if(method(utils,Kind::ISteamUtils,"GetAPICallResult",{handle,call_data,(uint64_t)size,(uint64_t)kind,failed}).rax==1){require(!*(bool*)win64_pointer(failed,1,0),"async callresult IO success");return;}usleep(1000);}fail("24 MiB async callresult timeout");
        };
        fetch(write_call,sizeof(RemoteStorageFileWriteAsyncComplete_t),RemoteStorageFileWriteAsyncComplete_t::k_iCallback);
        require(((RemoteStorageFileWriteAsyncComplete_t*)win64_pointer(call_data,sizeof(RemoteStorageFileWriteAsyncComplete_t),0))->m_eResult==k_EResultOK,"24 MiB async write result");
        uint64_t read_call=method(storage,Kind::ISteamRemoteStorage,"FileReadAsync",{file,0,n}).rax;require(read_call!=0,"24 MiB async read handle");
        fetch(read_call,sizeof(RemoteStorageFileReadAsyncComplete_t),RemoteStorageFileReadAsyncComplete_t::k_iCallback);
        auto *completion=(RemoteStorageFileReadAsyncComplete_t*)win64_pointer(call_data,sizeof(RemoteStorageFileReadAsyncComplete_t),0);
        require(completion->m_hFileReadAsync==read_call&&completion->m_eResult==k_EResultOK&&!completion->m_nOffset&&completion->m_cubRead==n,"24 MiB exact native async callback schema");
        memset(win64_pointer(output,n,1),0,n);require(method(storage,Kind::ISteamRemoteStorage,"FileReadAsyncComplete",{read_call,output,n}).rax==1&&!memcmp(pattern,win64_pointer(output,n,0),n),"24 MiB async byte-exact read completion");
        uint64_t stream=method(storage,Kind::ISteamRemoteStorage,"FileWriteStreamOpen",{file}).rax;require(stream!=0,"large stream handle");
        require(method(storage,Kind::ISteamRemoteStorage,"FileWriteStreamWriteChunk",{stream,input,n}).rax==1&&method(storage,Kind::ISteamRemoteStorage,"FileWriteStreamClose",{stream}).rax==1,"24 MiB stream write chunk and close");
        memset(win64_pointer(output,n,1),0,n);require(method(storage,Kind::ISteamRemoteStorage,"FileRead",{file,output,n}).rax==n&&!memcmp(pattern,win64_pointer(output,n,0),n),"24 MiB stream byte-exact reread");
        require(method(storage,Kind::ISteamRemoteStorage,"FileDelete",{file}).rax==1,"remove own large file fixture");win64_free(input);win64_free(output);win64_free(file);win64_free(failed);win64_free(call_data);
    }
    uint64_t stat=text("bridge_probe_stat"),out=win64_alloc(4,1);require(method(stats,Kind::ISteamUserStats,"SetStat",{stat,31337}).rax==1,"int32 stat setter");require(method(stats,Kind::ISteamUserStats,"GetStat",{stat,out}).rax==1&&*(int32_t*)win64_pointer(out,4,0)==31337,"typed int32 output");
    CPU float_call=cpu({stats,stat,0});float_call.xmm[2].f32[0]=3.25f;require(win64_steam_dispatch(&float_call,read(read(stats)+8*slot(Kind::ISteamUserStats,"SetStat",1)))&&float_call.rax==1,"positional XMM2 float argument");
    require(method(stats,Kind::ISteamUserStats,"GetStat",{stat,out},1).rax==1&&*(float*)win64_pointer(out,4,0)==3.25f,"typed float output");
    // Frostpunk passes achievement display names to SetAchievement, IndicateAchievementProgress and SetStat.
    const char *display_names[]={"Endless: Backup Plan","Builders: I'll Be Home for Christmass","Society: The scienitfic method, vol 2","Refugee: Iron man (Refugees)"};
    for(const char *display:display_names){uint64_t n=text(display);
        method(stats,Kind::ISteamUserStats,"SetAchievement",{n});method(stats,Kind::ISteamUserStats,"IndicateAchievementProgress",{n,1,2});
        require(method(stats,Kind::ISteamUserStats,"SetStat",{n,7}).rax==1&&method(stats,Kind::ISteamUserStats,"GetStat",{n,out}).rax==1&&*(int32_t*)win64_pointer(out,4,0)==7,"display-name stat round trip");win64_free(n);}
    uint64_t token=text("bridge probe: slot?1.bin"),bad=text("bridge<probe>.bin"),token_out=win64_alloc(8,1);
    require(method(storage,Kind::ISteamRemoteStorage,"FileWrite",{token,payload,8}).rax==1&&method(storage,Kind::ISteamRemoteStorage,"FileRead",{token,token_out,8}).rax==8&&!memcmp(win64_pointer(token_out,8,0),bytes,8)&&method(storage,Kind::ISteamRemoteStorage,"FileDelete",{token}).rax==1,"Goldberg-tokenized file name round trip");
    require(!method(storage,Kind::ISteamRemoteStorage,"FileWrite",{bad,payload,8}).rax&&method(storage,Kind::ISteamRemoteStorage,"FileWriteStreamOpen",{bad}).rax==k_UGCFileStreamHandleInvalid,"refused storage name fails only its call");
    win64_free(token);win64_free(bad);win64_free(token_out);
    uint64_t record=win64_alloc(24,1);write(record,0x20010000);require(imported("SteamInternal_ContextInit",{record}).rax==record+16&&read(record+16)==apps_proxy,"guest ContextInit callback");imported("SteamInternal_ContextInit",{record});require(context_calls==1,"context generation cached");
    uint64_t cb=callback(0,24);imported("SteamAPI_RegisterCallback",{cb,1101});require(*(int32_t*)win64_pointer(cb+12,4,0)==1101,"registration updates guest callback id");require(method(stats,Kind::ISteamUserStats,"RequestCurrentStats").rax==1,"local current stats request");
    std::thread worker,shutdown;
    if(concurrency_mode==1){
        worker=std::thread([&]{test_stack=win64_alloc(4096,1);for(unsigned i=0;i<2000&&!callback_entered;++i)usleep(1000);require(callback_entered,"worker callback start gate");require(imported("SteamAPI_GetHSteamUser").rax==user,"concurrent flat Steam call");uint64_t output=win64_alloc(4,1);require(method(stats,Kind::ISteamUserStats,"GetStat",{stat,output},1).rax==1&&*(float*)win64_pointer(output,4,0)==3.25f,"concurrent native locked stats call");win64_free(output);worker_done=true;win64_free(test_stack);});
        shutdown=std::thread([&]{test_stack=win64_alloc(4096,1);for(unsigned i=0;i<2000&&!callback_entered;++i)usleep(1000);require(callback_entered,"shutdown callback start gate");shutdown_started=true;imported("SteamAPI_Shutdown");shutdown_finished=true;win64_free(test_stack);});
    }
    for(unsigned i=0;i<40&&!callback_calls;++i){imported("SteamAPI_RunCallbacks");usleep(10000);}require(callback_calls>0,"native event -> static guest callback");
    if(concurrency_mode==1){worker.join();shutdown.join();require(shutdown_finished,"shutdown completes after callbacks");printf("{\"pass\":true,\"callback_waits_worker_service_call\":true,\"shutdown_waits_active_callback\":true,\"worker_flat_and_native_locked_stat_calls\":true,\"native_threads\":3,\"game_boot_proven\":false}\n");return 0;}
    imported("SteamAPI_UnregisterCallback",{cb});
    expected_async=method(storage,Kind::ISteamRemoteStorage,"FileReadAsync",{name,0,8}).rax;require(expected_async!=0,"native async operation");uint64_t cr=callback(1332,24);imported("SteamAPI_RegisterCallResult",{cr,expected_async});for(unsigned i=0;i<40&&!result_calls;++i){imported("SteamAPI_RunCallbacks");usleep(10000);}require(result_calls==1,"native async -> static guest callresult");require(method(storage,Kind::ISteamRemoteStorage,"FileReadAsyncComplete",{expected_async,result,8}).rax==1&&!memcmp(win64_pointer(result,8,0),bytes,8),"async binary completion");imported("SteamAPI_UnregisterCallResult",{cr,expected_async});
    imported("SteamAPI_ReleaseCurrentThreadMemory");auto socket_count=reinterpret_cast<unsigned long long(*)()>(dlsym(dso,"FrostpunkOffline_DeniedSocketCalls"));require(socket_count&&socket_count()==0,"no network sockets attempted");imported("SteamAPI_Shutdown");
    printf("{\"pass\":true,\"interfaces\":7,\"canonical_proxies\":true,\"Win64_hidden_class_return\":true,\"float_XMM2\":true,\"context_callbacks\":%u,\"event_callbacks\":%u,\"callresults\":%u,\"bridge_calls\":%u,\"GetAchievement_native_pairs\":%u,\"GetAchievement_known_unknown_empty_colon_separators\":true,\"GetAchievement_fullCPU_onebyte_guards_errno\":true,\"binary_bytes\":8,\"large_binary_bytes\":25165824,\"large_async_bytes\":25165824,\"large_stream_bytes\":25165824,\"offline\":true,\"cloud\":false,\"socket_attempts\":0,\"static_fixture_callbacks\":true,\"native_install_path\":\"%s\",\"native_user_data_path\":\"%s\",\"game_boot_proven\":false}\n",context_calls,callback_calls,result_calls,invocations.load(),achievement_cases,win64_string_a(install_path),win64_string_a(user_path));return 0;
}
