// Host fixture only: exercise the exact cache state machine with real Win32
// files. ARM fixture executes openat/O_NOFOLLOW/fsync/renameat directly.
#include <windows.h>
#include <string>

static HANDLE pcHandle(intptr_t fd) { return reinterpret_cast<HANDLE>(fd); }
static intptr_t pcFd(HANDLE h) { return h == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<intptr_t>(h); }
static std::string pcPath(intptr_t directory, const char* name) {
  char path[4096];
  DWORD n = GetFinalPathNameByHandleA(pcHandle(directory), path, sizeof(path), FILE_NAME_NORMALIZED);
  return !n || n >= sizeof(path) ? std::string() : std::string(path, n) + "\\" + name;
}
static intptr_t pcOpenDirectory(const char* path) {
  HANDLE h = CreateFileA(path, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  BY_HANDLE_FILE_INFORMATION info;
  if (h != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(h, &info)
      || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))) {
    CloseHandle(h); h = INVALID_HANDLE_VALUE;
  }
  return pcFd(h);
}
static intptr_t pcOpenRead(intptr_t dir, const char* name) {
  return pcFd(CreateFileA(pcPath(dir, name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}
static intptr_t pcOpenNew(intptr_t dir, const char* name) {
  return pcFd(CreateFileA(pcPath(dir, name).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}
static bool pcSize(intptr_t fd, size_t* bytes) {
  BY_HANDLE_FILE_INFORMATION st;
  if (!GetFileInformationByHandle(pcHandle(fd), &st) || (st.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
  *bytes = size_t((uint64_t(st.nFileSizeHigh) << 32) | st.nFileSizeLow); return true;
}
static intptr_t pcRead(intptr_t fd, void* data, size_t bytes) {
  DWORD n = 0; return ReadFile(pcHandle(fd), data, DWORD(bytes), &n, nullptr) ? intptr_t(n) : -1;
}
static intptr_t pcWrite(intptr_t fd, const void* data, size_t bytes) {
  DWORD n = 0; return WriteFile(pcHandle(fd), data, DWORD(bytes), &n, nullptr) ? intptr_t(n) : -1;
}
static bool pcSync(intptr_t fd) { return FlushFileBuffers(pcHandle(fd)) != 0; }
static bool pcClose(intptr_t fd) { return CloseHandle(pcHandle(fd)) != 0; }
static bool pcRename(intptr_t dir, const char* from, const char* to) {
  return MoveFileExA(pcPath(dir, from).c_str(), pcPath(dir, to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static void pcRemove(intptr_t dir, const char* name) { DeleteFileA(pcPath(dir, name).c_str()); }
static unsigned pcPid() { return GetCurrentProcessId(); }
