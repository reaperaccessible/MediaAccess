/*
 * ytdlp_updater.cpp — keep yt-dlp.exe on the latest NIGHTLY build (v2.72)
 *
 * See docs/PLAN_YTDLP_NIGHTLY.md. Summary:
 *   - A check runs at every launch, and again once 12 h have passed (hourly
 *     timer + resume from sleep). One run at a time (g_state).
 *   - Source: api.github.com/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest.
 *   - A download lands in yt-dlp.exe.new and is installed ONLY after: byte
 *     count == the asset's "size", SHA-256 == its line in SHA2-256SUMS, and
 *     "yt-dlp.exe.new --version" == tag_name. The swap is rename-based
 *     (current -> .old, .new -> current). A verified .new that could not be
 *     swapped is recorded in the INI (YtdlpPendingTag / YtdlpPendingSha256)
 *     and retried later; a .new without that record is deleted.
 *   - Startup repair: current missing + .old present -> restore .old; leftover
 *     .old files (a running exe can be renamed but not deleted) are removed.
 *   - The bundled lib\yt-dlp.exe is copied to %LOCALAPPDATA% on first run so
 *     the path handed to mpv never changes.
 *   - While an update runs, user YouTube actions wait (YtdlpWaitIfUpdating):
 *     silently during the API check, then in an unclosable "please wait"
 *     window, 90 s at most. Every step is logged under [YTDLP].
 */

#include "mediaaccess/ytdlp_updater.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/settings.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/utils.h"
#include "mediaaccess/version.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

enum YtdlpState : int { kIdle = 0, kChecking, kDownloading, kVerifying, kInstalling };

// Delay before contacting GitHub so the app finishes loading first. Cut short
// as soon as a YouTube action is waiting for the check.
constexpr DWORD     kStartupGraceMs     = 5000;
constexpr DWORD     kHttpTimeoutMs      = 10000;     // per WinHTTP operation
constexpr ULONGLONG kDownloadBudgetMs   = 110000;    // whole download (< 2 min exit cap)
constexpr DWORD     kVersionTimeoutMs   = 30000;     // "--version" of the new build
constexpr ULONGLONG kRecheckIntervalMs  = 12ULL * 60 * 60 * 1000;
constexpr ULONGLONG kUiWaitBudgetMs     = 90000;     // mandatory wait window cap
constexpr ULONGLONG kCheckWaitBudgetMs  = 12000;     // silent wait during the API check
constexpr DWORD     kHttpChunk          = 32768;

const wchar_t* const kApiHost = L"api.github.com";
const wchar_t* const kApiPath = L"/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest";

std::atomic<int>       g_state{kIdle};
std::atomic<bool>      g_abort{false};
std::atomic<bool>      g_bypass{false};        // UI gate gave up: stop waiting until the run ends
std::atomic<bool>      g_waitShown{false};     // a UI wait is in progress (re-entrancy guard)
std::atomic<bool>      g_waitExit{false};      // app exit requested during a UI wait: end it, exit after
std::atomic<long long> g_bytesDone{0};
std::atomic<long long> g_bytesTotal{0};
std::atomic<ULONGLONG> g_lastSuccessTick{0};   // 0 = never succeeded this session

HANDLE  g_graceCut   = nullptr;   // manual-reset: a waiter wants the check now
HANDLE  g_workerIdle = nullptr;   // manual-reset: signalled when no worker runs
DWORD   g_mainThreadId = 0;
SRWLOCK g_pathLock = SRWLOCK_INIT;

// ---- small helpers ---------------------------------------------------------

std::wstring LocalDir() {
    wchar_t base[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
    std::wstring dir = std::wstring(base) + L"\\MediaAccess";
    CreateDirectoryW(dir.c_str(), nullptr);  // ok if it already exists
    return dir;
}
std::wstring TargetPath()  { return LocalDir() + L"\\yt-dlp.exe"; }
std::wstring NewPath()     { return TargetPath() + L".new"; }

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring IniGet(const wchar_t* key) {
    wchar_t buf[256] = {0};
    GetPrivateProfileStringW(L"YouTube", key, L"", buf, 256, g_configPath.c_str());
    return buf;
}
void IniSet(const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(L"YouTube", key, value.empty() ? nullptr : value.c_str(),
                               g_configPath.c_str());
}

void Trim(std::string& s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    s.erase(0, i);
}

// ---- minimal JSON walker (just enough for the GitHub release object) -------

size_t SkipWs(const std::string& s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}

// s[i] == '"'. Returns the index just past the closing quote (npos on error).
size_t SkipString(const std::string& s, size_t i) {
    for (++i; i < s.size(); ++i) {
        if (s[i] == '\\') { ++i; continue; }
        if (s[i] == '"') return i + 1;
    }
    return std::string::npos;
}

// Skip one JSON value starting at i (after whitespace). Returns the index past it.
size_t SkipValue(const std::string& s, size_t i) {
    i = SkipWs(s, i);
    if (i >= s.size()) return std::string::npos;
    if (s[i] == '"') return SkipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') { i = SkipString(s, i); if (i == std::string::npos) return i; continue; }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') { if (--depth == 0) return i + 1; }
            ++i;
        }
        return std::string::npos;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') ++i;
    return i;
}

// Decode a JSON string token (with its quotes). Only the escapes GitHub uses
// in tags, names and URLs matter here.
std::string Unquote(const std::string& tok) {
    if (tok.size() < 2 || tok.front() != '"') return tok;
    std::string out;
    for (size_t i = 1; i + 1 < tok.size(); ++i) {
        if (tok[i] == '\\' && i + 2 < tok.size()) { out += tok[++i]; continue; }
        out += tok[i];
    }
    return out;
}

// Find member |key| of the object that starts at objPos ('{'). Only direct
// members are considered — nested objects are skipped whole.
bool JsonMember(const std::string& s, size_t objPos, const char* key,
                size_t& valStart, size_t& valEnd) {
    if (objPos >= s.size() || s[objPos] != '{') return false;
    size_t i = SkipWs(s, objPos + 1);
    while (i < s.size() && s[i] == '"') {
        size_t keyEnd = SkipString(s, i);
        if (keyEnd == std::string::npos) return false;
        std::string k = Unquote(s.substr(i, keyEnd - i));
        i = SkipWs(s, keyEnd);
        if (i >= s.size() || s[i] != ':') return false;
        size_t vs = SkipWs(s, i + 1);
        size_t ve = SkipValue(s, vs);
        if (ve == std::string::npos) return false;
        if (k == key) { valStart = vs; valEnd = ve; return true; }
        i = SkipWs(s, ve);
        if (i < s.size() && s[i] == ',') { i = SkipWs(s, i + 1); continue; }
        break;
    }
    return false;
}

std::string JsonMemberText(const std::string& s, size_t objPos, const char* key) {
    size_t vs = 0, ve = 0;
    if (!JsonMember(s, objPos, key, vs, ve)) return "";
    return Unquote(s.substr(vs, ve - vs));
}

struct ReleaseInfo {
    std::string tag;
    std::string exeUrl;
    long long   exeSize = 0;
    std::string sumsUrl;
};

bool ParseRelease(const std::string& json, ReleaseInfo& out) {
    size_t root = SkipWs(json, 0);
    out.tag = JsonMemberText(json, root, "tag_name");
    size_t as = 0, ae = 0;
    if (out.tag.empty() || !JsonMember(json, root, "assets", as, ae) || json[as] != '[')
        return false;
    size_t i = SkipWs(json, as + 1);
    while (i < ae && json[i] == '{') {
        size_t objEnd = SkipValue(json, i);
        if (objEnd == std::string::npos) break;
        std::string name = JsonMemberText(json, i, "name");
        if (name == "yt-dlp.exe") {
            out.exeUrl  = JsonMemberText(json, i, "browser_download_url");
            out.exeSize = _atoi64(JsonMemberText(json, i, "size").c_str());
        } else if (name == "SHA2-256SUMS") {
            out.sumsUrl = JsonMemberText(json, i, "browser_download_url");
        }
        i = SkipWs(json, objEnd);
        if (i < ae && json[i] == ',') i = SkipWs(json, i + 1);
        else break;
    }
    return !out.exeUrl.empty() && out.exeSize > 0 && !out.sumsUrl.empty();
}

// ---- HTTP -------------------------------------------------------------------

struct HttpHandles {
    HINTERNET session = nullptr, connect = nullptr, request = nullptr;
    ~HttpHandles() {
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (session) WinHttpCloseHandle(session);
    }
};

// Open a GET on an https URL and receive the response headers. WinHTTP follows
// redirects itself (github.com -> release-assets.githubusercontent.com).
bool HttpOpen(const std::wstring& url, const wchar_t* extraHeaders, HttpHandles& h, DWORD& status) {
    URL_COMPONENTS uc = {0};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {0};
    std::vector<wchar_t> pathBuf(4096, 0);
    uc.lpszHostName = hostBuf;        uc.dwHostNameLength = 256;
    uc.lpszUrlPath  = pathBuf.data(); uc.dwUrlPathLength  = static_cast<DWORD>(pathBuf.size());
    wchar_t extraBuf[2048] = {0};
    uc.lpszExtraInfo = extraBuf;      uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    std::wstring path = std::wstring(pathBuf.data()) + extraBuf;

    std::wstring ua = std::wstring(L"MediaAccess/") + Utf8ToWide(APP_VERSION);
    h.session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.session) return false;
    WinHttpSetTimeouts(h.session, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs, kHttpTimeoutMs);
    h.connect = WinHttpConnect(h.session, hostBuf, uc.nPort, 0);
    if (!h.connect) return false;
    h.request = WinHttpOpenRequest(h.connect, L"GET", path.c_str(), nullptr,
                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   WINHTTP_FLAG_SECURE);
    if (!h.request) return false;
    if (!WinHttpSendRequest(h.request, extraHeaders ? extraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            extraHeaders ? static_cast<DWORD>(-1L) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h.request, nullptr))
        return false;
    status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(h.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    return true;
}

// GET a small text body. Any read error = failure (no silent truncation).
bool HttpGetText(const std::wstring& url, const wchar_t* extraHeaders, std::string& body) {
    body.clear();
    HttpHandles h;
    DWORD status = 0;
    if (!HttpOpen(url, extraHeaders, h, status)) {
        LogF("YTDLP", "HTTP request failed (%lu)", (unsigned long)GetLastError());
        return false;
    }
    if (status != 200) { LogF("YTDLP", "HTTP status %lu", (unsigned long)status); return false; }
    char buf[8192];
    for (;;) {
        if (g_abort.load()) return false;
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail)) return false;
        if (avail == 0) break;  // end of body
        DWORD toRead = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
        DWORD read = 0;
        if (!WinHttpReadData(h.request, buf, toRead, &read) || read == 0) return false;
        body.append(buf, read);
    }
    return !body.empty();
}

// Download to |dest|. Fails on ANY read/write error, on abort, or when the
// whole transfer exceeds kDownloadBudgetMs. Progress goes to g_bytesDone.
bool HttpDownload(const std::wstring& url, const std::wstring& dest, long long& written) {
    written = 0;
    HttpHandles h;
    DWORD status = 0;
    if (!HttpOpen(url, nullptr, h, status)) {
        LogF("YTDLP", "download: request failed (%lu)", (unsigned long)GetLastError());
        return false;
    }
    if (status != 200) { LogF("YTDLP", "download: HTTP status %lu", (unsigned long)status); return false; }

    HANDLE f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LogF("YTDLP", "download: cannot create file (%lu)", (unsigned long)GetLastError());
        return false;
    }
    ULONGLONG start = GetTickCount64();
    std::vector<char> buf(kHttpChunk);
    bool ok = true;
    for (;;) {
        if (g_abort.load()) { Log("YTDLP", std::string("download: aborted")); ok = false; break; }
        if (GetTickCount64() - start > kDownloadBudgetMs) {
            Log("YTDLP", std::string("download: time budget exceeded"));
            ok = false; break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(h.request, &avail)) {
            LogF("YTDLP", "download: read error (%lu)", (unsigned long)GetLastError());
            ok = false; break;
        }
        if (avail == 0) break;  // end of body
        DWORD toRead = (avail < buf.size()) ? avail : static_cast<DWORD>(buf.size());
        DWORD read = 0;
        if (!WinHttpReadData(h.request, buf.data(), toRead, &read) || read == 0) {
            LogF("YTDLP", "download: read error (%lu)", (unsigned long)GetLastError());
            ok = false; break;
        }
        DWORD w = 0;
        if (!WriteFile(f, buf.data(), read, &w, nullptr) || w != read) {
            LogF("YTDLP", "download: write error (%lu)", (unsigned long)GetLastError());
            ok = false; break;
        }
        written += read;
        g_bytesDone.store(written);
    }
    if (!FlushFileBuffers(f)) ok = false;
    CloseHandle(f);
    return ok;
}

// ---- verification -----------------------------------------------------------

// Lower-case hex SHA-256 of a file ("" on error).
std::string Sha256File(const std::wstring& path) {
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 16);
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr)) { ok = false; break; }
            if (read == 0) break;
            if (BCryptHashData(hash, buf.data(), read, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* d = "0123456789abcdef";
            for (unsigned char b : digest) { hex += d[b >> 4]; hex += d[b & 15]; }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

// Find the "<hex>  yt-dlp.exe" line of SHA2-256SUMS.
std::string ExpectedSha(const std::string& sums) {
    size_t pos = 0;
    while (pos < sums.size()) {
        size_t eol = sums.find('\n', pos);
        std::string line = sums.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? sums.size() : eol + 1;
        Trim(line);
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) continue;
        std::string hex = line.substr(0, sp);
        std::string name = line.substr(sp);
        Trim(name);
        if (!name.empty() && name[0] == '*') name.erase(0, 1);  // binary-mode marker
        if (name == "yt-dlp.exe" && hex.size() == 64) {
            for (auto& c : hex) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            return hex;
        }
    }
    return "";
}

// Run "<exe> --version" hidden; returns its trimmed stdout ("" on failure/timeout).
std::string RunVersion(const std::wstring& exe) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\" --version";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    BOOL started = CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!started) {
        LogF("YTDLP", "--version: cannot start (%lu)", (unsigned long)GetLastError());
        CloseHandle(rd);
        return "";
    }
    std::string out;
    ULONGLONG start = GetTickCount64();
    bool timedOut = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char buf[512];
            DWORD read = 0;
            if (ReadFile(rd, buf, sizeof(buf), &read, nullptr) && read > 0) out.append(buf, read);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            // Drain what is left.
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                char buf[512];
                DWORD read = 0;
                if (!ReadFile(rd, buf, sizeof(buf), &read, nullptr) || read == 0) break;
                out.append(buf, read);
            }
            break;
        }
        if (GetTickCount64() - start > kVersionTimeoutMs || g_abort.load()) { timedOut = true; break; }
    }
    if (timedOut) {
        TerminateProcess(pi.hProcess, 1);
        Log("YTDLP", std::string("--version: timed out"));
        out.clear();
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    Trim(out);
    return out;
}

// ---- file swap --------------------------------------------------------------

// Delete every leftover yt-dlp.exe.old* (a running exe can be renamed but not
// deleted, so an .old may survive until the next run).
void DeleteOldCopies() {
    std::wstring dir = LocalDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\yt-dlp.exe.old*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (DeleteFileW(p.c_str())) Log("YTDLP", L"removed " + std::wstring(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// current -> .old (unique name if an .old is still locked), .new -> current.
// Never interrupted by g_abort. Rolls the old copy back if the second rename fails.
bool SwapInNew() {
    std::wstring target = TargetPath();
    std::wstring fresh = NewPath();
    std::wstring old;
    if (FileExists(target)) {
        old = target + L".old";
        if (FileExists(old) && !DeleteFileW(old.c_str()))
            old = target + L".old" + std::to_wstring(GetTickCount64());
        if (!MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            LogF("YTDLP", "swap: cannot move the current copy aside (%lu)", (unsigned long)GetLastError());
            return false;
        }
    }
    if (!MoveFileExW(fresh.c_str(), target.c_str(), 0)) {
        LogF("YTDLP", "swap: cannot move the new copy in (%lu)", (unsigned long)GetLastError());
        if (!old.empty()) MoveFileExW(old.c_str(), target.c_str(), 0);  // roll back
        return false;
    }
    if (!old.empty() && !DeleteFileW(old.c_str()))
        Log("YTDLP", std::string("swap: old copy still in use, removed at next launch"));
    return true;
}

void ClearPending() {
    IniSet(L"YtdlpPendingTag", L"");
    IniSet(L"YtdlpPendingSha256", L"");
}

// Install a verified .new left by an earlier run. A .new without a matching
// record in the INI is never trusted and is deleted.
void TryInstallPending() {
    std::wstring fresh = NewPath();
    if (!FileExists(fresh)) { ClearPending(); return; }
    std::wstring tag = IniGet(L"YtdlpPendingTag");
    std::wstring sha = IniGet(L"YtdlpPendingSha256");
    if (tag.empty() || sha.empty() || Utf8ToWide(Sha256File(fresh)) != sha) {
        Log("YTDLP", std::string("pending: unverified yt-dlp.exe.new deleted"));
        DeleteFileW(fresh.c_str());
        ClearPending();
        return;
    }
    if (SwapInNew()) {
        IniSet(L"YtdlpVersion", tag);
        ClearPending();
        Log("YTDLP", L"pending: installed " + tag);
    } else {
        Log("YTDLP", std::string("pending: swap failed again, kept for a later try"));
    }
}

// ---- worker -----------------------------------------------------------------

void FinishRun(bool pathChanged) {
    g_bytesDone.store(0);
    g_bytesTotal.store(0);
    g_bypass.store(false);
    SetEvent(g_workerIdle);   // before Idle: a new run started after this re-arms it
    g_state.store(kIdle);
    if (g_hwnd) PostMessageW(g_hwnd, WM_YTDLP_UPDATE_DONE, pathChanged ? 1 : 0, 0);
}

void UpdateThread() {
    bool pathChanged = false;
    std::wstring fresh = NewPath();
    std::wstring target = TargetPath();

    // Sleep through the startup grace unless a waiter cuts it short.
    WaitForSingleObject(g_graceCut, kStartupGraceMs);
    if (g_abort.load()) { Log("YTDLP", std::string("check aborted")); FinishRun(false); return; }

    TryInstallPending();

    std::string json;
    if (!HttpGetText(std::wstring(L"https://") + kApiHost + kApiPath,
                     L"Accept: application/vnd.github+json\r\n", json)) {
        Log("YTDLP", std::string("check failed (no network or GitHub unreachable)"));
        FinishRun(false);
        return;
    }
    ReleaseInfo rel;
    if (!ParseRelease(json, rel)) {
        Log("YTDLP", std::string("check failed: release JSON not understood"));
        FinishRun(false);
        return;
    }
    std::wstring tagW = Utf8ToWide(rel.tag);
    std::wstring installed = IniGet(L"YtdlpVersion");
    Log("YTDLP", L"installed " + (installed.empty() ? std::wstring(L"(unknown)") : installed) +
                 L", latest nightly " + tagW);
    // Version unknown (fresh install seeded from lib\, or an older MediaAccess):
    // ask the file itself before downloading the same build again.
    if (installed.empty() && FileExists(target) && RunVersion(target) == rel.tag) {
        IniSet(L"YtdlpVersion", tagW);
        installed = tagW;
        Log("YTDLP", std::string("installed copy identified as the latest nightly"));
    }
    if (installed == tagW && FileExists(target)) {
        g_lastSuccessTick.store(GetTickCount64());
        Log("YTDLP", std::string("up to date"));
        FinishRun(false);
        return;
    }

    // A verified .new for this very tag may be waiting for its swap.
    if (IniGet(L"YtdlpPendingTag") == tagW && FileExists(fresh)) {
        Log("YTDLP", std::string("verified download already present, retrying the swap"));
    } else {
        ClearPending();
        g_bytesTotal.store(rel.exeSize);
        g_bytesDone.store(0);
        g_state.store(kDownloading);
        LogF("YTDLP", "download started (%lld bytes)", rel.exeSize);
        long long written = 0;
        bool ok = HttpDownload(Utf8ToWide(rel.exeUrl), fresh, written);
        LogF("YTDLP", "download %s (%lld bytes)", ok ? "finished" : "failed", written);

        if (ok) {
            g_state.store(kVerifying);
            if (written != rel.exeSize) {
                LogF("YTDLP", "size check FAILED (%lld, expected %lld)", written, rel.exeSize);
                ok = false;
            } else {
                Log("YTDLP", std::string("size check ok"));
            }
        }
        std::string sha;
        if (ok && !g_abort.load()) {
            std::string sums;
            std::string expected;
            if (HttpGetText(Utf8ToWide(rel.sumsUrl), nullptr, sums)) expected = ExpectedSha(sums);
            sha = Sha256File(fresh);
            if (expected.empty() || sha != expected) {
                Log("YTDLP", "SHA-256 check FAILED (got " + sha + ", expected " +
                             (expected.empty() ? std::string("(unavailable)") : expected) + ")");
                ok = false;
            } else {
                Log("YTDLP", std::string("SHA-256 check ok"));
            }
        } else if (ok) {
            ok = false;
        }
        if (ok && !g_abort.load()) {
            std::string ver = RunVersion(fresh);
            if (ver != rel.tag) {
                Log("YTDLP", "--version check FAILED (\"" + ver + "\")");
                ok = false;
            } else {
                Log("YTDLP", std::string("--version check ok"));
            }
        } else if (ok) {
            ok = false;
        }
        if (!ok) {
            if (g_abort.load()) Log("YTDLP", std::string("update aborted"));
            DeleteFileW(fresh.c_str());
            FinishRun(false);
            return;
        }
        IniSet(L"YtdlpPendingTag", tagW);
        IniSet(L"YtdlpPendingSha256", Utf8ToWide(sha));
    }

    // Swap — not interruptible (two renames, a few milliseconds).
    g_state.store(kInstalling);
    if (!SwapInNew()) {
        Log("YTDLP", std::string("install failed, verified download kept for a later try"));
        FinishRun(false);
        return;
    }
    IniSet(L"YtdlpVersion", tagW);
    ClearPending();
    g_lastSuccessTick.store(GetTickCount64());
    Log("YTDLP", L"installed " + tagW);

    if (GetYtdlpPath() != target) {
        SetYtdlpPath(target);   // before signalling the end, so waiters see it
        pathChanged = true;
        Log("YTDLP", L"path now " + target);
    }
    FinishRun(pathChanged);
}

// ---- mandatory wait window --------------------------------------------------

constexpr int  kIdcWaitText = 100;
constexpr UINT kWaitTimerId = 1;

struct WaitCtx {
    ULONGLONG deadline = 0;
    int lastSpokenStep = 0;   // 25 / 50 / 75 already announced
    WNDPROC oldEditProc = nullptr;
    bool timedOut = false;
};

std::wstring ProgressText() {
    int st = g_state.load();
    if (st == kVerifying || st == kInstalling) return T("Installing the update");
    long long total = g_bytesTotal.load();
    int pct = total > 0 ? static_cast<int>(g_bytesDone.load() * 100 / total) : 0;
    if (pct > 100) pct = 100;
    return std::wstring(T("Update")) + L" " + std::to_wstring(pct) + L" %";
}

LRESULT CALLBACK WaitEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WaitCtx* ctx = reinterpret_cast<WaitCtx*>(GetWindowLongPtrW(GetParent(hwnd), DWLP_USER));
    switch (msg) {
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;   // Esc/Enter/Tab come here, not to the dialog
        case WM_KEYDOWN:
            if (wParam != VK_SHIFT && wParam != VK_CONTROL && wParam != VK_MENU &&
                wParam != VK_LWIN && wParam != VK_RWIN && wParam != VK_CAPITAL) {
                SpeakW(ProgressText(), true);
            }
            return 0;
        case WM_CHAR:
        case WM_KEYUP:
            return 0;
    }
    return CallWindowProcW(ctx ? ctx->oldEditProc : DefWindowProcW, hwnd, msg, wParam, lParam);
}

INT_PTR CALLBACK WaitDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    WaitCtx* ctx = reinterpret_cast<WaitCtx*>(GetWindowLongPtrW(hDlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            ctx = reinterpret_cast<WaitCtx*>(lParam);
            SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(ctx));
            SetWindowTextW(hDlg, T("YouTube update"));
            HWND edit = GetDlgItem(hDlg, kIdcWaitText);
            SetWindowTextW(edit, T("Please wait, a mandatory YouTube update is in progress"));
            ctx->oldEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WaitEditProc)));
            SetTimer(hDlg, kWaitTimerId, 250, nullptr);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, 0);
            return FALSE;  // focus set by hand
        }
        case WM_TIMER:
            if (wParam == kWaitTimerId && ctx) {
                if (g_state.load() == kIdle || g_waitExit.load()) { EndDialog(hDlg, 1); return TRUE; }
                if (GetTickCount64() >= ctx->deadline) {
                    ctx->timedOut = true;
                    EndDialog(hDlg, 0);
                    return TRUE;
                }
                long long total = g_bytesTotal.load();
                if (g_state.load() == kDownloading && total > 0) {
                    int pct = static_cast<int>(g_bytesDone.load() * 100 / total);
                    int step = (pct >= 75) ? 75 : (pct >= 50) ? 50 : (pct >= 25) ? 25 : 0;
                    if (step > ctx->lastSpokenStep) {
                        ctx->lastSpokenStep = step;
                        SpeakW(std::wstring(T("Update")) + L" " + std::to_wstring(step) + L" %", false);
                    }
                }
            }
            return TRUE;
        case WM_COMMAND:
            // IDOK / IDCANCEL (Enter, Esc via the dialog manager): never close.
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                SpeakW(ProgressText(), true);
                return TRUE;
            }
            break;
        case WM_CLOSE:          // Alt+F4 — ignored
            SpeakW(ProgressText(), true);
            return TRUE;
        case WM_DESTROY:
            KillTimer(hDlg, kWaitTimerId);
            if (ctx && ctx->oldEditProc)
                SetWindowLongPtrW(GetDlgItem(hDlg, kIdcWaitText), GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(ctx->oldEditProc));
            break;
    }
    return FALSE;
}

// In-memory dialog template: a caption without a close box, and one read-only
// edit holding the message (focused, so the screen reader reads it at once).
std::vector<WORD> BuildWaitTemplate() {
    std::vector<WORD> t;
    auto dword = [&](DWORD v) { t.push_back(LOWORD(v)); t.push_back(HIWORD(v)); };
    auto str = [&](const wchar_t* s) { while (*s) t.push_back(*s++); t.push_back(0); };
    auto align = [&]() { if (t.size() % 2) t.push_back(0); };

    dword(WS_POPUP | WS_CAPTION | DS_MODALFRAME | DS_CENTER | DS_SETFONT);  // style
    dword(0);                    // exStyle
    t.push_back(1);              // cdit
    t.push_back(0); t.push_back(0);        // x, y
    t.push_back(260); t.push_back(40);     // cx, cy
    t.push_back(0);              // no menu
    t.push_back(0);              // default class
    str(L"YouTube");             // caption (localized in WM_INITDIALOG)
    t.push_back(9);              // font size
    str(L"Segoe UI");

    align();
    dword(WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL);
    dword(0);
    t.push_back(8); t.push_back(12);       // x, y
    t.push_back(244); t.push_back(14);     // cx, cy
    t.push_back(static_cast<WORD>(kIdcWaitText));
    t.push_back(0xFFFF); t.push_back(0x0081);  // EDIT class
    str(L"");                    // text (set in WM_INITDIALOG)
    t.push_back(0);              // no creation data
    return t;
}

// Pump non-input messages while |cond| holds (the silent wait). Keyboard and
// mouse input is dropped so nothing else starts meanwhile.
template <typename Cond>
void PumpWhile(Cond cond) {
    while (cond()) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) { PostQuitMessage(static_cast<int>(m.wParam)); return; }
            if ((m.message >= WM_KEYFIRST && m.message <= WM_KEYLAST) ||
                (m.message >= WM_MOUSEFIRST && m.message <= WM_MOUSELAST))
                continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

void YtdlpInit() {
    g_mainThreadId = GetCurrentThreadId();
    if (!g_graceCut)   g_graceCut   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_workerIdle) g_workerIdle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
}

std::wstring GetYtdlpPath() {
    AcquireSRWLockShared(&g_pathLock);
    std::wstring copy = g_ytdlpPath;
    ReleaseSRWLockShared(&g_pathLock);
    return copy;
}

void SetYtdlpPath(const std::wstring& path) {
    AcquireSRWLockExclusive(&g_pathLock);
    g_ytdlpPath = path;
    ReleaseSRWLockExclusive(&g_pathLock);
}

void YtdlpPrepareAtStartup(const std::wstring& appDir) {
    std::wstring target = TargetPath();
    // Repair a swap cut in the middle (current renamed away, new not in yet).
    if (!FileExists(target)) {
        std::wstring dir = LocalDir();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\yt-dlp.exe.old*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring old = dir + L"\\" + fd.cFileName;
            FindClose(h);
            if (MoveFileExW(old.c_str(), target.c_str(), 0))
                Log("YTDLP", L"repair: restored " + std::wstring(fd.cFileName));
        }
    }
    DeleteOldCopies();
    TryInstallPending();

    // Seed the local copy from the bundled one so the path never changes.
    if (!FileExists(target)) {
        std::wstring bundled = appDir + L"lib\\yt-dlp.exe";
        if (FileExists(bundled)) {
            if (CopyFileW(bundled.c_str(), target.c_str(), TRUE)) {
                IniSet(L"YtdlpVersion", L"");   // unknown build: the check will replace it
                Log("YTDLP", std::string("seeded the local copy from lib\\yt-dlp.exe"));
            } else {
                LogF("YTDLP", "seed copy failed (%lu)", (unsigned long)GetLastError());
            }
        }
    }
}

void LaunchYtdlpUpdateCheck() {
    int expected = kIdle;
    if (!g_state.compare_exchange_strong(expected, kChecking)) return;  // one at a time
    g_abort.store(false);
    g_bypass.store(false);
    ResetEvent(g_graceCut);
    ResetEvent(g_workerIdle);
    Log("YTDLP", std::string("check started"));
    std::thread(UpdateThread).detach();
}

void YtdlpMaybeRecheck(bool force) {
    ULONGLONG last = g_lastSuccessTick.load();
    if (force || last == 0 || GetTickCount64() - last >= kRecheckIntervalMs)
        LaunchYtdlpUpdateCheck();
}

bool YtdlpUpdateBusy() {
    return g_state.load() != kIdle;
}

bool YtdlpWaitShown() {
    return g_waitShown.load();
}

void YtdlpEndWaitForExit() {
    if (g_waitShown.load()) {
        Log("YTDLP", std::string("exit requested during the wait"));
        g_waitExit.store(true);
    }
}

bool YtdlpInstallRunning() {
    int st = g_state.load();
    return st == kDownloading || st == kVerifying || st == kInstalling;
}

bool YtdlpAbortRequested() {
    return g_abort.load();
}

void YtdlpWaitSilentlyIfUpdating() {
    if (GetCurrentThreadId() == g_mainThreadId) return;  // UI paths are gated at their entry
    if (g_state.load() == kIdle || g_bypass.load()) return;
    SetEvent(g_graceCut);
    Log("YTDLP", std::string("background task waiting for the update"));
    ULONGLONG start = GetTickCount64();
    while (g_state.load() != kIdle && !g_bypass.load() &&
           GetTickCount64() - start < kUiWaitBudgetMs)
        Sleep(200);
}

bool YtdlpWaitIfUpdating(HWND owner) {
    if (GetCurrentThreadId() != g_mainThreadId) {
        YtdlpWaitSilentlyIfUpdating();
        return true;
    }
    if (g_waitShown.load()) {
        Log("YTDLP", std::string("YouTube action ignored: already waiting for the update"));
        return false;
    }
    if (g_state.load() == kIdle || g_bypass.load()) return true;

    g_waitShown.store(true);
    SetEvent(g_graceCut);   // the waiter wants the check now, not after the grace
    HWND prevFocus = GetFocus();
    ULONGLONG start = GetTickCount64();

    // 1) API check: wait silently (normally well under a second).
    if (g_state.load() == kChecking) {
        Log("YTDLP", std::string("YouTube action waiting silently for the check"));
        PumpWhile([&] {
            return g_state.load() == kChecking && !g_waitExit.load() &&
                   GetTickCount64() - start < kCheckWaitBudgetMs;
        });
    }

    // 2) A download or install is running: the mandatory wait window.
    bool timedOut = false;
    int st = g_state.load();
    if (st == kDownloading || st == kVerifying || st == kInstalling) {
        Log("YTDLP", std::string("mandatory wait window shown"));
        WaitCtx ctx;
        ctx.deadline = start + kUiWaitBudgetMs;
        std::vector<WORD> tmpl = BuildWaitTemplate();
        if (!owner || !IsWindow(owner)) owner = GetMessageBoxOwner();
        DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
                                owner, WaitDlgProc, reinterpret_cast<LPARAM>(&ctx));
        timedOut = ctx.timedOut;
    } else if (st == kChecking) {
        timedOut = true;   // the check itself outlived its budget
    }

    if (g_waitExit.exchange(false) || g_isShuttingDown || !IsWindow(g_hwnd)) {
        // The user closed MediaAccess meanwhile: drop the YouTube action and
        // run the exit now that no wait loop is on the stack.
        Log("YTDLP", std::string("wait ended by an exit request, action dropped"));
        g_waitShown.store(false);
        if (!g_isShuttingDown && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        return false;
    }
    if (timedOut && g_state.load() != kIdle) {
        g_bypass.store(true);   // stop every other waiter until this run ends
        Log("YTDLP", std::string("wait cap reached, continuing with the current version"));
        Speak(Ts("The YouTube update could not finish; the current version is used"));
    } else {
        Log("YTDLP", std::string("wait over"));
    }
    if (prevFocus && IsWindow(prevFocus)) SetFocus(prevFocus);
    g_waitShown.store(false);
    return true;
}

void YtdlpAbortAndWait(DWORD ms) {
    if (g_state.load() == kIdle) return;
    Log("YTDLP", std::string("abort requested"));
    g_abort.store(true);
    SetEvent(g_graceCut);
    if (g_workerIdle) WaitForSingleObject(g_workerIdle, ms);
}
