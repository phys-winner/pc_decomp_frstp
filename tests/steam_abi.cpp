#include "steam_api_common.h"
#include "isteamuserstats.h"
#include "isteamremotestorage.h"
#include "isteamfriends.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
class AbiMethods {
public:
    virtual CSteamID Steam()=0;
    virtual CGameID Game()=0;
    virtual uint64 Values(CSteamID,CGameID,uint64)=0;
};
extern "C" void steam_abi_return();
extern "C" void game_abi_return();
extern "C" void steam_abi_values();
struct Object { void (*const *table)(); uint64_t captured[4]; };
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
NOINLINE uint64_t steam_return(AbiMethods *object){return object->Steam().ConvertToUint64();}
NOINLINE uint64_t game_return(AbiMethods *object){return object->Game().ToUint64();}
NOINLINE uint64_t values(AbiMethods *object){return object->Values(CSteamID(uint64(0x1122334455667788)),CGameID(uint64(0x8877665544332211)),uint64(0xaabbccddeeff0011));}
int main(){
    static_assert(sizeof(CSteamID)==8&&sizeof(CGameID)==8);
    static_assert(sizeof(UserStatsReceived_t)==24);
    void (*table[])()={steam_abi_return,game_abi_return,steam_abi_values};Object object={table,{0,0,0,0}};
    uint64_t steam=steam_return(reinterpret_cast<AbiMethods*>(&object));
    uint64_t game=game_return(reinterpret_cast<AbiMethods*>(&object));
    uint64_t value=values(reinterpret_cast<AbiMethods*>(&object));
    printf("{\"layouts\":{\"CSteamID\":[%zu,%zu],\"CGameID\":[%zu,%zu],\"CCallbackBase\":[%zu,%zu],\"UserStatsReceived_t\":[%zu,%zu,%zu,%zu],\"UserStatsStored_t\":[%zu,%zu,%zu],\"UserAchievementStored_t\":[%zu,%zu,%zu,%zu,%zu,%zu],\"RemoteStorageFileReadAsyncComplete_t\":[%zu,%zu,%zu,%zu,%zu],\"RemoteStorageFileWriteAsyncComplete_t\":[%zu,%zu],\"FriendGameInfo_t\":[%zu,%zu,%zu,%zu,%zu,%zu]}}\n",
      sizeof(CSteamID),alignof(CSteamID),sizeof(CGameID),alignof(CGameID),sizeof(CCallbackBase),alignof(CCallbackBase),
      sizeof(UserStatsReceived_t),offsetof(UserStatsReceived_t,m_nGameID),offsetof(UserStatsReceived_t,m_eResult),offsetof(UserStatsReceived_t,m_steamIDUser),
      sizeof(UserStatsStored_t),offsetof(UserStatsStored_t,m_nGameID),offsetof(UserStatsStored_t,m_eResult),
      sizeof(UserAchievementStored_t),offsetof(UserAchievementStored_t,m_nGameID),offsetof(UserAchievementStored_t,m_bGroupAchievement),offsetof(UserAchievementStored_t,m_rgchAchievementName),offsetof(UserAchievementStored_t,m_nCurProgress),offsetof(UserAchievementStored_t,m_nMaxProgress),
      sizeof(RemoteStorageFileReadAsyncComplete_t),offsetof(RemoteStorageFileReadAsyncComplete_t,m_hFileReadAsync),offsetof(RemoteStorageFileReadAsyncComplete_t,m_eResult),offsetof(RemoteStorageFileReadAsyncComplete_t,m_nOffset),offsetof(RemoteStorageFileReadAsyncComplete_t,m_cubRead),
      sizeof(RemoteStorageFileWriteAsyncComplete_t),offsetof(RemoteStorageFileWriteAsyncComplete_t,m_eResult),
      sizeof(FriendGameInfo_t),offsetof(FriendGameInfo_t,m_gameID),offsetof(FriendGameInfo_t,m_unGameIP),offsetof(FriendGameInfo_t,m_usGamePort),offsetof(FriendGameInfo_t,m_usQueryPort),offsetof(FriendGameInfo_t,m_steamIDLobby));
    bool pass=steam==0x123456789abcdef0&&game==0xfedcba9876543210&&
        object.captured[0]==0x1122334455667788&&object.captured[1]==0x8877665544332211&&object.captured[2]==0xaabbccddeeff0011&&value==0x55aa55aa55aa55aa;
    printf("{\"success\":%s,\"steam_id_size\":%zu,\"game_id_size\":%zu,\"user_stats_received_size\":%zu,\"user_stats_steam_id_offset\":%zu,\"SteamID_argument\":\"scalar64\",\"GameID_argument\":\"indirect_pointer\",\"member_class_return\":\"%s\"}\n",pass?"true":"false",sizeof(CSteamID),sizeof(CGameID),sizeof(UserStatsReceived_t),offsetof(UserStatsReceived_t,m_steamIDUser),
#if defined(_MSC_VER)
    "hidden_RDX_this_RCX"
#else
    "SteamID_x0_GameID_hidden_x8"
#endif
    );return pass?0:1;
}
