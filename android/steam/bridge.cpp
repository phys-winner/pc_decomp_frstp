/* Typed Win64 -> native ARM64 local-service boundary. Guest code is dispatched
 * statically; a guest function pointer is never called as a native pointer. */
#define STEAM_API_NODLL
#include "steam_api_common.h"
#include "isteamclient.h"
#include "isteamuser021.h"
#include "isteamapps.h"
#include "isteamutils.h"
#include "isteamfriends.h"
#include "isteamuserstats.h"
#include "isteamremotestorage.h"
#include "bridge.h"
#include "win64.h"
#include "steam_metadata.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {
constexpr uint64_t BASE=0xe6000000;
constexpr unsigned STRIDE=256;
std::mutex metadata_mutex;
std::mutex lifecycle_mutex;
std::condition_variable lifecycle_changed;
unsigned active_calls;
bool lifecycle_exclusive;
thread_local unsigned service_depth;
void reap_callbacks();
struct Call {
    Call(){std::unique_lock lock(lifecycle_mutex);lifecycle_changed.wait(lock,[]{return !lifecycle_exclusive;});++active_calls;++service_depth;}
    ~Call(){{std::lock_guard lock(lifecycle_mutex);--active_calls;--service_depth;}lifecycle_changed.notify_all();reap_callbacks();}
};
struct Exclusive {
    Exclusive();
    ~Exclusive(){{std::lock_guard lock(lifecycle_mutex);lifecycle_exclusive=false;}lifecycle_changed.notify_all();}
};
struct Proxy {uint64_t guest;void *native;Kind kind;uint64_t epoch;};
std::unordered_map<uint64_t,Proxy> proxies;
std::array<uint64_t,unsigned(Kind::Count)> tables{};
std::unordered_map<std::string,uint64_t> strings;
std::atomic<uint64_t> epoch{1},context_generation{0};
std::atomic<bool> initialized{false};
std::atomic<uint64_t> warning_hook{0};
thread_local CPU *active_cpu;
struct Active {CPU *old;explicit Active(CPU*c):old(active_cpu){active_cpu=c;}~Active(){active_cpu=old;}};
struct API {
    void *handle=nullptr;
    unsigned (*policy)()=nullptr;
    int (*validate_name)(const char*,int)=nullptr;
    /* Goldberg's definitions deliberately return unsigned steam_bool, unlike
     * the public SDK bool declarations. Preserve the actual native ABI. */
    steam_bool (*init)()=nullptr;
    void (*shutdown)()=nullptr;
    steam_bool (*restart)(uint32)=nullptr;
    void (*run)()=nullptr;
    int32 (*user)()=nullptr;
    void* (*create)(const char*)=nullptr;
    void* (*find)(int32,const char*)=nullptr;
    void (*release)()=nullptr;
    void (*register_result)(CCallbackBase*,SteamAPICall_t)=nullptr;
    void (*unregister_result)(CCallbackBase*,SteamAPICall_t)=nullptr;
    void (*register_callback)(CCallbackBase*,int)=nullptr;
    void (*unregister_callback)(CCallbackBase*)=nullptr;
} api;
[[noreturn]] void trap(const char *reason,Kind kind=Kind::Count,unsigned slot=0){
    if(kind==Kind::Count)fprintf(stderr,"UNSUPPORTED_LOCAL_STEAM: %s\n",reason);
    else fprintf(stderr,"UNSUPPORTED_LOCAL_STEAM: %s interface=%s slot=%u method=%s\n",reason,versions[unsigned(kind)],slot,slot<slot_counts[unsigned(kind)]?slot_names[unsigned(kind)][slot]:"out_of_range");
    abort();
}
Exclusive::Exclusive(){
    if(service_depth)trap("Init/Shutdown inside an active native call or guest callback is unsupported");
    std::unique_lock lock(lifecycle_mutex);
    /* Admit worker calls while a shutdown waits for callbacks to complete. A
     * callback may be waiting for precisely that worker. Only destruction,
     * after every active call returned, excludes new calls. */
    lifecycle_changed.wait(lock,[]{return !lifecycle_exclusive&&!active_calls;});lifecycle_exclusive=true;
}
uint64_t arg(CPU*c,unsigned i){return win64_argument(c,i);}
uint64_t rd(uint64_t p){uint64_t v;memcpy(&v,win64_pointer(p,8,0),8);return v;}
void wr(uint64_t p,uint64_t v){memcpy(win64_pointer(p,8,1),&v,8);}
size_t count(uint64_t n){if(n>16u*1024u*1024u)trap("negative/oversized service buffer count");return(size_t)n;}
size_t count_u(CPU*c,unsigned i){return count((uint32_t)arg(c,i));}
size_t count_s(CPU*c,unsigned i){int32_t n=(int32_t)arg(c,i);if(n<0)trap("negative service buffer count");return count((uint32_t)n);}
// Save files may exceed the generic service limit; retain the SDK's file cap.
size_t count_file(uint32_t n){if(n>k_unMaxCloudFileChunkSize)trap("oversized file buffer count");return(size_t)n;}
size_t count_file_u(CPU*c,unsigned i){return count_file((uint32_t)arg(c,i));}
size_t count_file_s(CPU*c,unsigned i){int32_t n=(int32_t)arg(c,i);if(n<0)trap("negative service buffer count");return count_file((uint32_t)n);}
size_t product(size_t a,size_t b){if(b&&a>SIZE_MAX/b)trap("buffer multiplication overflow");return a*b;}
void name_log(const char *tag,int is_stat,const char *s){
    fprintf(stderr,"%s is_stat=%d guest_return=0x%llx name_prefix=\"%.256s\"\n",tag,is_stat,(unsigned long long)(active_cpu?rd(active_cpu->rsp):0),s);fflush(stderr);
}
/* A stat or remote-storage name that the backend turns into a file of its profile. Goldberg's
 * Local_Storage::sanitize_file_name replaces : | * " ? with tokens, so these characters are plain
 * name text: the game passes achievement display names such as "Endless: Backup Plan" to SetStat.
 * An absolute or traversing name stays fatal (the game's own SteamGamerProfile rejects both before
 * calling); a name the storage preflight refuses fails only this call and touches no file. */
bool storage_name(uint64_t address,int is_stat){const char *s=win64_string_a(address);if(!*s||*s=='/'||*s=='\\'){
    name_log("STEAM_NAME_REJECT",is_stat,s);
    trap("service filename/stat name must be relative");
}const char *component=s;for(const char *p=s;;++p)if(!*p||*p=='/'||*p=='\\'){size_t n=p-component;if(!n||(n==1&&component[0]=='.')||(n==2&&component[0]=='.'&&component[1]=='.')){name_log("STEAM_NAME_REJECT",is_stat,s);trap("service path traversal rejected");}if(!*p)break;component=p+1;}
    if(api.validate_name(s,is_stat))return true;
    static std::atomic<unsigned> refused{0};if(refused.fetch_add(1)<32)name_log("STEAM_NAME_REFUSED",is_stat,s);
    return false;
}
Kind version(const char *s){for(unsigned i=0;i<unsigned(Kind::Count);++i)if(!strcmp(s,versions[i]))return Kind(i);trap("unrepresented requested interface version");}
uint64_t wrap(void *native,Kind kind){
    if(!native)return 0;std::lock_guard lock(metadata_mutex);uint64_t e=epoch.load();
    for(const auto &p:proxies)if(p.second.native==native&&p.second.kind==kind&&p.second.epoch==e)return p.first;
    unsigned k=unsigned(kind);if(!tables[k]){tables[k]=win64_alloc(slot_counts[k]*8,1);for(unsigned i=0;i<slot_counts[k];++i)wr(tables[k]+8*i,BASE+(k*STRIDE+i)*8);}
    uint64_t guest=win64_alloc(8,1);wr(guest,tables[k]);proxies.emplace(guest,Proxy{guest,native,kind,e});return guest;
}
Proxy object(uint64_t p){std::lock_guard lock(metadata_mutex);auto it=proxies.find(p);if(it==proxies.end()||it->second.epoch!=epoch.load())trap("invalid/stale interface proxy");win64_pointer(p,8,0);return it->second;}
uint64_t guest_string(const char *text){
    if(!text)return 0;size_t n=strnlen(text,32768);if(n==32768)trap("native service string exceeds bound");
    std::lock_guard lock(metadata_mutex);auto it=strings.find(text);if(it!=strings.end())return it->second;
    uint64_t p=win64_alloc(n+1,0);memcpy(win64_pointer(p,n+1,1),text,n+1);strings.emplace(text,p);return p;
}
uint64_t guest_call(CPU*c,uint64_t target,std::initializer_list<uint64_t> args){
    if(!target||args.size()>4||c->rsp<80)trap("guest callback contract");CPU nested=*c;
    uint64_t frame=(c->rsp-64)&~UINT64_C(15);win64_pointer(frame-8,40,1);nested.rsp=frame-8;wr(nested.rsp,UINT64_C(0xe600f000));
    uint64_t *r[]={&nested.rcx,&nested.rdx,&nested.r8,&nested.r9};unsigned i=0;for(uint64_t a:args)*r[i++]=a;
    Active active(&nested);dispatch(&nested,target);if(nested.rsp!=frame)trap("guest callback stack imbalance");return nested.rax;
}
using Counts=std::array<size_t,16>;
Counts counts(std::initializer_list<std::pair<unsigned,size_t>> changed={}){Counts n;n.fill(1);for(auto pair:changed)n.at(pair.first)=pair.second;return n;}
template<class T> constexpr bool class_id=std::is_same_v<T,CSteamID>||std::is_same_v<T,CGameID>;
template<class A> class Argument {
    using T=std::remove_cv_t<A>;using P=std::remove_pointer_t<T>;
    A value{};
public:
    Argument(CPU*c,unsigned index,size_t n){uint64_t bits=arg(c,index);
        if constexpr(std::is_same_v<T,CSteamID>)value=CSteamID(uint64(bits));
        else if constexpr(std::is_same_v<T,CGameID>)value=CGameID(uint64(rd(bits)));
        else if constexpr(std::is_same_v<T,const char*>)value=bits?win64_string_a(bits):nullptr;
        else if constexpr(std::is_pointer_v<T>){
            if constexpr(std::is_function_v<P>||std::is_pointer_v<std::remove_cv_t<P>>)trap("unrepresented function/double-pointer argument");
            else if constexpr(std::is_void_v<std::remove_cv_t<P>>)value=bits?win64_pointer(bits,n,!std::is_const_v<P>):nullptr;
            else value=bits?static_cast<A>(win64_pointer(bits,product(sizeof(P),n),!std::is_const_v<P>)):nullptr;
            if(!bits&&n)trap("null required service output/buffer");
        }else if constexpr(std::is_same_v<T,float>){uint32_t b=index<4?c->xmm[index].u32[0]:(uint32_t)bits;memcpy(&value,&b,4);}
        else if constexpr(std::is_same_v<T,double>){uint64_t b=index<4?c->xmm[index].u64[0]:bits;memcpy(&value,&b,8);}
        else value=static_cast<A>(bits);
    }
    A get(){return value;}
};
template<class T,class R,class...A,size_t...I>void invoke_impl(CPU*c,T*p,R(T::*method)(A...),Counts n,std::index_sequence<I...>){
    constexpr unsigned hidden=class_id<R>?1:0;uint64_t result_buffer=0;if constexpr(hidden){result_buffer=arg(c,1);win64_pointer(result_buffer,8,1);}
    std::tuple<Argument<A>...> a{Argument<A>(c,I+1+hidden,n[I+1])...};
    if constexpr(std::is_void_v<R>)(p->*method)(std::get<I>(a).get()...);
    else{R r=(p->*method)(std::get<I>(a).get()...);
        if constexpr(class_id<R>){if constexpr(std::is_same_v<R,CSteamID>)wr(result_buffer,r.ConvertToUint64());else wr(result_buffer,r.ToUint64());c->rax=result_buffer;}
        else if constexpr(std::is_same_v<R,const char*>)c->rax=guest_string(r);
        else if constexpr(std::is_pointer_v<R>)trap("unrepresented native pointer return");
        else if constexpr(std::is_same_v<R,float>)c->xmm[0].f32[0]=r;
        else if constexpr(std::is_same_v<R,double>)c->xmm[0].f64[0]=r;
        else if constexpr(sizeof(R)<=4)c->rax=uint32_t(r);else c->rax=uint64_t(r);
    }
}
template<class T,class R,class...A>void invoke(CPU*c,R(T::*method)(A...),Counts n=counts()){Proxy p=object(arg(c,0));invoke_impl(c,static_cast<T*>(p.native),method,n,std::index_sequence_for<A...>{});}
size_t callback_size(int id){switch(id){
#define CB(T) case T::k_iCallback:return sizeof(T)
    CB(UserStatsReceived_t);CB(UserStatsStored_t);CB(UserAchievementStored_t);CB(LeaderboardFindResult_t);CB(LeaderboardScoresDownloaded_t);CB(LeaderboardScoreUploaded_t);CB(NumberOfCurrentPlayers_t);CB(UserStatsUnloaded_t);CB(UserAchievementIconFetched_t);CB(GlobalAchievementPercentagesReady_t);CB(LeaderboardUGCSet_t);CB(GlobalStatsReceived_t);CB(RemoteStorageFileWriteAsyncComplete_t);CB(RemoteStorageFileReadAsyncComplete_t);CB(DlcInstalled_t);CB(SetPersonaNameResponse_t);
#undef CB
    default:trap("unrepresented callback payload schema");}}
struct Callback;
std::unordered_map<uint64_t,std::shared_ptr<Callback>> callbacks;
unsigned callback_runs;
struct Callback final:CCallbackBase,std::enable_shared_from_this<Callback>{
    uint64_t guest;size_t size;std::mutex operation_mutex;bool broadcast=false;std::set<SteamAPICall_t> calls;
    Callback(uint64_t p,int id,size_t n):guest(p),size(n){m_iCallback=id;}
    void deliver(void*payload,bool io,SteamAPICall_t call,bool result){
        auto hold=shared_from_this();if(!active_cpu)trap("native callback outside guest RunCallbacks");
        if(result){std::lock_guard lock(metadata_mutex);calls.erase(call);}
        if(!payload)trap("null native callback payload");uint64_t bytes=win64_alloc(size,0);memcpy(win64_pointer(bytes,size,1),payload,size);
        uint64_t table=rd(guest),target=rd(table+(result?8:0));
        if(result)guest_call(active_cpu,target,{guest,bytes,io?1u:0u,call});else guest_call(active_cpu,target,{guest,bytes});
        win64_free(bytes);
    }
    void Run(void*p)override{deliver(p,false,0,false);}
    void Run(void*p,bool io,SteamAPICall_t call)override{deliver(p,io,call,true);}
    int GetCallbackSizeBytes()override{return(int)size;}
};
void reap_callbacks(){
    std::lock_guard lock(metadata_mutex);if(callback_runs)return;
    for(auto it=callbacks.begin();it!=callbacks.end();) {
        const auto &cb=it->second;
        if(!cb->broadcast&&cb->calls.empty()&&cb.use_count()==1)it=callbacks.erase(it);else ++it;
    }
}
struct RunningCallbacks {
    RunningCallbacks(){std::lock_guard lock(metadata_mutex);++callback_runs;}
    ~RunningCallbacks(){std::lock_guard lock(metadata_mutex);--callback_runs;}
};
std::shared_ptr<Callback> callback(CPU*c,uint64_t guest,int id=-1){
    win64_pointer(guest,16,1);int actual;memcpy(&actual,win64_pointer(guest+12,4,0),4);if(id<0)id=actual;
    if(*(uint8_t*)win64_pointer(guest+8,1,0)&2)trap("gameserver callback in local client backend");
    size_t size=callback_size(id);uint64_t table=rd(guest);int supplied=(int32_t)guest_call(c,rd(table+16),{guest});if(supplied!=(int)size)trap("guest callback payload size differs from pinned SDK schema");
    std::lock_guard lock(metadata_mutex);auto it=callbacks.find(guest);if(it!=callbacks.end()){if(it->second->GetICallback()!=id||it->second->size!=size)trap("guest callback changed schema while registered");return it->second;}
    auto cb=std::make_shared<Callback>(guest,id,size);callbacks.emplace(guest,cb);return cb;
}
std::mutex warning_mutex;std::vector<std::pair<int,std::string>> warnings;
void native_warning(int severity,const char*text){size_t n=text?strnlen(text,32768):0;if(n==32768)trap("warning message length");std::lock_guard lock(warning_mutex);if(warnings.size()>=256)trap("warning queue budget");warnings.emplace_back(severity,text?text:"");}
void flush_warnings(CPU*c){std::vector<std::pair<int,std::string>> pending;{std::lock_guard lock(warning_mutex);pending.swap(warnings);}uint64_t target=warning_hook.load();if(!target)return;for(auto &w:pending)guest_call(c,target,{uint32_t(w.first),guest_string(w.second.c_str())});}
void set_warning(CPU*c,Kind kind){uint64_t hook=arg(c,1);warning_hook=hook;auto p=object(arg(c,0));
    if(kind==Kind::ISteamClient)static_cast<ISteamClient*>(p.native)->SetWarningMessageHook(hook?native_warning:nullptr);
    else static_cast<ISteamUtils*>(p.native)->SetWarningMessageHook(hook?native_warning:nullptr);
}
void client_get(CPU*c,Kind expected,unsigned slot){
    auto *p=static_cast<ISteamClient*>(object(arg(c,0)).native);int user=(int32_t)arg(c,1),pipe=(int32_t)arg(c,2);const char *v=win64_string_a(arg(c,3));
    if(expected==Kind::ISteamUtils){pipe=user;user=api.user();v=win64_string_a(arg(c,2));}
    Kind actual=version(v);if(expected!=Kind::Count&&actual!=expected)trap("client getter requested mismatched SDK version",Kind::ISteamClient,slot);
    c->rax=wrap(p->GetISteamGenericInterface(user,pipe,v),actual);
}
void api_result(CPU*c){int expected=(int32_t)arg(c,4);size_t n=callback_size(expected);if(arg(c,3)!=n)trap("GetAPICallResult size differs from known callback schema");invoke(c,&ISteamUtils::GetAPICallResult,counts({{2,n}}));}
template<class T>void symbol(T&fn,const char*name){fn=reinterpret_cast<T>(dlsym(api.handle,name));if(!fn)trap("required local backend export missing");}
}
extern "C" int win64_steam_configure(void *handle){
    std::lock_guard lock(metadata_mutex);if(api.handle)trap("local Steam backend configured twice");if(!handle)return 0;api.handle=handle;
    symbol(api.policy,"FrostpunkOffline_Policy");if(api.policy()!=7)trap("backend must explicitly enforce local-only/no-network/no-cloud policy");
    symbol(api.validate_name,"FrostpunkOffline_ValidateStorageName");
    symbol(api.init,"SteamAPI_Init");symbol(api.shutdown,"SteamAPI_Shutdown");symbol(api.restart,"SteamAPI_RestartAppIfNecessary");symbol(api.run,"SteamAPI_RunCallbacks");symbol(api.user,"SteamAPI_GetHSteamUser");symbol(api.create,"SteamInternal_CreateInterface");symbol(api.find,"SteamInternal_FindOrCreateUserInterface");symbol(api.release,"SteamAPI_ReleaseCurrentThreadMemory");symbol(api.register_result,"SteamAPI_RegisterCallResult");symbol(api.unregister_result,"SteamAPI_UnregisterCallResult");symbol(api.register_callback,"SteamAPI_RegisterCallback");symbol(api.unregister_callback,"SteamAPI_UnregisterCallback");return 1;
}
extern "C" int win64_steam_import(CPU*c,const char*dll,const char*name){
    if(strcasecmp(dll,"steam_api64.dll")&&strcasecmp(dll,"steam_api.dll"))return 0;if(!api.handle)trap("local Steam backend not configured");Active active(c);
    if(!strcmp(name,"SteamAPI_Init")){Exclusive lifecycle;unsigned ok=api.init();if(ok&&!initialized.exchange(true))++context_generation;c->rax=ok?1:0;return 1;}
    if(!strcmp(name,"SteamAPI_Shutdown")){Exclusive lifecycle;if(initialized.exchange(false)){api.shutdown();++epoch;++context_generation;std::lock_guard lock(metadata_mutex);callbacks.clear();}c->rax=0;return 1;}
    Call call;
    if(!strcmp(name,"SteamAPI_RestartAppIfNecessary"))c->rax=api.restart((uint32)arg(c,0))?1:0;
    else if(!strcmp(name,"SteamAPI_GetHSteamUser"))c->rax=uint32_t(api.user());
    else if(!strcmp(name,"SteamInternal_CreateInterface")){const char*v=win64_string_a(arg(c,0));Kind k=version(v);c->rax=wrap(api.create(v),k);}
    else if(!strcmp(name,"SteamInternal_FindOrCreateUserInterface")){const char*v=win64_string_a(arg(c,1));Kind k=version(v);c->rax=wrap(api.find((int32)arg(c,0),v),k);}
    else if(!strcmp(name,"SteamInternal_ContextInit")){uint64_t p=arg(c,0);win64_pointer(p,24,1);uint64_t generation=context_generation.load();if(rd(p+8)!=generation){guest_call(c,rd(p),{p+16});wr(p+8,generation);}c->rax=p+16;}
    else if(!strcmp(name,"SteamAPI_RunCallbacks")){RunningCallbacks running;api.run();flush_warnings(c);c->rax=0;}
    else if(!strcmp(name,"SteamAPI_RegisterCallResult")){uint64_t call=arg(c,1);if(call){auto cb=callback(c,arg(c,0));std::lock_guard operation(cb->operation_mutex);{std::lock_guard lock(metadata_mutex);cb->calls.insert(call);}api.register_result(cb.get(),call);}c->rax=0;}
    else if(!strcmp(name,"SteamAPI_UnregisterCallResult")){uint64_t call=arg(c,1);std::shared_ptr<Callback> cb;{std::lock_guard lock(metadata_mutex);auto it=callbacks.find(arg(c,0));if(it!=callbacks.end())cb=it->second;}if(call&&cb){std::lock_guard operation(cb->operation_mutex);api.unregister_result(cb.get(),call);std::lock_guard lock(metadata_mutex);cb->calls.erase(call);}c->rax=0;}
    else if(!strcmp(name,"SteamAPI_RegisterCallback")){auto cb=callback(c,arg(c,0),(int32_t)arg(c,1));std::lock_guard operation(cb->operation_mutex);{std::lock_guard lock(metadata_mutex);cb->broadcast=true;}api.register_callback(cb.get(),cb->GetICallback());int id=cb->GetICallback();memcpy(win64_pointer(arg(c,0)+12,4,1),&id,4);*(uint8_t*)win64_pointer(arg(c,0)+8,1,1)|=1;c->rax=0;}
    else if(!strcmp(name,"SteamAPI_UnregisterCallback")){std::shared_ptr<Callback> cb;{std::lock_guard lock(metadata_mutex);auto it=callbacks.find(arg(c,0));if(it!=callbacks.end())cb=it->second;}if(cb){std::lock_guard operation(cb->operation_mutex);api.unregister_callback(cb.get());std::lock_guard lock(metadata_mutex);cb->broadcast=false;}*(uint8_t*)win64_pointer(arg(c,0)+8,1,1)&=~1u;c->rax=0;}
    else if(!strcmp(name,"SteamAPI_ReleaseCurrentThreadMemory")){api.release();c->rax=0;}
    else return 0;return 1;
}
extern "C" int win64_steam_dispatch(CPU*c,uint64_t target){
    if(target<BASE||target-BASE>=unsigned(Kind::Count)*STRIDE*8)return 0;if((target-BASE)%8)trap("unaligned service sentinel");
    Call call;
    unsigned index=unsigned((target-BASE)/8),slot=index%STRIDE;Kind kind=Kind(index/STRIDE);Proxy p=object(arg(c,0));if(p.kind!=kind||slot>=slot_counts[unsigned(kind)])trap("proxy/vtable kind or slot mismatch",kind,slot);Active active(c);
    switch(kind){
#include "steam_dispatch.inc"
    default:break;
    }trap("unimplemented service method",kind,slot);
}
