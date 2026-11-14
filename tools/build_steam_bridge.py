"""Pin exact Steam SDK slots, verify its Win64 ABI, build the native bridge.

Backend is the explicitly configured local-only Goldberg implementation, never
an authenticated Steam connection. This builder does not run on the phone.
"""
import hashlib
import argparse
import json
from pathlib import Path
import re
import subprocess

import leaf_probe

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/"build/steam-bridge"
SDK=ROOT/"build/offline-services/source/sdk_includes"
PIN="475342f0d8b2bd7eb0d93bd7cfdd61e3ae7cda24"
INTERFACES={
    "ISteamClient":("isteamclient.h","SteamClient020"),
    "ISteamUser021":("isteamuser021.h","SteamUser021"),
    "ISteamApps":("isteamapps.h","STEAMAPPS_INTERFACE_VERSION008"),
    "ISteamUtils":("isteamutils.h","SteamUtils010"),
    "ISteamFriends":("isteamfriends.h","SteamFriends017"),
    "ISteamUserStats":("isteamuserstats.h","STEAMUSERSTATS_INTERFACE_VERSION012"),
    "ISteamRemoteStorage":("isteamremotestorage.h","STEAMREMOTESTORAGE_INTERFACE_VERSION016"),
}
ALLOWED={
    "ISteamClient":set("CreateSteamPipe BReleaseSteamPipe ConnectToGlobalUser CreateLocalUser ReleaseUser GetISteamUser GetISteamFriends GetISteamUtils GetISteamGenericInterface GetISteamUserStats GetISteamApps GetISteamRemoteStorage GetIPCCallCount SetWarningMessageHook BShutdownIfAllPipesClosed".split()),
    "ISteamUser021":set("GetHSteamUser BLoggedOn GetSteamID GetUserDataFolder GetGameBadgeLevel".split()),
    "ISteamApps":set("BIsSubscribed BIsLowViolence BIsCybercafe BIsVACBanned GetCurrentGameLanguage GetAvailableGameLanguages BIsSubscribedApp BIsDlcInstalled GetEarliestPurchaseUnixTime BIsSubscribedFromFreeWeekend GetDLCCount BGetDLCDataByIndex GetCurrentBetaName GetInstalledDepots GetAppInstallDir BIsAppInstalled GetAppOwner GetLaunchQueryParam GetDlcDownloadProgress GetAppBuildId GetLaunchCommandLine BIsSubscribedFromFamilySharing BIsTimedTrial".split()),
    "ISteamRemoteStorage":set("FileWrite FileRead FileWriteAsync FileReadAsync FileReadAsyncComplete FileForget FileDelete SetSyncPlatforms FileWriteStreamOpen FileWriteStreamWriteChunk FileWriteStreamClose FileWriteStreamCancel FileExists FilePersisted GetFileSize GetFileTimestamp GetSyncPlatforms GetFileCount GetFileNameAndSize GetQuota IsCloudEnabledForAccount IsCloudEnabledForApp SetCloudEnabledForApp".split()),
    "ISteamUserStats":set("RequestCurrentStats GetStat SetStat UpdateAvgRateStat GetAchievement SetAchievement ClearAchievement GetAchievementAndUnlockTime StoreStats GetAchievementIcon GetAchievementDisplayAttribute IndicateAchievementProgress GetNumAchievements GetAchievementName RequestUserStats GetUserStat GetUserAchievement GetUserAchievementAndUnlockTime ResetAllStats FindOrCreateLeaderboard FindLeaderboard GetLeaderboardName GetLeaderboardEntryCount GetLeaderboardSortMethod GetLeaderboardDisplayType DownloadLeaderboardEntries DownloadLeaderboardEntriesForUsers GetDownloadedLeaderboardEntry UploadLeaderboardScore AttachLeaderboardUGC GetNumberOfCurrentPlayers".split()),
    "ISteamUtils":set("GetSecondsSinceAppActive GetSecondsSinceComputerActive GetConnectedUniverse GetServerRealTime GetIPCountry GetImageSize GetImageRGBA GetCurrentBatteryPower GetAppID IsAPICallCompleted GetAPICallFailureReason GetAPICallResult GetIPCCallCount SetWarningMessageHook IsOverlayEnabled BOverlayNeedsPresent GetSteamUILanguage IsSteamRunningInVR IsSteamInBigPictureMode IsVRHeadsetStreamingEnabled".split()),
    # The optional monument button saves the profile before requesting the disabled offline overlay.
    "ISteamFriends":set("GetPersonaName GetPersonaState GetFriendCount GetFriendByIndex GetFriendRelationship GetFriendPersonaState GetFriendPersonaName GetFriendGamePlayed GetFriendPersonaNameHistory GetFriendSteamLevel GetPlayerNickname GetFriendsGroupCount GetFriendsGroupIDByIndex GetFriendsGroupName GetFriendsGroupMembersCount GetFriendsGroupMembersList HasFriend GetClanCount GetClanByIndex GetClanName GetClanTag GetClanActivityCounts GetFriendCountFromSource GetFriendFromSourceByIndex IsUserInSource ActivateGameOverlayToWebPage GetSmallFriendAvatar GetMediumFriendAvatar GetLargeFriendAvatar RequestUserInformation SetRichPresence ClearRichPresence GetFriendRichPresence GetFriendRichPresenceKeyCount GetFriendRichPresenceKeyByIndex".split()),
}
COUNTS={
    "GetUserDataFolder":{1:"count_s(c,2)"}, "BGetDLCDataByIndex":{4:"count_s(c,5)"},
    "GetCurrentBetaName":{1:"count_s(c,2)"},"GetInstalledDepots":{2:"count_u(c,3)"},
    "GetAppInstallDir":{2:"count_u(c,3)"},"GetLaunchCommandLine":{1:"count_s(c,2)"},
    "FileWrite":{2:"count_file_s(c,3)"},"FileRead":{2:"count_file_s(c,3)"},
    "FileWriteAsync":{2:"count_file_u(c,3)"},"FileReadAsyncComplete":{2:"count_file_u(c,3)"},
    "FileWriteStreamWriteChunk":{2:"count_file_s(c,3)"},"GetImageRGBA":{2:"count_s(c,3)"},
    "DownloadLeaderboardEntriesForUsers":{2:"count_s(c,3)"},
    "GetDownloadedLeaderboardEntry":{4:"count_s(c,5)"},"UploadLeaderboardScore":{4:"count_s(c,5)"},
    "GetFriendsGroupMembersList":{2:"count_s(c,3)"},
}


def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def run(command,**kwargs):
    p=subprocess.run([str(x) for x in command],capture_output=True,text=True,**kwargs)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    return p.stdout
def changed(path,text):
    if not path.exists()or path.read_text()!=text:path.write_text(text,encoding="utf-8",newline="\n")


def methods(interface,file):
    text=(SDK/file).read_text(encoding="utf-8",errors="strict")
    text=re.sub(r"/\*.*?\*/|//[^\n]*","",text,flags=re.S)
    match=re.search(r"class\s+"+interface+r"\s*\{(.*?)\n\};",text,re.S)
    if not match:raise ValueError("SDK class missing: "+interface)
    result=[]
    for declaration in re.findall(r"virtual\s+(.*?)\s*=\s*0\s*;",match[1],re.S):
        signature=re.fullmatch(r"(.*?)\b(\w+)\s*\((.*)\)\s*",declaration,re.S)
        if not signature:raise ValueError("Unparsed SDK virtual: "+declaration)
        returns,name,params=signature.groups();params=re.sub(r"STEAM_\w+\([^)]*\)","",params)
        types=[]
        for p in params.split(","):
            p=p.split("=")[0].strip()
            if not p or p=="void":continue
            p=re.sub(r"\b\w+\s*$","",p).strip();types.append(" ".join(p.split()))
        result.append({"slot":len(result),"name":name,"returns":" ".join(returns.split()),"types":types})
    if not result or len(result)>256:raise ValueError("Invalid SDK slot count")
    return result


def metadata():
    all_methods={i:methods(i,f) for i,(f,v) in INTERFACES.items()}
    header=["#pragma once","enum class Kind {"+",".join(INTERFACES)+",Count};",
            "static constexpr const char*versions[]={"+",".join(json.dumps(v)for f,v in INTERFACES.values())+"};",
            "static constexpr unsigned slot_counts[]={"+",".join(str(len(v))for v in all_methods.values())+"};",
            "static constexpr const char*slot_names[][256]={"]
    for i,entries in all_methods.items():header.append("{"+",".join(json.dumps(m["name"])for m in entries)+"},")
    header.append("};")
    dispatch=[];coverage={}
    getters={"GetISteamUser":"ISteamUser021","GetISteamFriends":"ISteamFriends","GetISteamUtils":"ISteamUtils","GetISteamUserStats":"ISteamUserStats","GetISteamApps":"ISteamApps","GetISteamRemoteStorage":"ISteamRemoteStorage","GetISteamGenericInterface":"Count"}
    for interface,entries in all_methods.items():
        dispatch.append(f"case Kind::{interface}:switch(slot){{");bound=[]
        for m in entries:
            name=m["name"];slot=m["slot"]
            if name not in ALLOWED[interface]:continue
            if name=="SetWarningMessageHook":statement=f"set_warning(c,Kind::{interface});"
            elif interface=="ISteamClient" and name in getters:statement=f"client_get(c,Kind::{getters[name]},{slot});"
            elif name=="GetAPICallResult":statement="api_result(c);"
            else:
                casts=f"static_cast<{m['returns']}({interface}::*)({','.join(m['types'])})>(&{interface}::{name})"
                sizes=COUNTS.get(name,{})
                statement=""
                # GetAchievement reads an in-memory ID map, never a storage path.
                if interface in ["ISteamRemoteStorage","ISteamUserStats"] and not (interface=="ISteamUserStats" and name=="GetAchievement"):
                    for n,t in enumerate(m["types"],1):
                        if t.replace(" ","")=="constchar*":statement+=f"relative_name(arg(c,{n}),{int(interface=='ISteamUserStats')});"
                counts=",counts({"+",".join("{"+str(k)+","+v+"}"for k,v in sizes.items())+"})"if sizes else""
                statement+=f"invoke(c,{casts}{counts});"
            dispatch.append(f"case {slot}: {statement}return 1;");bound.append(slot)
        dispatch.append("default:break;}break;")
        coverage[interface]={"version":INTERFACES[interface][1],"methods":[dict(m,bound=m["slot"]in bound)for m in entries]}
    changed(OUT/"steam_metadata.h","\n".join(header)+"\n")
    changed(OUT/"steam_dispatch.inc","\n".join(dispatch)+"\n")
    changed(OUT/"coverage.json",json.dumps(coverage,indent=2)+"\n")
    return coverage


def abi(toolchain):
    b,env=leaf_probe.windows_toolchain()
    run([b/"ml64.exe","/nologo","/c","/Fo"+str(OUT/"abi-x64.obj"),ROOT/"tests/steam_abi_x64.asm"],cwd=OUT,env=env)
    run([b/"cl.exe","/nologo","/O2","/std:c++17","/DSTEAM_WIN32","/DSTEAM_API_NODLL","/I"+str(ROOT/"reference/goldberg/sdk_includes"),ROOT/"tests/steam_abi.cpp",OUT/"abi-x64.obj","/Fe"+str(OUT/"abi-x64.exe")],cwd=OUT,env=env)
    lines=run([OUT/"abi-x64.exe"]).splitlines();result=[json.loads(x)for x in lines]
    if not result[-1]["success"]:raise ValueError("Native MSVC class ABI failed")
    run([toolchain/"clang++.exe","--target=aarch64-linux-android30","-O2","-std=c++17","-static-libstdc++","-Wl,-z,max-page-size=16384","-DSTEAM_API_NODLL","-I"+str(SDK),ROOT/"tests/steam_abi.cpp",ROOT/"tests/steam_abi_arm64.S","-o",OUT/"abi-arm64"])
    changed(OUT/"abi-windows.json",json.dumps(result,indent=2)+"\n")
    return result


def device_test(config,serial,receipt):
    adb=Path(config["android_sdk"])/"platform-tools/adb.exe";base="/data/local/tmp/frostpunk/offline-services";runs=[]
    def adb_run(arguments,expected=0):
        command=[str(adb),"-s",serial,*map(str,arguments)];p=subprocess.run(command,capture_output=True,text=True)
        record={"command":command,"returncode":p.returncode,"stdout":p.stdout,"stderr":p.stderr};runs.append(record)
        if expected==0 and p.returncode:raise RuntimeError(str(record))
        return p
    binaries={OUT/"steam-bridge-probe":"steam-bridge-probe",OUT/"abi-arm64":"steam-abi-arm64",ROOT/"build/offline-services/runtime/libfrostpunk_offline_services.so":"libfrostpunk_offline_services.so"}
    for host,name in binaries.items():adb_run(["push",host,base+"/"+name])
    adb_run(["shell","chmod","755",base+"/steam-bridge-probe",base+"/steam-abi-arm64"])
    adb_run(["shell","mkdir","-p",base+"/bridge-profile"])
    oracle=[json.loads(x)for x in adb_run(["shell",base+"/steam-abi-arm64"]).stdout.splitlines()]
    if not oracle[-1]["success"] or oracle[0]!=receipt["windows_abi"][0]:raise ValueError("MSVC/Android actual callback/ID ABI mismatch")
    command=["shell","env","LD_LIBRARY_PATH="+base,base+"/steam-bridge-probe",base+"/libfrostpunk_offline_services.so",base+"/bridge-profile","/sdcard/Frostpunk/game"]
    result=json.loads(adb_run(command).stdout)
    if not result["pass"]:raise ValueError("Native typed Steam bridge fixture failed")
    concurrent=json.loads(adb_run([*command,"shutdown-race"]).stdout)
    if not concurrent["pass"]:raise ValueError("Callback/worker/shutdown lifetime fixture failed")
    negative=[]
    for mode in ["path-traversal","absolute-path","size-overrun","negative-file-count","oversized-file-count","negative-stream-count","oversized-async-count","async-size-overrun","wrong-kind","unknown-slot","callback-size","shutdown-self"]:
        p=adb_run([*command,mode],expected=1);passed=p.returncode!=0 and ("UNSUPPORTED_LOCAL_STEAM:"in p.stderr+p.stdout or "invalid guest pointer/range/write permission"in p.stderr+p.stdout)
        negative.append({"mode":mode,"passed":passed,"returncode":p.returncode})
        if not passed:raise ValueError("Service boundary contract did not fail closed: "+mode)
    actual={}
    for host,name in binaries.items():
        value=adb_run(["shell","sha256sum",base+"/"+name]).stdout.split()[0]
        if value!=digest(host):raise ValueError("Deployed native Steam hash mismatch")
        actual[name]=value
    evidence={"success":True,"serial":serial,"target":"aarch64-linux-android30","result":result,"concurrent":concurrent,"abi_device":oracle,"negative":negative,"deployed_hashes":actual,"build_receipt_sha256":digest(OUT/"build-receipt.json"),"runs":runs,"game_boot_proven":False,"atomic_storage_IO":False,"storage_boundary":"Lexical validation plus native retained-profile-fd O_NOFOLLOW preflight; same-UID concurrent filesystem mutation remains outside this preflight contract"}
    changed(ROOT/"research/steam-win64-bridge.json",json.dumps(evidence,indent=2)+"\n")
    print(json.dumps(result,indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--run",action="store_true");parser.add_argument("--serial");args=parser.parse_args()
    if args.run and not args.serial:parser.error("--run requires --serial")
    OUT.mkdir(parents=True,exist_ok=True)
    if run(["git","-C",ROOT/"reference/goldberg","rev-parse","HEAD"]).strip()!=PIN:raise ValueError("Goldberg pin mismatch")
    backend=json.loads((ROOT/"research/offline-services-build.json").read_text())
    if backend["goldberg_commit"]!=PIN or backend["experimental"] or not backend["offline"] or backend["cloud"]:raise ValueError("Offline backend policy receipt mismatch")
    for path in SDK.glob("*.h"):
        key="sdk_includes/"+path.name
        patch=backend["private_overlay_patches"].get(key)
        expected=patch["patched_sha256"]if patch else backend["source_inputs"][str(Path("reference/goldberg")/key)]
        if digest(path)!=expected:raise ValueError("SDK differs from compiled backend receipt: "+key)
    dso=ROOT/"build/offline-services/runtime/libfrostpunk_offline_services.so"
    if digest(dso)!=backend["binaries"][dso.name]["sha256"]:raise ValueError("Native backend binary differs from build receipt")
    config=json.loads((ROOT/"port.json").read_text());toolchain=Path(config["android_sdk"])/"ndk"/config["android_ndk_version"] /"toolchains/llvm/prebuilt/windows-x86_64/bin"
    coverage=metadata();oracle=abi(toolchain)
    inputs=[ROOT/"android/steam/bridge.cpp",ROOT/"android/steam/bridge.h",ROOT/"tools/build_steam_bridge.py",ROOT/"android/host/win64.h",ROOT/"native/runtime64/cpu64.h",ROOT/"research/offline-services-build.json",dso,*sorted(SDK.glob("*.h")),OUT/"steam_metadata.h",OUT/"steam_dispatch.inc",ROOT/"tests/steam_abi.cpp",ROOT/"tests/steam_abi_x64.asm",ROOT/"tests/steam_abi_arm64.S",ROOT/"tests/nativeSteam_bridge.cpp"]
    before={str(p.relative_to(ROOT)):digest(p)for p in inputs}
    flags=["--target=aarch64-linux-android30","-O2","-std=c++17","-fPIC","-fno-strict-aliasing","-Wall","-Wextra","-Werror","-Wno-sign-compare","-Wno-unused-function","-Wno-unknown-pragmas","-Wno-inconsistent-missing-override","-I"+str(SDK),"-I"+str(ROOT/"native/runtime64"),"-I"+str(ROOT/"android/host"),"-I"+str(OUT)]
    run([toolchain/"clang++.exe",*flags,"-c",ROOT/"android/steam/bridge.cpp","-o",OUT/"bridge.o"])
    run([toolchain/"llvm-ar.exe","rcs",OUT/"libwin64_steam.a",OUT/"bridge.o"])
    run([toolchain/"clang++.exe",*flags,"-I"+str(ROOT/"android/steam"),ROOT/"tests/nativeSteam_bridge.cpp",OUT/"libwin64_steam.a","-static-libstdc++","-Wl,-z,max-page-size=16384","-ldl","-o",OUT/"steam-bridge-probe"])
    if before!={str(p.relative_to(ROOT)):digest(p)for p in inputs}:raise ValueError("Steam bridge inputs changed during build")
    receipt={"success":True,"upstream_revision":PIN,"target":"aarch64-linux-android30","compiler_sha256":digest(toolchain/"clang++.exe"),"flags":flags,"inputs":before,"coverage":{i:{"bound":sum(m["bound"]for m in v["methods"]),"total":len(v["methods"])}for i,v in coverage.items()},"windows_abi":oracle,"artifacts":{str(p.relative_to(ROOT)):digest(p)for p in [OUT/"libwin64_steam.a",OUT/"abi-arm64",OUT/"steam-bridge-probe"]},"android_execution_proven":False}
    changed(OUT/"build-receipt.json",json.dumps(receipt,indent=2)+"\n")
    print(json.dumps(receipt["coverage"],indent=2))
    if args.run:device_test(config,args.serial,receipt)


if __name__=="__main__":main()
