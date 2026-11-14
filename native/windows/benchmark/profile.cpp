#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <wchar.h>
#include "detours.h"

static wchar_t profile[MAX_PATH];
static decltype(&SHGetFolderPathW) real_folder = SHGetFolderPathW;
static decltype(&SHGetSpecialFolderPathW) real_special = SHGetSpecialFolderPathW;
static decltype(&SHGetKnownFolderPath) real_known = SHGetKnownFolderPath;

static HRESULT WINAPI private_folder(HWND, int, HANDLE, DWORD, LPWSTR output) {
    if (!output) return E_INVALIDARG;
    DWORD saved = GetLastError();
    wcscpy_s(output, MAX_PATH, profile);
    SetLastError(saved);
    return S_OK;
}

static BOOL WINAPI private_special(HWND, LPWSTR output, int, BOOL) {
    if (!output) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    DWORD saved = GetLastError();
    wcscpy_s(output, MAX_PATH, profile);
    SetLastError(saved);
    return TRUE;
}

static HRESULT WINAPI private_known(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *output) {
    if (!output) return E_INVALIDARG;
    DWORD saved = GetLastError();
    *output = static_cast<PWSTR>(CoTaskMemAlloc((wcslen(profile) + 1) * sizeof(wchar_t)));
    if (!*output) { SetLastError(saved); return E_OUTOFMEMORY; }
    wcscpy_s(*output, wcslen(profile) + 1, profile);
    SetLastError(saved);
    return S_OK;
}

extern "C" __declspec(dllexport) BOOL FrostpunkBenchmarkIsolationReady() { return profile[0] != 0; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH || DetourIsHelperProcess()) return TRUE;
    if (!DetourRestoreAfterWith()) return FALSE;
    DWORD count = GetEnvironmentVariableW(L"FROSTPUNK_BENCHMARK_PROFILE", profile, MAX_PATH);
    wchar_t absolute[MAX_PATH];
    if (!count || count >= MAX_PATH || !GetFullPathNameW(profile, MAX_PATH, absolute, NULL) ||
        _wcsicmp(profile, absolute) || GetFileAttributesW(profile) == INVALID_FILE_ATTRIBUTES ||
        !(GetFileAttributesW(profile) & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    if (DetourTransactionBegin() != NO_ERROR) return FALSE;
    LONG result = DetourUpdateThread(GetCurrentThread());
    if (!result) result = DetourAttach(reinterpret_cast<PVOID *>(&real_folder), private_folder);
    if (!result) result = DetourAttach(reinterpret_cast<PVOID *>(&real_special), private_special);
    if (!result) result = DetourAttach(reinterpret_cast<PVOID *>(&real_known), private_known);
    if (result) { DetourTransactionAbort(); return FALSE; }
    return DetourTransactionCommit() == NO_ERROR;
}
