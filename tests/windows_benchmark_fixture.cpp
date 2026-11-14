#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <assert.h>
#include <stdio.h>
#include <string>
#include "detours.h"

static int missing_environment(const wchar_t *directory) {
    wchar_t target[32768], dll[32768];
    assert(GetModuleFileNameW(NULL, target, 32768));
    wcscpy_s(dll, target);
    wchar_t *last = wcsrchr(dll, L'\\');
    assert(last);
    wcscpy_s(last + 1, 32768 - (last + 1 - dll), L"frostpunk-benchmark-profile.dll");
    char dll_a[32768];
    assert(WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, dll, -1, dll_a, sizeof(dll_a), NULL, NULL));
    std::wstring marker = std::wstring(directory) + L"\\entry-must-not-run.txt";
    std::wstring command = L"\"" + std::wstring(target) + L"\" --entry-marker \"" + marker + L"\"";
    assert(SetEnvironmentVariableW(L"FROSTPUNK_BENCHMARK_PROFILE", NULL));
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    assert(DetourCreateProcessWithDllExW(target, &command[0], NULL, NULL, TRUE,
        CREATE_SUSPENDED, NULL, NULL, &startup, &process, dll_a, NULL));
    assert(ResumeThread(process.hThread) != static_cast<DWORD>(-1));
    CloseHandle(process.hThread);
    DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 73);
    assert(waited == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    assert(GetExitCodeProcess(process.hProcess, &exit_code));
    CloseHandle(process.hProcess);
    assert(exit_code == 0xc0000142u);
    assert(GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND);
    puts("WINDOWS_BENCHMARK_MISSING_ENV_PASS DLL_INIT_FAILED=1 target_entry_executed=0");
    return 0;
}

int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc == 3 && !wcscmp(argv[1], L"--test-missing-env")) return missing_environment(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--entry-marker")) {
        HANDLE file = CreateFileW(argv[2], GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        CloseHandle(file);
        return 0;
    }
    assert(argc == 5);
    assert(!wcscmp(argv[1], L"two words") && !wcscmp(argv[2], L"a\"b") && !wcscmp(argv[3], L"trailing\\"));
    wchar_t expected[MAX_PATH], output[MAX_PATH];
    assert(GetEnvironmentVariableW(L"FROSTPUNK_BENCHMARK_PROFILE", expected, MAX_PATH));
    assert(!wcscmp(argv[4], expected));
    HMODULE library = GetModuleHandleW(L"frostpunk-benchmark-profile.dll");
    assert(library);
    auto ready = reinterpret_cast<BOOL (*)()>(GetProcAddress(library, "FrostpunkBenchmarkIsolationReady"));
    assert(ready && ready());
    for (const wchar_t *name : {L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA"}) {
        assert(GetEnvironmentVariableW(name, output, MAX_PATH));
        assert(!wcscmp(expected, output));
    }
    SetLastError(12345);
    assert(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, output) == S_OK);
    assert(!wcscmp(expected, output) && GetLastError() == 12345);
    SetLastError(23456);
    assert(SHGetSpecialFolderPathW(NULL, output, CSIDL_APPDATA, FALSE));
    assert(!wcscmp(expected, output) && GetLastError() == 23456);
    PWSTR known = NULL;
    SetLastError(34567);
    assert(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &known) == S_OK);
    assert(known && !wcscmp(expected, known) && GetLastError() == 34567);
    CoTaskMemFree(known);
    assert(SHGetFolderPathW(NULL, 0, NULL, 0, NULL) == E_INVALIDARG);
    assert(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, NULL) == E_INVALIDARG);
    assert(!SHGetSpecialFolderPathW(NULL, NULL, 0, FALSE) && GetLastError() == ERROR_INVALID_PARAMETER);
    std::wstring marker = std::wstring(expected) + L"\\fixture-marker.txt";
    HANDLE file = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    DWORD written;
    assert(WriteFile(file, "private", 7, &written, NULL) && written == 7);
    assert(CloseHandle(file));
    puts("WINDOWS_BENCHMARK_FIXTURE_PASS shell_APIs=3 private_environment=3 LastError_preserved=3 arguments_quoted=3 marker_private=1");
    return 0;
}
