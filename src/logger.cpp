// =============================================================================
// logger.cpp — append-only diagnostic log at %LOCALAPPDATA%\MediaAccess\
//              mediaaccess.log, rotated when it grows past 1 MB.
//
// Used as a low-overhead trace channel for tricky code paths (mpv DLL
// loading, BASS init failure, network errors) so we can diagnose user
// reports without asking for verbose debug dumps. CRITICAL_SECTION guards
// concurrent writes from worker threads.
// =============================================================================

#include "mediaaccess/logger.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdarg>
#include <string>

static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static std::wstring g_logPath;
// Soft cap. When exceeded the active log is renamed to .old (single
// generation kept) and a fresh file is started — total disk usage is
// bounded at ~2 MB.
static const DWORD MAX_LOG_BYTES = 1024 * 1024;  // 1 MB

static std::wstring BuildLogPath() {
    wchar_t base[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
    std::wstring dir = std::wstring(base) + L"\\MediaAccess";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\mediaaccess.log";
}

static void RotateIfTooBig() {
    HANDLE h = CreateFileW(g_logPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    if (!ok || sz.QuadPart < MAX_LOG_BYTES) return;
    std::wstring old = g_logPath + L".old";
    DeleteFileW(old.c_str());
    MoveFileW(g_logPath.c_str(), old.c_str());
}

static void TimestampPrefix(char* out, size_t n) {
    SYSTEMTIME st; GetLocalTime(&st);
    _snprintf_s(out, n, _TRUNCATE, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void WriteLine(const char* tag, const std::string& utf8Msg) {
    if (!g_logReady) return;
    EnterCriticalSection(&g_logLock);
    RotateIfTooBig();
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char ts[64]; TimestampPrefix(ts, sizeof(ts));
        std::string line = std::string(ts) + "[" + tag + "] " + utf8Msg + "\r\n";
        DWORD w = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_logLock);
}

void InitLogger() {
    InitializeCriticalSection(&g_logLock);
    g_logPath = BuildLogPath();
    g_logReady = true;
    Log("INIT", std::string("MediaAccess logger started"));
}

void FreeLogger() {
    if (!g_logReady) return;
    Log("EXIT", std::string("MediaAccess logger stopped"));
    g_logReady = false;
    DeleteCriticalSection(&g_logLock);
}

void Log(const char* tag, const std::string& msg) {
    WriteLine(tag, msg);
}

void Log(const char* tag, const std::wstring& msg) {
    if (msg.empty()) { WriteLine(tag, ""); return; }
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) { WriteLine(tag, ""); return; }
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &utf8[0], len, nullptr, nullptr);
    WriteLine(tag, utf8);
}

void LogF(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    WriteLine(tag, buf);
}
