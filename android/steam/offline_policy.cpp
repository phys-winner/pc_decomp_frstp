/* Native Android configuration for an explicitly selected offline local
 * service backend. It never locates a real Steam installation or account.
 * The profile directory must already exist and be private to this backend. */
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dlc_config.h"
static std::atomic<unsigned long long> denied_sockets{0};
static std::mutex profile_mutex;
static int profile_fd=-1;
extern "C" int __wrap_socket(int,int,int){++denied_sockets;errno=EPERM;return -1;}
static bool directory_at(int root,const char *name) {
    if(mkdirat(root,name,0700)&&errno!=EEXIST)return false;
    int fd=openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return false;close(fd);return true;
}
static bool config_at(int root,const char *name,const std::string &value) {
    int fd=openat(root,name,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW|O_CLOEXEC,0600);if(fd<0)return false;
    size_t at=0;while(at<value.size()){ssize_t n=write(fd,value.data()+at,value.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);return false;}at+=(size_t)n;}
    int status=fsync(fd);int closed=close(fd);return !status&&!closed;
}
static const char *system_language() {
    const char *tag=getenv("FROSTPUNK_SYSTEM_LANGUAGE");
    if(!tag||strnlen(tag,64)<2||strnlen(tag,64)==64||tag[2]&&tag[2]!='-'&&tag[2]!='_')return "english";
    char code[3]={tag[0],tag[1],0};for(char &c:code)if(c>='A'&&c<='Z')c+=32;
    static const struct { const char *code,*steam; } languages[]={
        {"en","english"},{"ru","russian"},{"de","german"},{"pl","polish"},
        {"fr","french"},{"es","spanish"},{"it","italian"},{"pt","brazilian"},
        {"tr","turkish"},{"ja","japanese"},{"ko","koreana"},{"zh","schinese"}};
    for(const auto &language:languages)if(!strcmp(code,language.code))return language.steam;
    return "english";
}
extern "C" int FrostpunkOffline_Configure(const char *profile,const char *game) {
    if(!profile||profile[0]!='/'||!game||game[0]!='/'||strchr(profile,'\n')||strchr(game,'\n')||strchr(game,'\r'))return 0;
    char *resolved=realpath(profile,nullptr);if(!resolved)return 0;std::string path(resolved);free(resolved);
    int root=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(root<0)return 0;
    bool ok=directory_at(root,"data")&&directory_at(root,"steam_settings")&&config_at(root,"local_save.txt","data\n");
    int settings=ok?openat(root,"steam_settings",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;
    if(settings<0)ok=false;
    if(ok)ok=config_at(settings,"offline.txt","")&&config_at(settings,"disable_networking.txt","")&&
        config_at(settings,"disable_overlay.txt","")&&config_at(settings,"disable_lobby_creation.txt","")&&
        config_at(settings,"DLC.txt",FROSTPUNK_DLC_CONFIG)&&config_at(settings,"steam_appid.txt","323190\n")&&
        config_at(settings,"force_account_name.txt","AndroidLocal\n")&&config_at(settings,"force_language.txt",std::string(system_language())+"\n")&&
        config_at(settings,"app_paths.txt",std::string("323190=")+game+"\n");
    if(settings>=0)close(settings);
    if(ok){std::lock_guard<std::mutex> lock(profile_mutex);int copy=fcntl(root,F_DUPFD_CLOEXEC,0);if(copy<0)ok=false;else{if(profile_fd>=0)close(profile_fd);profile_fd=copy;}}
    close(root);if(!ok)return 0;
    return !setenv("SteamAppPath",path.c_str(),1)&&!setenv("SteamAppId","323190",1)&&
        !setenv("SteamGameId","323190",1)&&!setenv("XDG_DATA_HOME",path.c_str(),1);
}
/* The relative path exactly as the backend opens it: Steam_User_Stats lower-cases stat and
 * leaderboard names (ascii_to_lowercase), then Local_Storage::sanitize_file_name drops a leading
 * "./", turns '\' into '/' and replaces | : * " ? with tokens (reference/goldberg/dll). */
static std::string storage_path(const char *name,int is_stat) {
    std::string raw(name);
    if(is_stat)for(char &ch:raw)if(ch>='A'&&ch<='Z')ch=(char)(ch-'A'+'a');
    if(raw.size()>2&&raw[0]=='.'&&(raw[1]=='\\'||raw[1]=='/'))raw.erase(0,2);
    std::string path;path.reserve(raw.size());
    for(char ch:raw)switch(ch){
        case '\\':path+='/';break;
        case '|':path+=".V_SLASH.";break;
        case ':':path+=".COLON.";break;
        case '*':path+=".ASTERISK.";break;
        case '"':path+=".QUOTE.";break;
        case '?':path+=".Q_MARK.";break;
        default:path+=ch;
    }
    return path;
}
/* Side-effect-free preflight, not an atomic replacement for the upstream
 * fstream IO. The final app must use a private profile with no untrusted writer
 * capable of replacing a checked component with a symlink between calls. */
extern "C" int FrostpunkOffline_ValidateStorageName(const char *name,int is_stat) {
    if(!name||!*name||name[0]=='/'||name[0]=='\\'||strlen(name)>4096)return 0;
    std::string path=storage_path(name,is_stat);
    for(char ch:path){unsigned char byte=(unsigned char)ch;if(byte<32||byte==127||ch=='<'||ch=='>')return 0;}
    std::vector<std::string> components;
    for(size_t start=0;;){size_t end=path.find('/',start);std::string part=path.substr(start,end==std::string::npos?std::string::npos:end-start);
        if(part.empty()||part=="."||part==".."||part.size()>255)return 0;components.push_back(part);if(end==std::string::npos)break;start=end+1;}
    int fd;{std::lock_guard<std::mutex> lock(profile_mutex);if(profile_fd<0)return 0;fd=fcntl(profile_fd,F_DUPFD_CLOEXEC,0);}if(fd<0)return 0;
    const char *fixed[]={"data","323190",is_stat?"stats":"remote"};
    for(const char *part:fixed){int next=openat(fd,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(fd);
        if(next<0)return saved==ENOENT;fd=next;}
    for(size_t i=0;i+1<components.size();++i){int next=openat(fd,components[i].c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);int saved=errno;close(fd);
        if(next<0)return saved==ENOENT;fd=next;}
    struct stat st;int status=fstatat(fd,components.back().c_str(),&st,AT_SYMLINK_NOFOLLOW),saved=errno;close(fd);
    return status?saved==ENOENT:S_ISREG(st.st_mode);
}
extern "C" unsigned long long FrostpunkOffline_DeniedSocketCalls(){return denied_sockets.load();}
extern "C" int FrostpunkOffline_TestNetworkDenied(){return __wrap_socket(AF_INET,SOCK_STREAM,0)==-1&&errno==EPERM;}
extern "C" unsigned FrostpunkOffline_Policy(){return 7;}
