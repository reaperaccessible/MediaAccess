#include "updater.h"
#include "version.h"
#include "globals.h"
#include "accessibility.h"
#include "translations.h"
#include "resource.h"
#include <winhttp.h>
#include <shlobj.h>
#include <commctrl.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <regex>
#include <algorithm>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comctl32.lib")
// TaskDialogIndirect requires ComCtl32 v6. Embed the manifest dependency.
#pragma comment(linker, "/manifestdependency:\"type='win32' " \
    "name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

// Parse a dotted version string ("1.0.24", "1.24", "v2.0.3-beta") into a
// vector of unsigned ints. Non-numeric trailing junk is ignored. Missing
// components are treated as 0, so "1.24" == "1.24.0" == "1.24.0.0".
static std::vector<unsigned> ParseVersionParts(const std::string& s) {
    std::vector<unsigned> parts;
    size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    while (i < s.size()) {
        unsigned n = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + unsigned(s[i] - '0');
            ++i;
            any = true;
        }
        if (!any) break;
        parts.push_back(n);
        if (i < s.size() && s[i] == '.') { ++i; continue; }
        break;
    }
    return parts;
}

// Returns true iff `remote` is strictly newer than `local`.
// Pads the shorter version with zeros so "1.24" vs "1.0.23" compares correctly:
// {1,24,0} vs {1,0,23} -> first 1==1, second 24>0 -> remote newer.
static bool IsRemoteNewer(const std::string& remote, const std::string& local) {
    auto r = ParseVersionParts(remote);
    auto l = ParseVersionParts(local);
    size_t n = std::max(r.size(), l.size());
    r.resize(n, 0);
    l.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (r[i] > l[i]) return true;
        if (r[i] < l[i]) return false;
    }
    return false;
}

// Simple JSON value extraction (no external library needed)
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";

    size_t startQuote = json.find('"', colonPos + 1);
    if (startQuote == std::string::npos) return "";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == '"' && json[endQuote - 1] != '\\') break;
        endQuote++;
    }

    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

// Extract first object from JSON array
static std::string ExtractFirstArrayObject(const std::string& json) {
    size_t start = json.find('[');
    if (start == std::string::npos) return "";

    size_t objStart = json.find('{', start);
    if (objStart == std::string::npos) return "";

    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < json.length() && depth > 0) {
        if (json[objEnd] == '{') depth++;
        else if (json[objEnd] == '}') depth--;
        objEnd++;
    }

    return json.substr(objStart, objEnd - objStart);
}

// Convert string to lowercase
static std::string ToLower(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = (char)tolower((unsigned char)c);
    }
    return result;
}

// Asset URLs for Windows
struct WindowsAssets {
    std::string zipUrl;
    std::string installerUrl;
};

// Find Windows assets in release (both zip and installer)
static WindowsAssets FindWindowsAssets(const std::string& releaseJson) {
    WindowsAssets assets;

    size_t assetsPos = releaseJson.find("\"assets\"");
    if (assetsPos == std::string::npos) return assets;

    size_t arrayStart = releaseJson.find('[', assetsPos);
    if (arrayStart == std::string::npos) return assets;

    int depth = 1;
    size_t arrayEnd = arrayStart + 1;
    while (arrayEnd < releaseJson.length() && depth > 0) {
        if (releaseJson[arrayEnd] == '[') depth++;
        else if (releaseJson[arrayEnd] == ']') depth--;
        arrayEnd++;
    }

    std::string assetsArray = releaseJson.substr(arrayStart, arrayEnd - arrayStart);

    std::string fallbackZipUrl;

    size_t pos = 0;
    while (pos < assetsArray.length()) {
        size_t objStart = assetsArray.find('{', pos);
        if (objStart == std::string::npos) break;

        int objDepth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < assetsArray.length() && objDepth > 0) {
            if (assetsArray[objEnd] == '{') objDepth++;
            else if (assetsArray[objEnd] == '}') objDepth--;
            objEnd++;
        }

        std::string asset = assetsArray.substr(objStart, objEnd - objStart);
        std::string name = ExtractJsonString(asset, "name");
        std::string nameLower = ToLower(name);
        std::string url = ExtractJsonString(asset, "browser_download_url");

        // Skip non-Windows platforms
        if (nameLower.find("linux") != std::string::npos ||
            nameLower.find("macos") != std::string::npos ||
            nameLower.find("darwin") != std::string::npos ||
            nameLower.find("mac-") != std::string::npos ||
            nameLower.find("-mac") != std::string::npos) {
            pos = objEnd;
            continue;
        }

        // Installer exe (Setup.exe, Installer.exe, etc.)
        if ((nameLower.find("setup") != std::string::npos ||
             nameLower.find("installer") != std::string::npos) &&
            nameLower.find(".exe") != std::string::npos) {
            assets.installerUrl = url;
        }
        // Zip file
        else if (nameLower.find(".zip") != std::string::npos) {
            if (nameLower.find("windows") != std::string::npos ||
                nameLower.find("win64") != std::string::npos ||
                nameLower.find("win32") != std::string::npos ||
                nameLower.find("win-") != std::string::npos ||
                nameLower.find("-win") != std::string::npos) {
                assets.zipUrl = url;
            } else if (fallbackZipUrl.empty()) {
                fallbackZipUrl = url;
            }
        }

        pos = objEnd;
    }

    if (assets.zipUrl.empty() && !fallbackZipUrl.empty()) {
        assets.zipUrl = fallbackZipUrl;
    }

    return assets;
}

// HTTP GET request using WinHTTP
static std::string HttpGet(const std::wstring& host, const std::wstring& path, bool https = true) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"MediaAccess/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return "";

    // Enable TLS 1.2 (required for GitHub API)
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // GitHub API requires a User-Agent header
    WinHttpAddRequestHeaders(hRequest,
        L"Accept: application/vnd.github.v3+json\r\nUser-Agent: MediaAccess/1.0",
        -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        DWORD bytesAvailable;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1);
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                buffer[bytesRead] = 0;
                result += buffer.data();
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

// Download file with progress callback
static bool HttpDownload(const std::string& url, const std::wstring& destPath,
                         DownloadProgressCallback progressCallback, int maxRedirects = 5) {
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);

    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        return false;
    }

    bool https = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"MediaAccess/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool success = false;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        // Check for redirect
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode >= 300 && statusCode < 400) {
            if (maxRedirects <= 0) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return false;  // Too many redirects
            }
            wchar_t redirectUrl[2048] = {0};
            DWORD redirectUrlSize = sizeof(redirectUrl);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, redirectUrl, &redirectUrlSize, WINHTTP_NO_HEADER_INDEX)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);

                std::wstring wRedirect(redirectUrl);
                std::string redirect(wRedirect.begin(), wRedirect.end());
                return HttpDownload(redirect, destPath, progressCallback, maxRedirects - 1);
            }
        }

        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX);

        std::ofstream outFile(destPath, std::ios::binary);
        if (!outFile) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        size_t totalDownloaded = 0;
        DWORD bytesAvailable;
        std::vector<char> buffer(65536);

        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            DWORD toRead = (bytesAvailable < (DWORD)buffer.size()) ? bytesAvailable : (DWORD)buffer.size();
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead)) {
                outFile.write(buffer.data(), bytesRead);
                totalDownloaded += bytesRead;

                if (progressCallback) {
                    if (!progressCallback(totalDownloaded, contentLength)) {
                        outFile.close();
                        DeleteFileW(destPath.c_str());
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return false;
                    }
                }
            }
        }

        outFile.close();
        success = (totalDownloaded > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

// Generate a random suffix for temp filenames to prevent predictable path attacks
static std::wstring GetRandomSuffix() {
    ULONGLONG tick = GetTickCount64();
    DWORD pid = GetCurrentProcessId();
    wchar_t buf[32];
    swprintf(buf, 32, L"%llx%x", tick, pid);
    return buf;
}

// Cached random suffix (consistent within a single update session)
static std::wstring g_updateTempSuffix;
static std::wstring GetOrCreateTempSuffix() {
    if (g_updateTempSuffix.empty()) {
        g_updateTempSuffix = GetRandomSuffix();
    }
    return g_updateTempSuffix;
}

// Get path to downloaded update zip
static std::wstring GetUpdateZipPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"MediaAccess-update-" + GetOrCreateTempSuffix() + L".zip";
}

// Get path to app directory
static std::wstring GetAppDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring appPath(path);
    size_t lastSlash = appPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return appPath.substr(0, lastSlash);
    }
    return L".";
}

// Check if app was installed (vs portable) by looking for installed.txt marker
bool IsInstalledMode() {
    std::wstring appDir = GetAppDirectory();
    std::wstring markerPath = appDir + L"\\installed.txt";
    DWORD attrs = GetFileAttributesW(markerPath.c_str());
    return (attrs != INVALID_FILE_ATTRIBUTES);
}

// Get path to downloaded installer
static std::wstring GetUpdateInstallerPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"MediaAccess-Setup-" + GetOrCreateTempSuffix() + L".exe";
}

UpdateInfo CheckForUpdates() {
    UpdateInfo info = {false, "", "", "", "", "", ""};

    // Fetch releases from GitHub API
    std::string response = HttpGet(L"api.github.com", L"/repos/reaperaccessible/MediaAccess/releases");

    if (response.empty()) {
        info.errorMessage = Ts("Failed to connect to GitHub. Please check your internet connection.");
        return info;
    }

    // Get first (latest) release
    std::string release = ExtractFirstArrayObject(response);
    if (release.empty()) {
        info.errorMessage = Ts("No releases found.");
        return info;
    }

    std::string body = ExtractJsonString(release, "body");
    std::string tagName = ExtractJsonString(release, "tag_name");

    // Look for commit SHA in body: "commit XXXX"
    std::regex commitRegex("commit ([a-f0-9]+)");
    std::smatch commitMatch;
    if (std::regex_search(body, commitMatch, commitRegex)) {
        info.latestCommit = commitMatch[1].str();
    }

    // Look for version in body: "**Version:** X.Y.Z"
    std::regex versionRegex("\\*\\*Version:\\*\\* ([0-9.]+)");
    std::smatch versionMatch;
    if (std::regex_search(body, versionMatch, versionRegex)) {
        info.latestVersion = versionMatch[1].str();
    } else {
        info.latestVersion = tagName;
    }
    // Normalize tag — strip leading "v" or "V" so "v1.0" compares equal to "1.0"
    if (!info.latestVersion.empty() && (info.latestVersion[0] == 'v' || info.latestVersion[0] == 'V')) {
        info.latestVersion = info.latestVersion.substr(1);
    }

    WindowsAssets assets = FindWindowsAssets(release);
    if (assets.zipUrl.empty() && assets.installerUrl.empty()) {
        info.errorMessage = Ts("No Windows download available for this release.");
        return info;
    }

    info.downloadUrl = assets.zipUrl;
    info.installerUrl = assets.installerUrl;
    info.releaseNotes = body;

    // Only offer the update when the remote version is strictly newer than
    // what's installed. We previously also offered an update when versions
    // matched but commit SHAs differed (to ship same-version hotfix rebuilds),
    // but that fired false positives whenever the installer was built from
    // a parent commit and shipped under a release tag pointing at the next
    // commit. The simple rule below is what users expect: bump the version
    // to ship a release. No commit-tiebreaker, no surprise prompts.
    //
    // ParseVersionParts handles both old "1.0.X" and new "1.X" schemes by
    // zero-padding the shorter version, so "1.24" > "1.0.23" correctly:
    // {1,24,0} vs {1,0,23} -> second component 24 > 0 -> remote newer.
    std::string localVersion = APP_VERSION;
    info.available = IsRemoteNewer(info.latestVersion, localVersion);

    return info;
}

// Track whether we're updating with installer or zip
static bool g_updateWithInstaller = false;

bool DownloadUpdate(const std::string& url, DownloadProgressCallback progressCallback) {
    // Generate a fresh random suffix for this download session
    g_updateTempSuffix.clear();

    std::wstring destPath;
    std::string urlLower = ToLower(url);
    if ((urlLower.find("setup") != std::string::npos || urlLower.find("installer") != std::string::npos) &&
        urlLower.find(".exe") != std::string::npos) {
        destPath = GetUpdateInstallerPath();
        g_updateWithInstaller = true;
    } else {
        destPath = GetUpdateZipPath();
        g_updateWithInstaller = false;
    }
    return HttpDownload(url, destPath, progressCallback);
}

// v2.18 — lightweight update log so a failed Help-menu update can be diagnosed
// from real data instead of guessing. Appends to %TEMP%\MediaAccess_update.log.
// Best-effort; never throws.
static void UpdateLog(const char* stage) {
    wchar_t dir[MAX_PATH] = {0};
    if (!GetTempPathW(MAX_PATH, dir)) return;
    std::wstring path = std::wstring(dir) + L"MediaAccess_update.log";
    std::ofstream f(path.c_str(), std::ios::app);
    if (f) f << stage << "\r\n";
}

void ApplyUpdate() {
    UpdateLog("ApplyUpdate: entered");
    if (g_updateWithInstaller) {
        std::wstring installerPath = GetUpdateInstallerPath();

        if (GetFileAttributesW(installerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(GetMessageBoxOwner(), T("Update file not found. The download may have failed."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }

        // Write a batch wrapper in %TEMP% that runs the installer
        // synchronously, then re-launches MediaAccess.exe so the user
        // doesn't have to do it by hand. Same idea as the zip-path batch
        // below, but for the standard installer path.
        //
        // Why a batch and not chained cmd.exe `&&`: the relaunch must wait
        // for the installer to fully terminate (Inno's bootstrapper +
        // Restart Manager + UAC elevation chain) — `start /wait` on the
        // installer is the simplest way to express that.
        //
        // Why %TEMP% and not the app directory: under Program Files the
        // app dir is read-only for a normal user, and Inno may briefly
        // lock files there during install.
        //
        // The batch deliberately does NOT delete itself (`del %~f0`).
        // Self-deleting batches that spawn an installer + another exe is
        // a common malware heuristic; Windows nettoie %TEMP% au reboot.
        //
        // Belt-and-suspenders: the installer itself still has
        // CloseApplications=yes + AppMutex=MediaAccessSingleInstance in
        // installer.iss, so Inno's Restart Manager closes MediaAccess
        // gracefully before file copy even if the timing slips.
        //
        // Chicken-and-egg: this code runs in the OUTGOING version. Users
        // upgrading from 1.55/1.56 to 1.57 won't see the relaunch (their
        // old updater doesn't know about it). The auto-relaunch kicks in
        // for the FIRST upgrade after 1.57. Documented in changelog.txt.
        wchar_t tempDir[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tempDir);
        wchar_t batchPath[MAX_PATH] = {0};
        if (!GetTempFileNameW(tempDir, L"MAU", 0, batchPath)) {
            MessageBoxW(GetMessageBoxOwner(), T("Failed to launch installer."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }
        // GetTempFileName gave us a .tmp; rename to .cmd so cmd.exe takes
        // it as a script. Easiest path: just write to a sibling .cmd file
        // and delete the .tmp.
        std::wstring batchPathCmd = std::wstring(batchPath) + L".cmd";
        DeleteFileW(batchPath);

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring exePathW(exePath);

        // Convert to narrow for std::ofstream. Paths from GetTempPathW /
        // GetModuleFileNameW on Windows 11 fit ANSI for the standard
        // user account locations (%LOCALAPPDATA%\Temp, Program Files).
        std::string narrowBatch(batchPathCmd.begin(), batchPathCmd.end());
        std::string narrowInstaller(installerPath.begin(), installerPath.end());
        std::string narrowExe(exePathW.begin(), exePathW.end());

        std::ofstream batch(narrowBatch);
        if (!batch.is_open()) {
            MessageBoxW(GetMessageBoxOwner(), T("Failed to launch installer."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }
        // v2.16 — wait for THIS MediaAccess process to FULLY exit before running
        // the silent installer, instead of a blind fixed delay. The old
        // `timeout /t 2` was fragile: when an update was launched from the Help
        // menu while media was playing, shutdown (BASS/MPV/WASAPI teardown +
        // state save) took longer than 2s, so the SILENT installer started while
        // the app was still running and its files were locked → "the installer
        // downloads but doesn't start correctly." (At startup the app exits
        // instantly, which is why the startup update always worked.) `timeout`
        // also fails with no console (we launch DETACHED), so it wasn't even
        // waiting. Poll the exact PID with tasklist; `ping` provides the headless
        // ~1s delay between polls. A 60-iteration cap (~60s) guarantees we never
        // hang forever — after it, Inno's CloseApplications/AppMutex still apply.
        DWORD selfPid = GetCurrentProcessId();
        batch << "@echo off\r\n";
        batch << "REM MediaAccess auto-relaunch wrapper - safe to delete.\r\n";
        batch << "set /a tries=0\r\n";
        batch << ":waitloop\r\n";
        batch << "tasklist /fi \"PID eq " << selfPid << "\" 2>nul | find \""
              << selfPid << "\" >nul || goto installnow\r\n";
        batch << "set /a tries+=1\r\n";
        batch << "if %tries% geq 60 goto installnow\r\n";
        batch << "ping -n 2 127.0.0.1 >nul\r\n";
        batch << "goto waitloop\r\n";
        batch << ":installnow\r\n";
        // start /wait blocks until the installer terminates (including
        // its elevated bootstrapper child). Quoted empty "" is the window
        // title argument that start expects when paths are quoted.
        batch << "start /wait \"\" \"" << narrowInstaller << "\" /SILENT\r\n";
        // Relaunch unconditionally. If the install failed (UAC denied,
        // antivirus block, etc.) the old binary at exePath is still there
        // and starts fine — better than leaving the user with nothing.
        batch << "start \"\" \"" << narrowExe << "\"\r\n";
        batch.close();

        // Launch the batch detached so it survives our process exit.
        std::wstring shellCmd = L"cmd.exe /c \"" + batchPathCmd + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, &shellCmd[0], nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW | DETACHED_PROCESS,
                            nullptr, nullptr, &si, &pi)) {
            MessageBoxW(GetMessageBoxOwner(), T("Failed to launch installer."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // v2.18 — CRITICAL: exit RELIABLY so the waiting installer can replace
        // our files. The old code POSTED WM_CLOSE and relied on the message loop
        // delivering the resulting WM_QUIT — but the manual (Help-menu) update
        // runs through a nested message loop that could swallow the quit, so the
        // process never actually terminated and the silent installer collided
        // with the still-running, file-locked app. (The startup update happened
        // to win the timing.) We now run the normal shutdown cleanup SYNCHRONOUSLY
        // via SendMessage(WM_CLOSE) — which executes WM_DESTROY (SaveSettings,
        // SavePlaybackState, FreeBass/FreeMPV, etc.) right here — then guarantee
        // termination with ExitProcess so no message-loop state can keep us alive.
        UpdateLog("ApplyUpdate: installer launched, closing app now");
        SendMessageW(g_hwnd, WM_CLOSE, 0, 0);   // synchronous WM_DESTROY cleanup
        UpdateLog("ApplyUpdate: cleanup done, ExitProcess");
        ExitProcess(0);                          // unconditional, no loop needed
    } else {
        std::wstring appDir = GetAppDirectory();
        std::wstring zipPath = GetUpdateZipPath();
        std::wstring batchPath = appDir + L"\\update.bat";
        std::wstring extractDir = appDir + L"\\update_temp";

        if (GetFileAttributesW(zipPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(GetMessageBoxOwner(), T("Update file not found. The download may have failed."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring exeName(exePath);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }

        // Convert paths to narrow strings for batch file
        std::string narrowZip(zipPath.begin(), zipPath.end());
        std::string narrowExtract(extractDir.begin(), extractDir.end());
        std::string narrowApp(appDir.begin(), appDir.end());
        std::string narrowExe(exeName.begin(), exeName.end());

        std::ofstream batch(batchPath);
        batch << "@echo off\r\n";
        batch << "echo Updating MediaAccess...\r\n";
        batch << "timeout /t 2 /nobreak > nul\r\n";
        // Use double-quotes escaped for PowerShell -Command; single-quote the paths
        // inside PowerShell to avoid injection via path characters
        batch << "powershell -Command \"Expand-Archive -LiteralPath '"
              << narrowZip << "' -DestinationPath '"
              << narrowExtract << "' -Force\"\r\n";
        batch << "xcopy /s /y /q \"" << narrowExtract << "\\*\" \"" << narrowApp << "\\\"\r\n";
        batch << "rmdir /s /q \"" << narrowExtract << "\"\r\n";
        batch << "del \"" << narrowZip << "\"\r\n";
        batch << "start \"\" \"" << narrowApp << "\\" << narrowExe << "\"\r\n";
        batch << "del \"%~f0\"\r\n";
        batch.close();

        HINSTANCE result = ShellExecuteW(NULL, L"open", batchPath.c_str(), NULL, appDir.c_str(), SW_HIDE);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            MessageBoxW(GetMessageBoxOwner(), T("Failed to launch update script."),
                T("Update Error"), MB_OK | MB_ICONERROR);
            return;
        }
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// Progress dialog data
struct ProgressDialogData {
    HWND hwndDialog;
    HWND hwndProgress;
    HWND hwndText;
    bool cancelled;
    size_t totalBytes;
    size_t downloadedBytes;
};

static ProgressDialogData* g_progressData = nullptr;

static INT_PTR CALLBACK ProgressDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            g_progressData->hwndDialog = hwnd;
            g_progressData->hwndProgress = GetDlgItem(hwnd, IDC_PROGRESS_BAR);
            g_progressData->hwndText = GetDlgItem(hwnd, IDC_PROGRESS_TEXT);
            SendMessageW(g_progressData->hwndProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            return TRUE;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                g_progressData->cancelled = true;
                return TRUE;
            }
            break;

        case WM_USER + 100:
            if (g_progressData) {
                int percent = (g_progressData->totalBytes > 0)
                    ? (int)((g_progressData->downloadedBytes * 100) / g_progressData->totalBytes)
                    : 0;
                SendMessageW(g_progressData->hwndProgress, PBM_SETPOS, percent, 0);

                wchar_t text[256];
                double downloadedMB = g_progressData->downloadedBytes / (1024.0 * 1024.0);
                double totalMB = g_progressData->totalBytes / (1024.0 * 1024.0);
                swprintf(text, 256, T("Downloading: %.1f MB / %.1f MB (%d%%)"),
                    downloadedMB, totalMB, percent);
                SetWindowTextW(g_progressData->hwndText, text);
            }
            return TRUE;

        case WM_USER + 101:
            DestroyWindow(hwnd);
            return TRUE;

        case WM_USER + 102:
            DestroyWindow(hwnd);
            return TRUE;
    }
    return FALSE;
}

void ShowCheckForUpdatesDialog(HWND hwndParent, bool silent) {
    std::thread([hwndParent, silent]() {
        UpdateInfo info = CheckForUpdates();

        PostMessageW(hwndParent, WM_USER + 200, 0,
            reinterpret_cast<LPARAM>(new std::pair<UpdateInfo, bool>(info, silent)));
    }).detach();
}

void CheckForUpdatesOnStartup() {
    if (!g_checkForUpdates) return;

    std::thread([]() {
        Sleep(3000);
        if (g_hwnd) {
            ShowCheckForUpdatesDialog(g_hwnd, true);
        }
    }).detach();
}

void HandleUpdateCheckResult(HWND hwnd, UpdateInfo* info, bool silent) {
    if (!info->errorMessage.empty()) {
        if (!silent) {
            // Errors come from JSON (already UTF-8) or our Ts() translations (also UTF-8).
            // Must convert to wide for MessageBoxW or accents become mojibake.
            std::wstring werr(info->errorMessage.begin(), info->errorMessage.end());
            // Fallback proper UTF-8 -> wide conversion
            int n = MultiByteToWideChar(CP_UTF8, 0, info->errorMessage.c_str(), -1, nullptr, 0);
            if (n > 0) { werr.assign(n - 1, 0); MultiByteToWideChar(CP_UTF8, 0, info->errorMessage.c_str(), -1, &werr[0], n); }
            MessageBoxW(hwnd, werr.c_str(), T("Check for Updates"), MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (!info->available) {
        if (!silent) {
            Speak(Ts("No updates available. You are running the latest version."));
            MessageBoxW(hwnd, T("No updates available. You are running the latest version."),
                T("Check for Updates"), MB_OK | MB_ICONINFORMATION);
        }
        return;
    }

    // Build the prompt in wide chars end-to-end so accented French renders correctly.
    std::wstring wmessage = T("A new version of MediaAccess is available!");
    wmessage += L"\n\n";
    wmessage += T("Current version: ");
    wmessage += L"" APP_VERSION;
    if (strlen(BUILD_COMMIT) > 0) {
        wmessage += L" (";
        std::string sha7(BUILD_COMMIT);
        sha7 = sha7.substr(0, 7);
        wmessage += std::wstring(sha7.begin(), sha7.end());
        wmessage += L")";
    }
    wmessage += L"\n";
    wmessage += T("Latest version: ");
    wmessage += std::wstring(info->latestVersion.begin(), info->latestVersion.end());
    if (!info->latestCommit.empty()) {
        std::string sha7 = info->latestCommit.substr(0, 7);
        wmessage += L" (";
        wmessage += std::wstring(sha7.begin(), sha7.end());
        wmessage += L")";
    }
    wmessage += L"\n\n";
    wmessage += T("Do you want to download and install the update?");

    Speak(Ts("Update available. ") + info->latestVersion);

    // Use TaskDialogIndirect so we control the button labels — MessageBoxA/W
    // takes "Yes"/"No" from the Windows UI language, ignoring our app locale.
    TASKDIALOG_BUTTON buttons[] = {
        { IDYES, T("BTN_YES") },
        { IDNO,  T("BTN_NO")  },
    };
    TASKDIALOGCONFIG cfg = {0};
    cfg.cbSize = sizeof(cfg);
    cfg.hwndParent = hwnd;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = T("Update Available");
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszContent = wmessage.c_str();
    cfg.pButtons = buttons;
    cfg.cButtons = 2;
    cfg.nDefaultButton = IDYES;
    int pressed = 0;
    HRESULT hr = TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (FAILED(hr)) {
        // Fallback to MessageBoxW if TaskDialog isn't available (very old Windows).
        pressed = MessageBoxW(hwnd, wmessage.c_str(), T("Update Available"),
                              MB_YESNO | MB_ICONQUESTION);
    }
    if (pressed == IDYES) {
        // Heap-allocate so the background thread has a safe pointer
        ProgressDialogData* progressData = new ProgressDialogData();
        progressData->hwndDialog = nullptr;
        progressData->hwndProgress = nullptr;
        progressData->hwndText = nullptr;
        progressData->cancelled = false;
        progressData->totalBytes = 0;
        progressData->downloadedBytes = 0;
        g_progressData = progressData;

        HWND hwndProgress = CreateDialogW(GetModuleHandle(NULL),
            MAKEINTRESOURCEW(IDD_PROGRESS), hwnd, ProgressDlgProc);

        if (!hwndProgress) {
            MessageBoxW(hwnd, T("Starting download..."), T("Update"), MB_OK);
        }

        if (hwndProgress) {
            ShowWindow(hwndProgress, SW_SHOW);
        }

        std::string downloadUrl;
        if (IsInstalledMode() && !info->installerUrl.empty()) {
            downloadUrl = info->installerUrl;
        } else if (!info->downloadUrl.empty()) {
            downloadUrl = info->downloadUrl;
        } else if (!info->installerUrl.empty()) {
            downloadUrl = info->installerUrl;
        }

        std::thread([hwnd, hwndProgress, downloadUrl, progressData]() {
            bool wasCancelled = false;
            bool success = DownloadUpdate(downloadUrl, [hwndProgress, progressData, &wasCancelled](size_t downloaded, size_t total) {
                progressData->downloadedBytes = downloaded;
                progressData->totalBytes = total;
                if (hwndProgress) {
                    PostMessageW(hwndProgress, WM_USER + 100, 0, 0);
                }
                wasCancelled = progressData->cancelled;
                return !wasCancelled;
            });

            wasCancelled = progressData->cancelled;

            if (hwndProgress) {
                PostMessageW(hwndProgress, success ? WM_USER + 101 : WM_USER + 102, 0, 0);
            }

            Sleep(100);

            if (success && !wasCancelled) {
                PostMessageW(hwnd, WM_USER + 201, 0, 0);
            } else if (!success && !wasCancelled) {
                MessageBoxW(hwnd, T("Failed to download update."), T("Error"), MB_OK | MB_ICONERROR);
            }
        }).detach();

        if (hwndProgress) {
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0)) {
                if (!IsDialogMessageW(hwndProgress, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (!IsWindow(hwndProgress)) break;
            }
        }

        g_progressData = nullptr;
        delete progressData;
    }
}
