#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "steam_api.h"
#include <cassert>
#include <cstdio>
#include <cstring>

template<class T> static T symbol(HMODULE library, const char *name) {
    FARPROC result = GetProcAddress(library, name);
    assert(result);
    return reinterpret_cast<T>(result);
}

int wmain(int argc, wchar_t **argv) {
    assert(argc == 4);
    assert(SetEnvironmentVariableW(L"SteamAppPath", argv[2]));
    assert(SetEnvironmentVariableW(L"SteamAppId", L"323190"));
    assert(SetEnvironmentVariableW(L"SteamGameId", L"323190"));
    assert(SetCurrentDirectoryW(argv[2]));
    bool writing = !wcscmp(argv[3], L"write");
    assert(writing || !wcscmp(argv[3], L"read"));
    HMODULE library = LoadLibraryW(argv[1]);
    assert(library);
    assert(symbol<bool(*)()>(library, "SteamAPI_Init")());
    HSteamUser user = symbol<HSteamUser(*)()>(library, "SteamAPI_GetHSteamUser")();
    auto factory = symbol<void*(*)(HSteamUser, const char*)>(library, "SteamInternal_FindOrCreateUserInterface");
    auto storage = static_cast<ISteamRemoteStorage*>(factory(user, "STEAMREMOTESTORAGE_INTERFACE_VERSION016"));
    auto apps = static_cast<ISteamApps*>(factory(user, "STEAMAPPS_INTERFACE_VERSION008"));
    auto stats = static_cast<ISteamUserStats*>(factory(user, "STEAMUSERSTATS_INTERFACE_VERSION012"));
    auto steam_user = static_cast<ISteamUser*>(factory(user, "SteamUser021"));
    auto utils = static_cast<ISteamUtils*>(factory(user, "SteamUtils010"));
    assert(storage && apps && stats && steam_user && utils);
    assert(utils->GetAppID() == 323190 && !steam_user->BLoggedOn());
    assert(!storage->IsCloudEnabledForAccount() && !storage->IsCloudEnabledForApp());
    storage->SetCloudEnabledForApp(true);
    assert(!storage->IsCloudEnabledForAccount() && !storage->IsCloudEnabledForApp());
    const AppId_t dlc_ids[] = {1125020, 1146960, 1147010};
    const char *dlc_names[] = {"Frostpunk: The Rifts", "Frostpunk: The Last Autumn", "Frostpunk: On The Edge"};
    assert(apps->GetDLCCount() == 3);
    for (int i = 0; i < 3; ++i) {
        AppId_t id = 0; bool available = false; char name[64] = {};
        assert(apps->BIsDlcInstalled(dlc_ids[i]) && apps->BIsSubscribedApp(dlc_ids[i]));
        assert(apps->BGetDLCDataByIndex(i, &id, &available, name, sizeof(name)) && id == dlc_ids[i] && available && !strcmp(name, dlc_names[i]));
    }
    assert(!apps->BIsDlcInstalled(867400) && !apps->BIsSubscribedApp(867400) && !apps->BGetDLCDataByIndex(3, nullptr, nullptr, nullptr, 0));
    const unsigned char expected[] = {0, 1, 0x1a, 0xff, 42, 0, 7, 9};
    unsigned char actual[sizeof(expected)] = {};
    if (writing) {
        assert(storage->FileWrite("windows_probe.bin", expected, sizeof(expected)));
        assert(stats->SetStat("windows_probe_stat", int32(31337)) && stats->StoreStats());
    }
    assert(storage->FileRead("windows_probe.bin", actual, sizeof(actual)) == sizeof(actual));
    assert(!memcmp(actual, expected, sizeof(actual)));
    int32 value = 0;
    assert(stats->GetStat("windows_probe_stat", &value) && value == 31337);
    auto attempts = symbol<unsigned long long(*)()>(library, "FrostpunkOffline_DeniedSocketCalls");
    assert(attempts() == 0);
    assert(symbol<int(*)()>(library, "FrostpunkOffline_TestNetworkDenied")());
    assert(attempts() == 1);
    symbol<void(*)()>(library, "SteamAPI_Shutdown")();
    puts("{\"pass\":true,\"native_windows_x64\":true,\"init\":true,\"cloud\":false,\"offline\":true,\"DLC_count\":3,\"binary_bytes\":8,\"stat_value\":31337,\"backend_socket_attempts\":0,\"negative_socket_denied\":true}");
    return 0;
}
