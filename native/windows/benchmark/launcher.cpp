#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "detours.h"
#include "identity.h"

static int fail(const wchar_t *message) {
    fwprintf(stderr, L"BENCHMARK_LAUNCH_REJECT %ls error=%lu\n", message, GetLastError());
    return 73;
}

static bool exact_file(const wchar_t *path, const char *expected) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char buffer[65536], digest[32];
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
              BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    DWORD got = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) { ok = false; break; }
        if (!got) break;
        ok = BCryptHashData(hash, buffer, got, 0) >= 0;
    }
    ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    char actual[65];
    for (unsigned i = 0; ok && i < sizeof(digest); ++i) sprintf_s(actual + i * 2, 3, "%02x", digest[i]);
    return ok && !strcmp(actual, expected);
}

static std::wstring quote(const wchar_t *input) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (; *input; ++input) {
        if (*input == L'\\') { ++slashes; continue; }
        result.append(slashes * (*input == L'\"' ? 2 : 1), L'\\');
        if (*input == L'\"') result += L'\\';
        result += *input;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc < 3) {
        fputs("Usage: frostpunk-benchmark.exe <exact-staged-original-exe> <private-profile> [target args...]\n", stderr);
        return 64;
    }
    wchar_t target[32768], profile[MAX_PATH], dll[32768];
    DWORD target_size = GetFullPathNameW(argv[1], 32768, target, NULL);
    DWORD profile_size = GetFullPathNameW(argv[2], MAX_PATH, profile, NULL);
    DWORD dll_size = GetModuleFileNameW(NULL, dll, 32768);
    if (!target_size || target_size >= 32768 || !profile_size || profile_size >= MAX_PATH ||
        !dll_size || dll_size >= 32768) return fail(L"absolute path");
    wchar_t *last = wcsrchr(dll, L'\\');
    if (!last) return fail(L"launcher directory");
    wcscpy_s(last + 1, 32768 - (last + 1 - dll), L"frostpunk-benchmark-profile.dll");
    bool fixture = !_wcsicmp(target, FP_FIXTURE_PATH);
    if ((!fixture && _wcsicmp(target, FP_ORIGINAL_PATH)) ||
        !exact_file(target, fixture ? FP_FIXTURE_SHA256 : FP_ORIGINAL_SHA256)) return fail(L"target identity");
    if (!exact_file(dll, FP_PROFILE_DLL_SHA256)) return fail(L"isolation DLL identity");
    DWORD attributes = GetFileAttributesW(profile);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return fail(L"private profile missing");
    if (!fixture && _wcsicmp(profile, FP_PRIVATE_PROFILE)) return fail(L"game local_save profile differs");
    if (!fixture && (!exact_file(FP_OFFLINE_DLL_PATH, FP_OFFLINE_DLL_SHA256) ||
                     !exact_file(FP_LOCAL_SAVE_PATH, FP_LOCAL_SAVE_SHA256))) return fail(L"offline provider identity");
    for (const wchar_t *name : {L"FROSTPUNK_BENCHMARK_PROFILE", L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA"})
        if (!SetEnvironmentVariableW(name, profile)) return fail(L"child private environment");
    if (!SetEnvironmentVariableW(L"SteamAppPath", FP_GAME_DIRECTORY) ||
        !SetEnvironmentVariableW(L"SteamAppId", L"323190") ||
        !SetEnvironmentVariableW(L"SteamGameId", L"323190")) return fail(L"offline Steam environment");
    std::wstring command = quote(target);
    for (int i = 3; i < argc; ++i) command += L" " + quote(argv[i]);
    if (command.size() >= 32767) return fail(L"command line too long");
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(0);
    char dll_a[32768];
    BOOL substituted = FALSE;
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, dll, -1, dll_a, sizeof(dll_a), NULL, &substituted) || substituted)
        return fail(L"Detours DLL path encoding");
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!DetourCreateProcessWithDllExW(target, mutable_command.data(), NULL, NULL, TRUE,
            CREATE_SUSPENDED, NULL, fixture ? NULL : FP_GAME_DIRECTORY, &startup, &process,
            dll_a, NULL)) return fail(L"suspended isolated process creation");
    fwprintf(stderr, L"BENCHMARK_CHILD pid=%lu original_native=%u private_profile=%ls\n", process.dwProcessId, !fixture, profile);
    bool resumed = ResumeThread(process.hThread) != static_cast<DWORD>(-1);
    if (!resumed) TerminateProcess(process.hProcess, 73);
    CloseHandle(process.hThread);
    DWORD result = 73;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hProcess);
    return static_cast<int>(result);
}
