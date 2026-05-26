/*
 * ytdlp_updater.cpp — silently keep yt-dlp.exe up to date
 *
 * On app startup we spawn a detached background thread that:
 *   1. Reads the last-seen GitHub release tag from FastPlay.ini ([YouTube] YtdlpVersion=).
 *   2. Queries https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest
 *      to learn the current tag.
 *   3. If the tag is different, downloads the new yt-dlp.exe to
 *      %LOCALAPPDATA%\MediaAccess\yt-dlp.exe (always user-writable).
 *   4. Atomically replaces the old copy (rename-on-write), then updates
 *      g_ytdlpPath and the saved tag.
 *
 * No UI, no user prompt. Failures are silent — the bundled lib\yt-dlp.exe
 * still works as a fallback. Network errors are non-fatal.
 */

#include "mediaaccess/globals.h"
#include "mediaaccess/settings.h"

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// ---- HTTP helpers ---------------------------------------------------------

bool HttpsGetText(const wchar_t* host, const wchar_t* path, std::string& outBody) {
    outBody.clear();
    HINTERNET hSession = WinHttpOpen(L"MediaAccess/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
              && WinHttpReceiveResponse(hReq, nullptr);
    if (ok) {
        DWORD avail = 0;
        char buf[8192];
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail) {
            DWORD read = 0;
            DWORD toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
            if (!WinHttpReadData(hReq, buf, toRead, &read) || read == 0) break;
            outBody.append(buf, read);
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok && !outBody.empty();
}

// Download |path| from |host| to |destFile|. Follows one HTTP 302 redirect
// (the GitHub /releases/latest/download/... URL always 302s to objects.githubusercontent.com).
bool HttpsDownloadFile(const wchar_t* host, const wchar_t* path, const std::wstring& destFile, int redirectsLeft = 3) {
    if (redirectsLeft < 0) return false;

    HINTERNET hSession = WinHttpOpen(L"MediaAccess/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // Don't auto-follow; we want to handle 302 manually so cross-host
    // redirects (github.com → objects.githubusercontent.com) work.
    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));

    bool success = false;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(hReq, nullptr))
    {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            // Follow redirect.
            wchar_t loc[2048] = {0};
            DWORD locLen = sizeof(loc);
            if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                    loc, &locLen, WINHTTP_NO_HEADER_INDEX))
            {
                URL_COMPONENTS uc = {0};
                uc.dwStructSize = sizeof(uc);
                wchar_t hostBuf[256] = {0}, pathBuf[2048] = {0};
                uc.lpszHostName = hostBuf; uc.dwHostNameLength = 256;
                uc.lpszUrlPath  = pathBuf; uc.dwUrlPathLength  = 2048;
                if (WinHttpCrackUrl(loc, 0, 0, &uc)) {
                    WinHttpCloseHandle(hReq);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    return HttpsDownloadFile(hostBuf, pathBuf, destFile, redirectsLeft - 1);
                }
            }
        } else if (status == 200) {
            // Write body to a temp file then rename.
            std::wstring tmp = destFile + L".new";
            HANDLE hFile = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                DWORD avail = 0;
                char buf[32768];
                bool failed = false;
                while (WinHttpQueryDataAvailable(hReq, &avail) && avail) {
                    DWORD read = 0;
                    DWORD toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
                    if (!WinHttpReadData(hReq, buf, toRead, &read) || read == 0) break;
                    DWORD written = 0;
                    if (!WriteFile(hFile, buf, read, &written, nullptr) || written != read) {
                        failed = true; break;
                    }
                }
                CloseHandle(hFile);
                if (!failed) {
                    // Atomic-ish replace.
                    DeleteFileW(destFile.c_str());
                    if (MoveFileW(tmp.c_str(), destFile.c_str())) success = true;
                    else DeleteFileW(tmp.c_str());
                } else {
                    DeleteFileW(tmp.c_str());
                }
            }
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return success;
}

// Pull a top-level string field out of a JSON blob. Good enough for the
// GitHub release JSON we consume (no nested objects in the value).
std::string ExtractJsonField(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

void EnsureMediaAccessAppDataDir(std::wstring& outDir) {
    wchar_t base[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
    outDir = std::wstring(base) + L"\\MediaAccess";
    CreateDirectoryW(outDir.c_str(), nullptr);  // ok if already exists
}

void UpdateThread() {
    // Give the app a chance to finish loading before we hit the network.
    Sleep(5000);

    std::string releaseJson;
    if (!HttpsGetText(L"api.github.com",
                      L"/repos/yt-dlp/yt-dlp/releases/latest", releaseJson))
        return;

    std::string latestTag = ExtractJsonField(releaseJson, "tag_name");
    if (latestTag.empty()) return;

    // Compare with the last tag we downloaded.
    wchar_t saved[64] = {0};
    GetPrivateProfileStringW(L"YouTube", L"YtdlpVersion", L"", saved, 64, g_configPath.c_str());
    std::wstring savedTag = saved;
    std::wstring latestTagW = Utf8ToWide(latestTag);

    std::wstring appDataDir;
    EnsureMediaAccessAppDataDir(appDataDir);
    std::wstring target = appDataDir + L"\\yt-dlp.exe";

    // Already at the latest version AND the file exists on disk → nothing to do.
    if (savedTag == latestTagW && PathFileExistsW(target.c_str())) return;

    if (!HttpsDownloadFile(L"github.com",
                           L"/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe",
                           target))
        return;

    // Success — point the app at the new copy and remember the version.
    g_ytdlpPath = target;
    WritePrivateProfileStringW(L"YouTube", L"YtdlpVersion", latestTagW.c_str(), g_configPath.c_str());
}

}  // namespace

// Public entry point — kicks off the silent update check on a detached thread.
void LaunchYtdlpUpdateCheck() {
    std::thread(UpdateThread).detach();
}
