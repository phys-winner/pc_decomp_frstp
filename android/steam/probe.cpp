/* Native-only backend fixture. No guest CPU callbacks, account identifiers,
 * tokens, real Steam paths, game launch or external network connection. */
#include "steam_api.h"
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <string>
#include <unistd.h>
static_assert(sizeof(UserStatsReceived_t)==24,"Win64 packed callback ABI");
static_assert(offsetof(UserStatsReceived_t,m_steamIDUser)==12,"Win64 SDK packed CSteamID offset");
static_assert(sizeof(UserStatsStored_t)==16,"Win64 stored callback ABI");
static void require(bool value,const char *what){if(!value){fprintf(stderr,"FAIL offline-services %s\n",what);_exit(1);}}
template<class T>static T symbol(void *library,const char *name){void *p=dlsym(library,name);require(p!=nullptr,name);return reinterpret_cast<T>(p);}
struct StatsCallback:CCallbackBase {
    unsigned calls=0;int bytes;explicit StatsCallback(int size):bytes(size){}
    void Run(void *data)override{uint64 app;memcpy(&app,data,8);require(app==323190,"native callback app identity");++calls;}
    void Run(void *data,bool failed,SteamAPICall_t)override{require(!failed,"native callback IO failure");Run(data);}
    int GetCallbackSizeBytes()override{return bytes;}
};
int main(int argc,char **argv) {
    require(argc==5,"usage <DSO> <existing-profile-root> <game-root> <write|read>");
    bool writing=!strcmp(argv[4],"write");require(writing||!strcmp(argv[4],"read"),"mode");
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!library)fprintf(stderr,"dlopen: %s\n",dlerror());require(library!=nullptr,"dlopen Android Bionic DSO");
    const char *required[]={"SteamInternal_CreateInterface","SteamAPI_RunCallbacks","SteamInternal_FindOrCreateUserInterface","SteamAPI_Init","SteamAPI_Shutdown","SteamAPI_RestartAppIfNecessary","SteamInternal_ContextInit","SteamAPI_RegisterCallResult","SteamAPI_UnregisterCallResult","SteamAPI_ReleaseCurrentThreadMemory","SteamAPI_GetHSteamUser"};
    for(const char *name:required)symbol<void *>(library,name);
    require(symbol<int(*)(const char*,const char*)>(library,"FrostpunkOffline_Configure")(argv[2],argv[3]),"isolated local configuration");
    require(symbol<unsigned(*)()>(library,"FrostpunkOffline_Policy")()==7,"offline network/cloud policy");
    auto attempts=symbol<unsigned long long(*)()>(library,"FrostpunkOffline_DeniedSocketCalls");
    require(symbol<bool(*)()>(library,"SteamAPI_Init")(),"native service initialize");
    auto factory=symbol<void*(*)(HSteamUser,const char*)>(library,"SteamInternal_FindOrCreateUserInterface");
    HSteamUser user=symbol<HSteamUser(*)()>(library,"SteamAPI_GetHSteamUser")();
    const char *versions[]={"STEAMAPPS_INTERFACE_VERSION008","SteamClient020","STEAMREMOTESTORAGE_INTERFACE_VERSION016","SteamUser021","STEAMUSERSTATS_INTERFACE_VERSION012","SteamUtils010","SteamFriends017"};
    void *interfaces[7];for(unsigned i=0;i<7;++i){interfaces[i]=i==1?symbol<void*(*)(const char*)>(library,"SteamInternal_CreateInterface")(versions[i]):factory(user,versions[i]);require(interfaces[i]!=nullptr,versions[i]);}
    auto apps=static_cast<ISteamApps*>(interfaces[0]);auto storage=static_cast<ISteamRemoteStorage*>(interfaces[2]);
    const char *expected_language=getenv("FROSTPUNK_EXPECTED_LANGUAGE");
    require(!strcmp(apps->GetCurrentGameLanguage(),expected_language?expected_language:"english"),"system language and English fallback");
    auto stats=static_cast<ISteamUserStats*>(interfaces[4]);auto utils=static_cast<ISteamUtils*>(interfaces[5]);
    char *expected_game=realpath(argv[3],nullptr);require(expected_game!=nullptr,"configured game directory exists");
    char installed_game[4096]={0};uint32 installed_length=apps->GetAppInstallDir(323190,installed_game,sizeof(installed_game));
    require(installed_length==strlen(expected_game)&&!strcmp(installed_game,expected_game),"GetAppInstallDir equals configured canonical game directory independently of cwd");free(expected_game);
    auto validate=symbol<int(*)(const char*,int)>(library,"FrostpunkOffline_ValidateStorageName");
    require(validate("nested/fixture.bin",0)&&validate("frostpunk_probe_stat",1),"safe storage name preflight");
    require(!validate("../escaped.bin",0)&&!validate("dir/../escaped.bin",0)&&!validate("/escaped.bin",0)&&!validate("C:escaped.bin",0),"invalid storage paths rejected");
    require(utils->GetAppID()==323190,"app id");require(!static_cast<ISteamUser*>(interfaces[3])->BLoggedOn(),"offline status is honest");
    const AppId_t dlc_ids[]={1125020,1146960,1147010};
    const char *dlc_names[]={"Frostpunk: The Rifts","Frostpunk: The Last Autumn","Frostpunk: On The Edge"};
    require(apps->GetDLCCount()==3,"configured DLC count");
    for(int i=0;i<3;++i){AppId_t id=0;bool available=false;char name[64]={};
        require(apps->BIsDlcInstalled(dlc_ids[i])&&apps->BIsSubscribedApp(dlc_ids[i]),"configured DLC installed and subscribed");
        require(apps->BGetDLCDataByIndex(i,&id,&available,name,sizeof(name))&&id==dlc_ids[i]&&available&&!strcmp(name,dlc_names[i]),"configured DLC metadata");}
    require(!apps->BIsDlcInstalled(867400)&&!apps->BIsSubscribedApp(867400)&&!apps->BGetDLCDataByIndex(3,nullptr,nullptr,nullptr,0),"unknown DLC rejected");
    require(!storage->IsCloudEnabledForAccount()&&!storage->IsCloudEnabledForApp(),"cloud disabled");
    const unsigned char payload[]={0,1,0x1a,0xff,42,0,7,9};unsigned char readback[sizeof(payload)]={0};
    if(writing){require(storage->FileWrite("frostpunk_probe.bin",payload,sizeof(payload))&&storage->FileWrite("nested/fixture.bin",payload,sizeof(payload)),"local binary write");require(stats->SetStat("frostpunk_probe_stat",int32(31337))&&stats->StoreStats(),"local stats write");}
    require(storage->FileRead("frostpunk_probe.bin",readback,sizeof(readback))==sizeof(readback)&&!memcmp(readback,payload,sizeof(payload)),"local binary read");
    require(storage->FileRead("nested/fixture.bin",readback,sizeof(readback))==sizeof(readback)&&!memcmp(readback,payload,sizeof(payload)),"nested binary read");
    std::string remote=std::string(argv[2])+"/data/323190/remote";int remote_fd=open(remote.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    require(remote_fd>=0&&!symlinkat("../stats",remote_fd,"probe-symlink"),"create own symlink boundary fixture");
    require(!validate("probe-symlink/escaped.bin",0)&&!validate("probe-symlink",0),"existing symlink storage paths rejected");
    require(!unlinkat(remote_fd,"probe-symlink",0),"remove own symlink fixture");close(remote_fd);
    int32 value=0;require(stats->GetStat("frostpunk_probe_stat",&value)&&value==31337,"local stats read");
    auto reg=symbol<void(*)(CCallbackBase*,int)>(library,"SteamAPI_RegisterCallback");auto unreg=symbol<void(*)(CCallbackBase*)>(library,"SteamAPI_UnregisterCallback");
    auto callbacks=symbol<void(*)()>(library,"SteamAPI_RunCallbacks");StatsCallback received(sizeof(UserStatsReceived_t));
    reg(&received,UserStatsReceived_t::k_iCallback);require(stats->RequestCurrentStats(),"request local stats callback");
    for(unsigned i=0;i<40&&!received.calls;++i){callbacks();usleep(10000);}
    require(received.calls!=0,"RunCallbacks delivers a native callback");unreg(&received);
    require(attempts()==0,"backend created no sockets");require(symbol<int(*)()>(library,"FrostpunkOffline_TestNetworkDenied")()&&attempts()==1,"network hard-deny negative test");
    symbol<void(*)()>(library,"SteamAPI_Shutdown")();
    printf("{\"pass\":true,\"native_aarch64\":true,\"mode\":\"%s\",\"required_symbols\":11,\"interfaces\":7,\"binary_bytes\":8,\"stat_value\":31337,\"native_callbacks\":%u,\"callback_size\":24,\"callback_id_offset\":12,\"DLC_count\":3,\"offline\":true,\"cloud\":false,\"backend_socket_attempts\":0,\"network_negative_denied\":true,\"storage_path_preflight\":true,\"existing_symlink_denied\":true,\"nested_file_persistence\":true,\"canonical_install_path\":\"%s\",\"atomic_storage_IO\":false,\"wine_box64\":false,\"game_boot_proven\":false}\n",writing?"write":"read",received.calls,installed_game);
    return 0;
}
