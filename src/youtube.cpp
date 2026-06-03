#include "youtube.h"
#include "globals.h"
#include "utils.h"
#include "player.h"
#include "accessibility.h"
#include "translations.h"
#include "video_engine.h"  // IsMPVAvailable() — used as YouTube playback fallback
#include "ui.h"           // v1.60 — SetNowPlaying / SourceType
#include "resource.h"
#include <wininet.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <shlobj.h>      // SHGetFolderPathW, CSIDL_*
#include <knownfolders.h>// FOLDERID_Downloads
#include <regex>
#include <sstream>
#include <algorithm>
#include <vector>

#pragma comment(lib, "wininet.lib")

// Sanitize user input for safe inclusion in command-line arguments.
// Removes characters that could break out of quoting or enable command injection.
static std::wstring SanitizeForCommandLine(const std::wstring& input) {
    std::wstring result;
    result.reserve(input.size());
    for (wchar_t ch : input) {
        switch (ch) {
            case L'"':
            case L'&':
            case L'|':
            case L'>':
            case L'<':
            case L'^':
            case L'%':
                // Drop dangerous characters
                break;
            default:
                result += ch;
                break;
        }
    }
    return result;
}

// Dialog state
static HWND g_ytDialog = nullptr;
static std::vector<YouTubeResult> g_ytResults;
static std::wstring g_ytNextPageToken;
static std::wstring g_ytCurrentQuery;
static bool g_ytIsPlaylistView = false;
static std::wstring g_ytCurrentPlaylistId;

// Forward declarations
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken);
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results, int count = 50);
static std::wstring RunYtdlp(const std::wstring& args);
static std::wstring UrlEncode(const std::wstring& str);
static std::wstring HttpGet(const std::wstring& url);
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key);

// Check if yt-dlp is available
static bool IsYtdlpAvailable() {
    if (g_ytdlpPath.empty()) return false;
    return PathFileExistsW(g_ytdlpPath.c_str()) != FALSE;
}

// Check if API key is available
static bool HasApiKey() {
    return !g_ytApiKey.empty();
}

// URL encode a string
static std::wstring UrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else {
            encoded << L'%' << std::hex << std::uppercase
                    << ((c >> 4) & 0xF) << (c & 0xF);
        }
    }
    return encoded.str();
}

// Simple HTTP GET request
static std::wstring HttpGet(const std::wstring& url) {
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"MediaAccess/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (hConnect) {
        char buffer[4096];
        DWORD bytesRead;
        std::string response;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        // Convert UTF-8 to wide string
        int len = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
        if (len > 0) {
            result.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, &result[0], len);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return result;
}

// Full JSON string unescape — handles every escape sequence in the JSON spec:
// \" \\ \/ \b \f \n \r \t \uXXXX (with surrogate-pair handling for codepoints
// above U+FFFF). The previous version only handled \n and \" which meant
// YouTube titles like "Chaîne 1" were displayed with the literal six
// characters î instead of being decoded to "Chaîne 1".
static std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c != L'\\' || i + 1 >= s.size()) { out.push_back(c); ++i; continue; }
        wchar_t n = s[i + 1];
        switch (n) {
            case L'"':  out.push_back(L'"');  i += 2; break;
            case L'\\': out.push_back(L'\\'); i += 2; break;
            case L'/':  out.push_back(L'/');  i += 2; break;
            case L'b':  out.push_back(L'\b'); i += 2; break;
            case L'f':  out.push_back(L'\f'); i += 2; break;
            case L'n':  out.push_back(L' ');  i += 2; break;  // newlines→space in titles
            case L'r':  out.push_back(L' ');  i += 2; break;
            case L't':  out.push_back(L' ');  i += 2; break;
            case L'u': {
                if (i + 5 >= s.size()) { out.push_back(c); ++i; break; }
                unsigned int cp = 0;
                bool ok = true;
                for (int k = 0; k < 4; ++k) {
                    wchar_t h = s[i + 2 + k];
                    cp <<= 4;
                    if      (h >= L'0' && h <= L'9') cp |= (unsigned)(h - L'0');
                    else if (h >= L'a' && h <= L'f') cp |= (unsigned)(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') cp |= (unsigned)(h - L'A' + 10);
                    else { ok = false; break; }
                }
                if (!ok) { out.push_back(c); ++i; break; }
                i += 6;
                // High surrogate? Look for a following \uXXXX low surrogate.
                if (cp >= 0xD800 && cp <= 0xDBFF
                    && i + 5 < s.size() && s[i] == L'\\' && s[i + 1] == L'u') {
                    unsigned int low = 0;
                    bool okLow = true;
                    for (int k = 0; k < 4; ++k) {
                        wchar_t h = s[i + 2 + k];
                        low <<= 4;
                        if      (h >= L'0' && h <= L'9') low |= (unsigned)(h - L'0');
                        else if (h >= L'a' && h <= L'f') low |= (unsigned)(h - L'a' + 10);
                        else if (h >= L'A' && h <= L'F') low |= (unsigned)(h - L'A' + 10);
                        else { okLow = false; break; }
                    }
                    if (okLow && low >= 0xDC00 && low <= 0xDFFF) {
                        // wchar_t on Windows is UTF-16 — push both halves.
                        out.push_back((wchar_t)cp);
                        out.push_back((wchar_t)low);
                        i += 6;
                        break;
                    }
                }
                out.push_back((wchar_t)cp);
                break;
            }
            default:
                out.push_back(c);
                ++i;
                break;
        }
    }
    return out;
}

// Simple JSON string value parser (not a full JSON parser, but it handles
// every escape sequence correctly).
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return L"";

    size_t colonPos = json.find(L':', keyPos + searchKey.length());
    if (colonPos == std::wstring::npos) return L"";

    size_t startQuote = json.find(L'"', colonPos + 1);
    if (startQuote == std::wstring::npos) return L"";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == L'"' && json[endQuote - 1] != L'\\') break;
        endQuote++;
    }

    return JsonUnescape(json.substr(startQuote + 1, endQuote - startQuote - 1));
}

// Run yt-dlp and capture output
static std::wstring RunYtdlp(const std::wstring& args) {
    if (!IsYtdlpAvailable()) return L"";

    std::wstring cmdLine = L"\"" + g_ytdlpPath + L"\" " + args;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return L"";

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return L"";
    }

    CloseHandle(hWritePipe);

    std::string output;
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 30000);  // 30 second timeout
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Convert UTF-8 to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &result[0], len);
    return result;
}

// Search using YouTube Data API
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken) {
    if (!HasApiKey()) return false;

    std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
    url += UrlEncode(query);
    url += L"&key=" + g_ytApiKey;
    if (!pageToken.empty()) {
        url += L"&pageToken=" + pageToken;
    }

    std::wstring response = HttpGet(url);
    if (response.empty()) return false;

    // Parse results (simple parsing, not full JSON)
    nextPageToken = ParseJsonString(response, L"nextPageToken");

    // Find items array and parse each item
    size_t itemsPos = response.find(L"\"items\"");
    if (itemsPos == std::wstring::npos) return false;

    size_t searchStart = itemsPos;
    while ((searchStart = response.find(L"\"videoId\"", searchStart)) != std::wstring::npos) {
        YouTubeResult result;
        result.videoId = ParseJsonString(response.substr(searchStart, 500), L"videoId");

        // Find the snippet for this item
        size_t snippetPos = response.rfind(L"\"snippet\"", searchStart);
        if (snippetPos != std::wstring::npos && snippetPos > itemsPos) {
            std::wstring snippet = response.substr(snippetPos, searchStart - snippetPos + 1000);
            result.title = ParseJsonString(snippet, L"title");
            result.channel = ParseJsonString(snippet, L"channelTitle");
        }

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
        searchStart += 10;
    }

    return !results.empty();
}

// Helper to try multiple JSON field names
static std::wstring ParseJsonStringMulti(const std::wstring& json, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* key : keys) {
        std::wstring value = ParseJsonString(json, key);
        if (!value.empty()) return value;
    }
    return L"";
}

// Tracks how many results yt-dlp's incremental pagination has emitted so
// far for the current query. yt-dlp doesn't expose a real page token, so
// we ask for ytsearch(N): each time, growing N, and only surface the items
// that weren't in the previous batch.
static int g_ytYtdlpLoaded = 0;

// Search using yt-dlp. Pass count = how many YouTube results to ASK FOR.
// The function returns whatever yt-dlp produced; the caller is responsible
// for skipping any items already shown to the user (see DoLoadMore path).
static bool SearchWithYtdlp(const std::wstring& query,
                            std::vector<YouTubeResult>& results,
                            int count) {
    // Use yt-dlp to search YouTube
    std::wstring safeQuery = SanitizeForCommandLine(query);
    wchar_t prefix[32];
    swprintf(prefix, 32, L"ytsearch%d:", count);
    std::wstring args = L"--flat-playlist --dump-json \"" + std::wstring(prefix) + safeQuery + L"\"";
    OutputDebugStringW((L"[YT] Running: " + g_ytdlpPath + L" " + args + L"\n").c_str());
    std::wstring output = RunYtdlp(args);
    OutputDebugStringW((L"[YT] Output length: " + std::to_wstring(output.length()) + L"\n").c_str());
    if (output.length() < 500) {
        OutputDebugStringW((L"[YT] Output: " + output + L"\n").c_str());
    }
    if (output.empty()) return false;

    // Parse JSON lines (each line is a video)
    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        // Try multiple field names for compatibility with different yt-dlp versions
        result.videoId = ParseJsonStringMulti(line, {L"id", L"video_id", L"display_id"});
        result.title = ParseJsonStringMulti(line, {L"title", L"fulltitle"});
        result.channel = ParseJsonStringMulti(line, {L"channel", L"uploader", L"uploader_id", L"channel_id"});
        result.duration = ParseJsonStringMulti(line, {L"duration_string", L"duration"});

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// Public search function
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    OutputDebugStringW(L"[YT] YouTubeSearch called\n");
    OutputDebugStringW((L"[YT] HasApiKey: " + std::wstring(HasApiKey() ? L"yes" : L"no") + L"\n").c_str());
    OutputDebugStringW((L"[YT] IsYtdlpAvailable: " + std::wstring(IsYtdlpAvailable() ? L"yes" : L"no") + L"\n").c_str());

    // Try API first if available
    if (HasApiKey() && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        OutputDebugStringW(L"[YT] API search succeeded\n");
        return true;
    }

    // yt-dlp fallback. yt-dlp doesn't have native pagination — we simulate
    // it by asking for a growing count each time and only surfacing the
    // items the user hasn't seen yet (deduplicated by video id below).
    if (IsYtdlpAvailable()) {
        OutputDebugStringW(L"[YT] Trying yt-dlp search\n");
        const int batchSize = 50;
        if (pageToken.empty()) {
            // First page.
            g_ytYtdlpLoaded = 0;
            if (!SearchWithYtdlp(query, results, batchSize)) return false;
            g_ytYtdlpLoaded = static_cast<int>(results.size());
            // Use a sentinel non-empty token so the auto-load path keeps
            // firing — we don't have a real "no more results" signal until
            // a re-query returns the same set, which we detect below.
            nextPageToken = (g_ytYtdlpLoaded >= batchSize) ? L"ytdlp" : L"";
            return !results.empty();
        }
        if (pageToken == L"ytdlp") {
            // Subsequent page: re-fetch a bigger batch, return only the new ids.
            int newTotal = g_ytYtdlpLoaded + batchSize;
            std::vector<YouTubeResult> all;
            if (!SearchWithYtdlp(query, all, newTotal)) return false;
            // Build the set of ids already known to the caller (g_ytResults
            // is static in this translation unit) so we don't re-emit them.
            // Dedup against the new batch too in case yt-dlp returns dups.
            std::vector<std::wstring> seen;
            seen.reserve(g_ytResults.size());
            for (const auto& r : g_ytResults) seen.push_back(r.videoId);
            for (const auto& r : all) {
                bool dup = false;
                for (const auto& id : seen) {
                    if (id == r.videoId) { dup = true; break; }
                }
                if (!dup) {
                    results.push_back(r);
                    seen.push_back(r.videoId);
                }
            }
            g_ytYtdlpLoaded = newTotal;
            // No new items → we've hit the search ceiling; stop offering more.
            nextPageToken = results.empty() ? L"" : L"ytdlp";
            return true;
        }
    }

    OutputDebugStringW(L"[YT] No search method available\n");
    return false;
}

// Get playlist contents
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    if (!IsYtdlpAvailable()) return false;

    std::wstring safePlaylistId = SanitizeForCommandLine(playlistId);
    std::wstring url = L"https://www.youtube.com/playlist?list=" + safePlaylistId;
    std::wstring args = L"--flat-playlist --dump-json \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return false;

    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        // Try multiple field names for compatibility with different yt-dlp versions
        result.videoId = ParseJsonStringMulti(line, {L"id", L"video_id", L"display_id"});
        result.title = ParseJsonStringMulti(line, {L"title", L"fulltitle"});
        result.channel = ParseJsonStringMulti(line, {L"channel", L"uploader", L"uploader_id", L"channel_id"});
        result.duration = ParseJsonStringMulti(line, {L"duration_string", L"duration"});

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// YouTube video mode toggle
static bool g_ytVideoMode = false;
void SetYouTubeVideoMode(bool mode) { g_ytVideoMode = mode; }
bool GetYouTubeVideoMode() { return g_ytVideoMode; }

// Get raw YouTube URL for video playback (libmpv handles yt-dlp internally)
bool YouTubeGetVideoURL(const std::wstring& videoId, std::wstring& url) {
    std::wstring safeId = SanitizeForCommandLine(videoId);
    url = L"https://www.youtube.com/watch?v=" + safeId;
    return true;
}

// Get stream URL for a video
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl) {
    if (!IsYtdlpAvailable()) return false;

    // Get best audio format URL. Prefer M4A/AAC because BASS can decode that
    // via bass_aac.dll. Plain "bestaudio" usually returns WebM/Opus which
    // BASS cannot demux (BASS_ERROR_FILEFORM). Fallback chain:
    //   1. Any m4a container (AAC).
    //   2. Any AAC codec regardless of container.
    //   3. mp3 (rare on YouTube, but fine for BASS).
    //   4. itag 140 — YouTube's universal 128 kbps AAC stream.
    std::wstring safeVideoId = SanitizeForCommandLine(videoId);
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeVideoId;
    std::wstring args = L"-f \"bestaudio[ext=m4a]/bestaudio[acodec=aac]/bestaudio[ext=mp3]/140\" --get-url \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);

    // Trim whitespace
    size_t start = output.find_first_not_of(L" \t\r\n");
    size_t end = output.find_last_not_of(L" \t\r\n");
    if (start != std::wstring::npos && end != std::wstring::npos) {
        streamUrl = output.substr(start, end - start + 1);
        return !streamUrl.empty() && streamUrl.find(L"http") == 0;
    }

    return false;
}

// Returns %LOCALAPPDATA%\MediaAccess\YouTubeCache (creates it if missing).
// Cache lives in LOCALAPPDATA — survives reboots and Windows disk cleanup.
static std::wstring GetYouTubeCacheDir() {
    wchar_t base[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
    std::wstring dir = std::wstring(base) + L"\\MediaAccess";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\YouTubeCache";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Returns the file in the cache for this video id if one exists, else L"".
// The video ID is the canonical key (independent of title changes on YT).
static std::wstring FindCachedAudio(const std::wstring& videoId) {
    std::wstring base = GetYouTubeCacheDir() + L"\\yt-" + videoId;
    static const wchar_t* exts[] = { L".m4a", L".mp4", L".aac", L".mp3", L".webm", L".opus" };
    for (auto ext : exts) {
        std::wstring candidate = base + ext;
        if (PathFileExistsW(candidate.c_str())) return candidate;
    }
    return L"";
}

// ============================================================
// Cache version marker (v1.0.12)
// ============================================================
//
// A zero-byte sidecar "yt-<videoId>.v2" next to the audio file means the
// audio was downloaded with --embed-chapters. Files downloaded prior to
// v1.0.12 don't have this marker, so we lazily re-download them on next
// play. After re-download lands the marker is written.

static std::wstring GetCacheMarkerPath(const std::wstring& videoId) {
    return GetYouTubeCacheDir() + L"\\yt-" + videoId + L".v2";
}

static bool HasCacheVersionMarker(const std::wstring& videoId) {
    return PathFileExistsW(GetCacheMarkerPath(videoId).c_str()) != FALSE;
}

static void WriteCacheVersionMarker(const std::wstring& videoId) {
    HANDLE h = CreateFileW(GetCacheMarkerPath(videoId).c_str(),
                           GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Look for a "yt-<id>.refresh.*" file in the cache dir. If one is found
// AND the original audio file isn't currently locked (e.g. BASS no longer
// playing it), atomically swap it in and create the version marker.
// Called at the top of YouTubePlayById so the swap happens between plays,
// never during one.
static void ApplyPendingCacheRefresh(const std::wstring& videoId) {
    std::wstring cacheDir = GetYouTubeCacheDir();
    std::wstring refreshBase = cacheDir + L"\\yt-" + videoId + L".refresh";
    static const wchar_t* exts[] = { L".m4a", L".mp4", L".aac", L".mp3" };
    std::wstring newFile;
    std::wstring newExt;
    for (auto ext : exts) {
        std::wstring c = refreshBase + ext;
        if (PathFileExistsW(c.c_str())) { newFile = c; newExt = ext; break; }
    }
    if (newFile.empty()) return;

    std::wstring target = cacheDir + L"\\yt-" + videoId + newExt;
    // Remove the old cached file (any extension) — even one with a
    // different ext, since the new file's ext may differ.
    std::wstring oldFile = FindCachedAudio(videoId);
    if (!oldFile.empty() && oldFile != target) DeleteFileW(oldFile.c_str());
    DeleteFileW(target.c_str());  // ok if it didn't exist

    if (MoveFileW(newFile.c_str(), target.c_str())) {
        WriteCacheVersionMarker(videoId);
    }
}

// Background re-download for a cached file that's missing the v2 marker.
// Writes to "yt-<videoId>.refresh.<ext>" so the file BASS is currently
// playing isn't touched. ApplyPendingCacheRefresh swaps it in on the
// next play of the same video.
static DWORD WINAPI RefreshCacheThread(LPVOID arg) {
    std::wstring* idPtr = static_cast<std::wstring*>(arg);
    std::wstring videoId = *idPtr;
    delete idPtr;

    if (!IsYtdlpAvailable()) return 0;

    std::wstring dir = GetYouTubeCacheDir();
    std::wstring outBase = dir + L"\\yt-" + videoId + L".refresh";
    std::wstring outArg  = outBase + L".%(ext)s";

    std::wstring url = L"https://www.youtube.com/watch?v=" + videoId;
    std::wstring args = L"-f \"bestaudio[ext=m4a]/bestaudio[acodec=aac]/140\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--embed-chapters --no-overwrites "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    RunYtdlp(args);
    // Nothing else to do — the next call to ApplyPendingCacheRefresh
    // (on next play of this video) will swap the file in.
    return 0;
}

// Kick off a background refresh for this videoId if no .v2 marker exists.
// Does nothing if there's already a refresh file pending (don't re-fetch
// the same data twice).
static void StartCacheRefreshIfNeeded(const std::wstring& videoId) {
    if (HasCacheVersionMarker(videoId)) return;
    // If a .refresh file is already present, skip — it'll be swapped on
    // next play. ApplyPendingCacheRefresh actually swaps; here we just
    // avoid spawning a second download for the same file.
    std::wstring cacheDir = GetYouTubeCacheDir();
    static const wchar_t* exts[] = { L".m4a", L".mp4", L".aac", L".mp3" };
    for (auto ext : exts) {
        std::wstring c = cacheDir + L"\\yt-" + videoId + L".refresh" + ext;
        if (PathFileExistsW(c.c_str())) return;
    }
    std::wstring* idCopy = new std::wstring(videoId);
    HANDLE t = CreateThread(nullptr, 0, RefreshCacheThread, idCopy, 0, nullptr);
    if (t) CloseHandle(t);
}

// Downloads YouTube audio into the persistent cache and returns the local path.
// If the audio is already cached we return immediately — no network round-trip,
// no yt-dlp invocation, no Speak("Downloading audio") needed by the caller.
bool YouTubeDownloadAudio(const std::wstring& videoId, std::wstring& outFilePath) {
    std::wstring safeId = SanitizeForCommandLine(videoId);

    // Cache hit → instant playback.
    std::wstring cached = FindCachedAudio(safeId);
    if (!cached.empty()) {
        outFilePath = cached;
        return true;
    }

    if (!IsYtdlpAvailable()) return false;

    std::wstring dir = GetYouTubeCacheDir();
    std::wstring outBase = dir + L"\\yt-" + safeId;
    std::wstring outArg  = outBase + L".%(ext)s";

    // Prefer M4A (AAC) for BASS compatibility. --no-playlist guards against
    // accidental queue expansion. --quiet/--no-progress/--no-warnings keep
    // stdout clean. We DON'T pass --no-overwrites because the cache hit
    // check above already covers that.
    // --embed-chapters writes YouTube chapter markers into the M4A file so
    // BASS sees them via ID3v2/Vorbis tags after our chapter-detection fix.
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring args = L"-f \"bestaudio[ext=m4a]/bestaudio[acodec=aac]/140\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--embed-chapters "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    RunYtdlp(args);

    cached = FindCachedAudio(safeId);
    if (!cached.empty()) {
        // Brand-new download done with --embed-chapters → flag it as v2.
        WriteCacheVersionMarker(safeId);
        outFilePath = cached;
        return true;
    }
    return false;
}

// Returns true if this video is already in the persistent cache.
bool YouTubeIsAudioCached(const std::wstring& videoId) {
    return !FindCachedAudio(SanitizeForCommandLine(videoId)).empty();
}

// Sanitize a video title for use as a Windows filename — strips characters
// that NTFS forbids: \ / : * ? " < > | plus control chars. Replaces them
// with a space then collapses runs of whitespace.
static std::wstring SanitizeForFilename(const std::wstring& title) {
    std::wstring out;
    out.reserve(title.size());
    for (wchar_t ch : title) {
        if (ch < 32) continue;  // control
        switch (ch) {
            case L'\\': case L'/': case L':': case L'*':
            case L'?':  case L'"': case L'<': case L'>': case L'|':
                out += L' '; break;
            default:
                out += ch;
        }
    }
    // Trim leading/trailing whitespace
    while (!out.empty() && iswspace(out.front())) out.erase(out.begin());
    while (!out.empty() && iswspace(out.back()))  out.pop_back();
    // Cap length (Windows MAX_PATH is 260; keep room for path + ext)
    if (out.size() > 150) out.resize(150);
    if (out.empty()) out = L"video";
    return out;
}

// Historical default: Downloads\MediaAccess\YouTube. Used when the user has
// not configured a custom download folder, or when the configured folder is
// unreachable (network drive offline, etc.). Creates parents as needed.
static std::wstring GetLegacyDownloadsTargetDir() {
    PWSTR raw = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &raw)) && raw) {
        root = raw;
        CoTaskMemFree(raw);
    } else {
        // Fallback: %USERPROFILE%\Downloads
        wchar_t profile[MAX_PATH] = {0};
        SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile);
        root = std::wstring(profile) + L"\\Downloads";
    }
    std::wstring out = root + L"\\MediaAccess";
    CreateDirectoryW(out.c_str(), nullptr);
    out += L"\\YouTube";
    CreateDirectoryW(out.c_str(), nullptr);
    return out;
}

// v1.71 — Returns the target directory for permanent YouTube downloads.
// Two-tier resolution:
//   1. If the user has set g_ytDownloadPath (Options > YouTube > Download folder)
//      AND the path is reachable + writable, use that. Create it if missing.
//   2. Otherwise (no preference, or preference unreachable / read-only), fall
//      back silently to the historical Downloads\MediaAccess\YouTube path.
// The fallback is silent on purpose: a chatty error message would be more
// annoying than missing a download (the user can always go fish the file out
// of the legacy folder if surprised). Race-safe: the value is captured once
// per call into a local std::wstring before any filesystem access. The Edit
// field in Options is NOT ES_READONLY, so the user can clear the path by
// hand to revert to the historical default — any free-typed garbage path
// simply falls back here without complaint.
static std::wstring GetDownloadsTargetDir() {
    std::wstring pref = g_ytDownloadPath;   // local snapshot — no race with Options write
    if (!pref.empty()) {
        wchar_t abs[MAX_PATH] = {0};
        DWORD n = GetFullPathNameW(pref.c_str(), MAX_PATH, abs, nullptr);
        if (n > 0 && n < MAX_PATH) {
            // CreateDirectoryW is a no-op if the directory already exists
            CreateDirectoryW(abs, nullptr);
            DWORD attrs = GetFileAttributesW(abs);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                // Confirm writability so a stale path (USB stick removed,
                // network share offline, read-only mount) falls back cleanly.
                std::wstring probe = std::wstring(abs) + L"\\.ma_write_test";
                HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                       nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    return abs;
                }
            }
        }
        // Any failure path here falls through to the legacy default below.
    }
    return GetLegacyDownloadsTargetDir();
}

// Permanently download a video to the user's Downloads folder.
// Filename uses the video's real title (sanitized) so users can browse the
// folder and recognize their tracks. Returns the absolute path on success.
bool YouTubeDownloadPermanent(const std::wstring& videoId,
                              const std::wstring& title,
                              std::wstring& outFilePath) {
    if (!IsYtdlpAvailable()) return false;
    std::wstring safeId = SanitizeForCommandLine(videoId);

    std::wstring dir = GetDownloadsTargetDir();
    std::wstring base = SanitizeForFilename(title);
    if (base.empty()) base = safeId;
    std::wstring outBase = dir + L"\\" + base;

    // If the file already exists (user downloaded this before), don't
    // overwrite — yt-dlp's default behavior with -o is to skip if file
    // exists, but be explicit by adding --no-overwrites.
    std::wstring outArg = outBase + L".%(ext)s";
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring args = L"-f \"bestaudio[ext=m4a]/bestaudio[acodec=aac]/140\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--no-overwrites --embed-chapters "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    RunYtdlp(args);

    static const wchar_t* exts[] = { L".m4a", L".mp4", L".aac", L".mp3", L".webm", L".opus" };
    for (auto ext : exts) {
        std::wstring candidate = outBase + ext;
        if (PathFileExistsW(candidate.c_str())) {
            outFilePath = candidate;
            return true;
        }
    }
    return false;
}

// ============================================================
// Format-aware download engine (v1.94+)
// ============================================================

// Resolve ffmpeg.exe. See header for the resolution order. Returns the full
// path or an empty string when ffmpeg is nowhere to be found (the caller then
// omits --ffmpeg-location and lets yt-dlp try whatever it can).
std::wstring GetFfmpegLocation() {
    // 1. Explicit INI override: [YouTube] FfmpegPath
    if (!g_configPath.empty()) {
        wchar_t iniBuf[MAX_PATH] = {0};
        GetPrivateProfileStringW(L"YouTube", L"FfmpegPath", L"",
                                 iniBuf, MAX_PATH, g_configPath.c_str());
        if (iniBuf[0] != L'\0' && PathFileExistsW(iniBuf)) {
            return std::wstring(iniBuf);
        }
    }

    // 2. <app>\lib\ffmpeg.exe (same layout used for yt-dlp's bundled fallback).
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        std::wstring bundled = std::wstring(exePath) + L"lib\\ffmpeg.exe";
        if (PathFileExistsW(bundled.c_str())) return bundled;
    }

    // 3. ffmpeg.exe on the system PATH.
    wchar_t found[MAX_PATH] = {0};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return std::wstring(found);
    }

    // 4. Not found.
    return L"";
}

// Format a byte count into a short human-readable string ("12.3 MB").
// Returns "" when size is unknown (0).
static std::wstring HumanFileSize(long long bytes) {
    if (bytes <= 0) return L"";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[32];
    if (u == 0) {
        swprintf(buf, 32, L"%lld %s", bytes, units[u]);
    } else {
        swprintf(buf, 32, L"%.1f %s", v, units[u]);
    }
    return std::wstring(buf);
}

// Parse a numeric JSON value (integer or float) that directly follows
// "key": ... within a JSON object substring. Returns 0.0 when the key is
// absent or the value is JSON null/string. Handles negatives and decimals.
static double ParseJsonNumber(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return 0.0;
    size_t colonPos = json.find(L':', keyPos + searchKey.length());
    if (colonPos == std::wstring::npos) return 0.0;
    size_t p = colonPos + 1;
    while (p < json.size() && (json[p] == L' ' || json[p] == L'\t')) ++p;
    // Collect a numeric token. Stops at the first char that can't be part of
    // an int/float literal (so "null"/strings yield 0).
    std::wstring tok;
    while (p < json.size()) {
        wchar_t c = json[p];
        if ((c >= L'0' && c <= L'9') || c == L'-' || c == L'+' ||
            c == L'.' || c == L'e' || c == L'E') {
            tok.push_back(c);
            ++p;
        } else {
            break;
        }
    }
    if (tok.empty()) return 0.0;
    try {
        return std::stod(tok);
    } catch (...) {
        return 0.0;
    }
}

// Split a "%(formats)j" JSON array string into its top-level object substrings.
// We don't have a full JSON parser, so we walk the array tracking brace depth
// and string/escape state to find each {...} object. Robust against braces,
// brackets and quotes appearing inside string values.
static std::vector<std::wstring> SplitJsonObjects(const std::wstring& arr) {
    std::vector<std::wstring> objs;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    size_t objStart = std::wstring::npos;
    for (size_t i = 0; i < arr.size(); ++i) {
        wchar_t c = arr[i];
        if (inStr) {
            if (esc)             { esc = false; }
            else if (c == L'\\') { esc = true; }
            else if (c == L'"')  { inStr = false; }
            continue;
        }
        if (c == L'"') { inStr = true; continue; }
        if (c == L'{') {
            if (depth == 0) objStart = i;
            ++depth;
        } else if (c == L'}') {
            if (depth > 0) --depth;
            if (depth == 0 && objStart != std::wstring::npos) {
                objs.push_back(arr.substr(objStart, i - objStart + 1));
                objStart = std::wstring::npos;
            }
        }
    }
    return objs;
}

// Query yt-dlp for all formats of a video and parse them, best first.
std::vector<YtFormat> ParseFormatsArray(const std::wstring& videoId) {
    std::vector<YtFormat> formats;
    if (!IsYtdlpAvailable()) return formats;

    std::wstring safeId = SanitizeForCommandLine(videoId);
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    // "%(formats)j" prints the full formats array as one compact JSON line.
    // RunYtdlp accumulates stdout into a growing std::string (no fixed buffer
    // cap), so a 30+ format JSON is captured in full.
    std::wstring args = L"--no-warnings --no-playlist "
                        L"--print \"%(formats)j\" \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return formats;

    // Trim to the JSON array. yt-dlp may emit lines before it; find the first
    // '[' and the matching tail ']'.
    size_t lb = output.find(L'[');
    size_t rb = output.rfind(L']');
    if (lb == std::wstring::npos || rb == std::wstring::npos || rb <= lb) {
        return formats;
    }
    std::wstring arr = output.substr(lb, rb - lb + 1);

    for (const std::wstring& obj : SplitJsonObjects(arr)) {
        YtFormat f;
        f.formatId = ParseJsonString(obj, L"format_id");
        if (f.formatId.empty()) continue;  // unusable without an id
        f.ext    = ParseJsonString(obj, L"ext");
        f.vcodec = ParseJsonString(obj, L"vcodec");
        f.acodec = ParseJsonString(obj, L"acodec");
        f.note   = ParseJsonString(obj, L"format_note");

        // v1.97 — Skip non-media "formats" that have neither audio nor video.
        // YouTube exposes storyboard entries (sb0/sb1/..., ext=mhtml — grids of
        // preview thumbnails) in the format list; they are not downloadable
        // media and produced a missing/.mhtml file when picked. Filter out any
        // entry where both codecs are absent ("none"/empty), and any mhtml ext.
        {
            bool hasVideo = (f.vcodec != L"none" && !f.vcodec.empty());
            bool hasAudio = (f.acodec != L"none" && !f.acodec.empty());
            if ((!hasVideo && !hasAudio) || f.ext == L"mhtml") continue;
        }

        f.height = static_cast<int>(ParseJsonNumber(obj, L"height"));

        // Size: filesize is exact, filesize_approx is an estimate. Prefer exact.
        long long fs = static_cast<long long>(ParseJsonNumber(obj, L"filesize"));
        if (fs <= 0) fs = static_cast<long long>(ParseJsonNumber(obj, L"filesize_approx"));
        f.filesize = fs > 0 ? fs : 0;
        f.sizeStr = HumanFileSize(f.filesize);

        bool audioOnly = (f.vcodec == L"none" || f.vcodec.empty()) && f.acodec != L"none";
        bool videoOnly = (f.acodec == L"none" || f.acodec.empty()) && f.vcodec != L"none";

        // Human resolution label.
        if (audioOnly) {
            f.resolution = L"audio only";
        } else if (f.height > 0) {
            wchar_t res[40];
            swprintf(res, 40, L"%dp", f.height);
            f.resolution = res;
            if (videoOnly) f.resolution += L" (video only)";
        } else if (!f.note.empty()) {
            f.resolution = f.note;
        } else {
            f.resolution = f.formatId;
        }

        formats.push_back(f);
    }

    // Sort best first: by height desc, then by size desc (proxy for bitrate).
    std::sort(formats.begin(), formats.end(),
              [](const YtFormat& a, const YtFormat& b) {
                  if (a.height != b.height) return a.height > b.height;
                  return a.filesize > b.filesize;
              });

    return formats;
}

// Validate a yt-dlp format_id against a strict whitelist so it can never break
// out of the command line. yt-dlp ids look like "137", "251", "616-drc",
// "bestvideo+bestaudio". We allow [A-Za-z0-9_+.-]; anything else is rejected.
static bool IsValidFormatId(const std::wstring& id) {
    if (id.empty() || id.size() > 64) return false;
    for (wchar_t c : id) {
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                  (c >= L'0' && c <= L'9') ||
                  c == L'_' || c == L'+' || c == L'.' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

// Download a specific format_id. Mirrors YouTubeDownloadPermanent's destination
// and naming, but selects the user's chosen format and merges to mp4 when the
// stream is video-only. See header for the contract.
bool YouTubeDownloadFormat(const std::wstring& videoId,
                           const std::wstring& title,
                           const std::wstring& formatId,
                           bool videoOnly,
                           std::wstring& outFilePath) {
    if (!IsYtdlpAvailable()) return false;
    if (!IsValidFormatId(formatId)) return false;  // caller falls back

    // Build the final -f selector AFTER validating the raw formatId above.
    // The "+bestaudio..." literals are constants controlled by the code, never
    // by the user, so no injection is possible by concatenating them here.
    // A video-only stream (acodec == "none") needs the best audio merged in;
    // otherwise the raw id already carries audio (combined) or is audio-only.
    std::wstring selector = videoOnly
        ? (formatId + L"+bestaudio[ext=m4a]/" + formatId + L"+bestaudio/" + formatId)
        : formatId;

    std::wstring safeId = SanitizeForCommandLine(videoId);

    std::wstring dir = GetDownloadsTargetDir();
    std::wstring base = SanitizeForFilename(title);
    if (base.empty()) base = safeId;
    std::wstring outBase = dir + L"\\" + base;
    std::wstring outArg = outBase + L".%(ext)s";

    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring ffmpeg = GetFfmpegLocation();

    std::wstring args = L"-f \"" + selector + L"\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--no-overwrites --embed-chapters "
                        L"--merge-output-format mp4 ";
    if (!ffmpeg.empty()) {
        args += L"--ffmpeg-location \"" + ffmpeg + L"\" ";
    }
    args += L"-o \"" + outArg + L"\" \"" + url + L"\"";
    RunYtdlp(args);

    // yt-dlp may produce any of these depending on the chosen stream and
    // whether a merge happened.
    static const wchar_t* exts[] = {
        L".mp4", L".m4a", L".aac", L".mp3", L".webm",
        L".mkv", L".flac", L".opus", L".wav"
    };
    for (auto ext : exts) {
        std::wstring candidate = outBase + ext;
        if (PathFileExistsW(candidate.c_str())) {
            outFilePath = candidate;
            return true;
        }
    }
    return false;
}

// Wipes every cached audio file. Returns the count removed.
int ClearYouTubeCache() {
    std::wstring dir = GetYouTubeCacheDir();
    std::wstring pattern = dir + L"\\yt-*.*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (DeleteFileW(full.c_str())) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// Returns total bytes used by the YouTube cache.
unsigned long long GetYouTubeCacheSize() {
    std::wstring dir = GetYouTubeCacheDir();
    std::wstring pattern = dir + L"\\yt-*.*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long long total = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER sz; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
        total += sz.QuadPart;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

// Enforce a cache size cap: delete the OLDEST yt-* files (by last-write time)
// until the total cache size is below the limit. Companion files (sidecars
// like .v2 markers, .refresh.*) are deleted alongside their primary so the
// cache stays consistent. Pass 0 or negative to disable (no-op).
// Returns the number of files actually removed.
int EnforceYouTubeCacheLimit(int limitMB) {
    if (limitMB <= 0) return 0;
    unsigned long long limitBytes = (unsigned long long)limitMB * 1024ULL * 1024ULL;
    unsigned long long current = GetYouTubeCacheSize();
    if (current <= limitBytes) return 0;

    std::wstring dir = GetYouTubeCacheDir();
    std::wstring pattern = dir + L"\\yt-*.*";

    struct CacheEntry {
        std::wstring name;
        ULONGLONG mtime;
        unsigned long long size;
    };
    std::vector<CacheEntry> entries;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER sz; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
        ULARGE_INTEGER mt; mt.LowPart = fd.ftLastWriteTime.dwLowDateTime; mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        entries.push_back({fd.cFileName, mt.QuadPart, sz.QuadPart});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Sort oldest first
    std::sort(entries.begin(), entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) { return a.mtime < b.mtime; });

    int removed = 0;
    for (const auto& e : entries) {
        if (current <= limitBytes) break;
        std::wstring full = dir + L"\\" + e.name;
        if (DeleteFileW(full.c_str())) {
            current = (current > e.size) ? (current - e.size) : 0;
            removed++;
        }
    }
    return removed;
}

// Removes any leftover files from the old %TEMP%\MediaAccess location
// (pre-v1.0.8). The new cache lives in %LOCALAPPDATA%\MediaAccess\YouTubeCache
// and is permanent — kept until the user explicitly clears it via the menu.
void CleanupYouTubeTempFiles() {
    wchar_t tempDir[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring dir = std::wstring(tempDir) + L"MediaAccess";
    std::wstring pattern = dir + L"\\yt-*.*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        DeleteFileW(full.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());  // ok if not empty (no-op)
}

// Plays a YouTube video by ID. Strategy (in order):
//   1. yt-dlp downloads bestaudio AAC/M4A to temp file -> BASS plays the
//      file locally with full DSP/effects support. (Most reliable, all features.)
//   2. If download fails or file won't load, libmpv plays the raw
//      YouTube URL directly (handles every format including livestreams,
//      but loses BASS effects). User is told via Speak.
//   3. If both fail, user sees a single error.
//
// IMPORTANT: When the user prefers video mode (g_ytVideoMode), skip the
// audio download and go straight to libmpv with the raw YouTube URL.
// ============================================================
// Hybrid playback state (v1.0.9)
// ============================================================
//
// When the user picks an uncached YouTube result we want INSTANT playback
// (like AYDP) but we also want all BASS DSP effects (tempo, pitch, EQ...)
// once they're available. Solution: libmpv streams the YouTube URL
// immediately while a background thread downloads the audio via yt-dlp.
// As soon as the download lands on disk, the main thread swaps the engine
// from mpv to BASS at the exact same playback position.
//
// `g_ytHybrid` tracks which video is currently in the "streaming, waiting
// for download" state. If the user moves on to something else, .active
// becomes false and the swap is skipped (the cached file still lands and
// will be reused next time).

#include <atomic>

struct YtHybridState {
    std::wstring videoId;
    std::atomic<bool> active{false};
};
static YtHybridState g_ytHybrid;

// Background thread: blocking download, then posts WM_YT_HYBRID_READY.
static DWORD WINAPI HybridDownloadThread(LPVOID arg) {
    std::wstring* idPtr = static_cast<std::wstring*>(arg);
    std::wstring videoId = *idPtr;
    delete idPtr;

    std::wstring localFile;
    if (YouTubeDownloadAudio(videoId, localFile)) {
        // Heap-allocate the id again so it survives until the main thread
        // pops the message off the queue.
        std::wstring* postId = new std::wstring(videoId);
        PostMessageW(g_hwnd, WM_YT_HYBRID_READY, 0, reinterpret_cast<LPARAM>(postId));
    }
    return 0;
}

// Called from the main thread when WM_YT_HYBRID_READY arrives. Swaps the
// libmpv stream for a BASS file at the same playback position. If the
// user has since changed track or stopped, the swap is skipped and the
// downloaded file just stays in the cache for next time.
void YouTubeOnHybridDownloadReady(const std::wstring& videoId) {
    if (!g_ytHybrid.active.load()) return;
    if (g_ytHybrid.videoId != videoId) return;
    g_ytHybrid.active.store(false);

    std::wstring localFile;
    if (!YouTubeDownloadAudio(videoId, localFile)) return;

    // Snapshot mpv's position BEFORE we silence it.
    double pos = GetCurrentPosition();

    // Critical: silence mpv FIRST. LoadFile() only tears down BASS state;
    // it doesn't know mpv is still streaming the same audio in parallel.
    // Without this stop, both engines play the YouTube audio at once.
    MPVStop();
    // Re-enable mpv's video for the next real video playback request.
    MPVSetAudioOnly(false);
    if (g_videoHwnd) ShowWindow(g_videoHwnd, SW_HIDE);
    g_isVideoPlaying = false;

    if (!LoadFile(localFile.c_str())) return;
    if (pos > 1.0) SeekToPosition(pos);
    if (g_speechYTHybrid) Speak(Ts("Effects activated"));
}

// Public: drop the pending hybrid swap. Used by LoadFile/LoadURL in
// player.cpp so a non-YouTube load isn't clobbered seconds later by a
// hybrid download that finished in the background.
void YouTubeCancelHybrid() {
    g_ytHybrid.active.store(false);
}

bool YouTubePlayById(const std::wstring& videoId) {
    CleanupYouTubeTempFiles();

    // Cancel any pending hybrid swap from a previous play — we're moving on.
    g_ytHybrid.active.store(false);

    // -----------------------------------------------------------------
    // Video mode: hand the raw URL to libmpv (video + audio via mpv).
    // -----------------------------------------------------------------
    if (g_ytVideoMode) {
        std::wstring url;
        if (YouTubeGetVideoURL(videoId, url)) {
            if (LoadURL(url.c_str(), /*silentOnFail=*/true)) {
                Speak(Ts("Playing video"));
                return true;
            }
        }
        Speak(Ts("Failed to load video"));
        return false;
    }

    // -----------------------------------------------------------------
    // Cache hit: play instantly with BASS, all effects active.
    // Before playing, swap in any pending .refresh file from a previous
    // background re-download. After starting playback, if this cached
    // file is missing the v2 marker (i.e. predates --embed-chapters)
    // kick off a silent background refresh — the next play of this same
    // video will pick up the chapters.
    // -----------------------------------------------------------------
    if (YouTubeIsAudioCached(videoId)) {
        std::wstring safeId = SanitizeForCommandLine(videoId);
        ApplyPendingCacheRefresh(safeId);
        std::wstring localFile;
        if (YouTubeDownloadAudio(videoId, localFile) && LoadFile(localFile.c_str())) {
            Speak(Ts("Playing from cache"));
            // If LoadFile populated chapters, this file is already good —
            // mark it so we don't try to refresh it later. Otherwise (no
            // chapters AND no marker = pre-v1.0.11 cache or v1.0.11 file
            // that just hasn't been marked yet), trigger a silent
            // background refresh.
            if (!g_chapters.empty()) {
                WriteCacheVersionMarker(safeId);
            } else {
                StartCacheRefreshIfNeeded(safeId);
            }
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Cache miss + libmpv available → HYBRID: stream now, BASS later.
    // We disable mpv's video output so no video window pops up — we only
    // want the audio while we wait for the BASS download to finish.
    // -----------------------------------------------------------------
    if (IsMPVAvailable()) {
        // Ensure mpv is initialized so the audio-only property sticks for
        // the next load. InitMPV is idempotent.
        InitMPV(g_videoHwnd);
        MPVSetAudioOnly(true);

        std::wstring rawUrl;
        if (YouTubeGetVideoURL(videoId, rawUrl) &&
            LoadURL(rawUrl.c_str(), /*silentOnFail=*/true))
        {
            // LoadURL routed to libmpv showed the video window — hide it,
            // we're audio-only here. Reset main window to audio size too.
            if (g_videoHwnd && IsWindowVisible(g_videoHwnd)) {
                ShowWindow(g_videoHwnd, SW_HIDE);
                SetWindowPos(g_hwnd, nullptr, 0, 0, 500, 150, SWP_NOMOVE | SWP_NOZORDER);
                g_isVideoPlaying = false;
            }

            Speak(Ts("Streaming, effects will activate shortly"));
            g_ytHybrid.videoId = videoId;
            g_ytHybrid.active.store(true);
            std::wstring* idCopy = new std::wstring(videoId);
            HANDLE t = CreateThread(nullptr, 0, HybridDownloadThread, idCopy, 0, nullptr);
            if (t) CloseHandle(t);
            return true;
        }
        // Stream failed: re-enable video for next time (e.g. real video playback).
        MPVSetAudioOnly(false);
    }

    // -----------------------------------------------------------------
    // Last resort: classic blocking download → BASS file.
    // -----------------------------------------------------------------
    std::wstring localFile;
    Speak(Ts("Downloading audio"));
    if (YouTubeDownloadAudio(videoId, localFile) && LoadFile(localFile.c_str())) {
        Speak(Ts("Playing"));
        return true;
    }

    Speak(Ts("Failed to play this video"));
    return false;
}

// Check if input is a YouTube URL
bool IsYouTubeURL(const std::wstring& input) {
    return input.find(L"youtube.com") != std::wstring::npos ||
           input.find(L"youtu.be") != std::wstring::npos;
}

// Parse YouTube URL
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel) {
    isPlaylist = false;
    isChannel = false;
    id.clear();

    // Check for playlist
    size_t listPos = url.find(L"list=");
    if (listPos != std::wstring::npos) {
        size_t start = listPos + 5;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        isPlaylist = true;
        return !id.empty();
    }

    // Check for channel
    if (url.find(L"/channel/") != std::wstring::npos || url.find(L"/@") != std::wstring::npos) {
        isChannel = true;
        // Extract channel ID or handle
        size_t pos = url.find(L"/channel/");
        if (pos != std::wstring::npos) {
            pos += 9;
        } else {
            pos = url.find(L"/@");
            if (pos != std::wstring::npos) pos += 2;
        }
        if (pos != std::wstring::npos) {
            size_t end = url.find_first_of(L"/?# ", pos);
            id = url.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            return !id.empty();
        }
    }

    // Check for video ID
    size_t vPos = url.find(L"v=");
    if (vPos != std::wstring::npos) {
        size_t start = vPos + 2;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    // youtu.be format
    size_t bePos = url.find(L"youtu.be/");
    if (bePos != std::wstring::npos) {
        size_t start = bePos + 9;
        size_t end = url.find_first_of(L"?# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    return false;
}

// Update results list in dialog
static void UpdateResultsList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    for (const auto& result : g_ytResults) {
        std::wstring display = result.title;
        if (!result.channel.empty()) {
            display += L" - " + result.channel;
        }
        if (!result.duration.empty()) {
            display += L" [" + result.duration + L"]";
        }
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
    }

    // Update load more button visibility
    HWND hLoadMore = GetDlgItem(hwnd, IDC_YT_LOADMORE);
    EnableWindow(hLoadMore, !g_ytNextPageToken.empty());
}

// Perform search
static void DoSearch(HWND hwnd) {
    wchar_t query[512];
    GetDlgItemTextW(hwnd, IDC_YT_SEARCH, query, 512);

    if (wcslen(query) == 0) return;

    g_ytCurrentQuery = query;
    g_ytResults.clear();
    g_ytNextPageToken.clear();
    g_ytIsPlaylistView = false;

    // Check if it's a YouTube URL
    if (IsYouTubeURL(query)) {
        std::wstring id;
        bool isPlaylist, isChannel;
        if (ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
            if (isPlaylist) {
                g_ytIsPlaylistView = true;
                g_ytCurrentPlaylistId = id;
                YouTubeGetPlaylistContents(id, g_ytResults, g_ytNextPageToken, L"");
                UpdateResultsList(hwnd);
                Speak(Ts("Playlist loaded"));
                return;
            } else if (!isPlaylist && !isChannel) {
                // Single video — let YouTubePlayById handle download/playback.
                // v1.60 — channel/title unknown at this point (no search
                // result row). Mark as YouTube; MPV/yt-dlp will fill in
                // the item via MPVGetMediaTitle and UpdateWindowTitle.
                SetNowPlaying(SourceType::YouTube, L"YouTube", L"");
                YouTubePlayById(id);
                return;
            }
        }
    }

    // Regular search
    Speak(Ts("Searching"));
    if (YouTubeSearch(query, g_ytResults, g_ytNextPageToken, L"")) {
        UpdateResultsList(hwnd);
        char buf[64];
        snprintf(buf, sizeof(buf), Ts("%d results").c_str(), static_cast<int>(g_ytResults.size()));
        Speak(buf);
    } else {
        Speak(Ts("No results or search failed"));
    }
}

// Load more results
static void DoLoadMore(HWND hwnd) {
    if (g_ytNextPageToken.empty()) return;

    std::vector<YouTubeResult> moreResults;
    std::wstring newToken;

    Speak(Ts("Loading more"));
    if (g_ytIsPlaylistView) {
        YouTubeGetPlaylistContents(g_ytCurrentPlaylistId, moreResults, newToken, g_ytNextPageToken);
    } else {
        YouTubeSearch(g_ytCurrentQuery, moreResults, newToken, g_ytNextPageToken);
    }

    g_ytNextPageToken = newToken;
    for (const auto& r : moreResults) {
        g_ytResults.push_back(r);
    }
    UpdateResultsList(hwnd);

    char buf[64];
    snprintf(buf, sizeof(buf), Ts("%d more loaded").c_str(), static_cast<int>(moreResults.size()));
    Speak(buf);
}

// ============================================================
// Infinite scroll (v1.0.14)
// ============================================================
//
// When the user navigates to the last item in the results list with the
// arrow keys we transparently load the next page in the background — the
// AYDP behaviour. A single atomic flag guards against multiple concurrent
// loads (auto-key-repeat, fast scrolling).

#include <atomic>

static std::atomic<bool> g_ytLoadingMore{false};

struct YtLoadMoreData {
    HWND hDlg;
    int previousSelection;          // restore after append so focus stays put
    std::vector<YouTubeResult> results;
    std::wstring newToken;
};

static DWORD WINAPI LoadMoreThreadProc(LPVOID arg) {
    YtLoadMoreData* d = static_cast<YtLoadMoreData*>(arg);

    // Snapshot the request state (g_ytCurrentQuery / token / mode) at the
    // moment we start. If the user changes search while we're mid-fetch
    // the result is discarded by the message handler (token/state check).
    bool isPlaylist = g_ytIsPlaylistView;
    std::wstring playlistId = g_ytCurrentPlaylistId;
    std::wstring query = g_ytCurrentQuery;
    std::wstring pageToken = g_ytNextPageToken;

    if (isPlaylist) {
        YouTubeGetPlaylistContents(playlistId, d->results, d->newToken, pageToken);
    } else {
        YouTubeSearch(query, d->results, d->newToken, pageToken);
    }
    PostMessageW(d->hDlg, WM_YT_LOAD_MORE_DONE, 0, reinterpret_cast<LPARAM>(d));
    return 0;
}

// Triggered from LBN_SELCHANGE when the user lands on the last item.
// No-ops if a load is already in flight or if there's no next page.
static void TriggerAutoLoadMore(HWND hwnd, int selection) {
    if (g_ytLoadingMore.load()) return;
    if (g_ytNextPageToken.empty()) return;

    g_ytLoadingMore.store(true);
    Speak(Ts("Loading more"));

    YtLoadMoreData* d = new YtLoadMoreData;
    d->hDlg = hwnd;
    d->previousSelection = selection;

    HANDLE t = CreateThread(nullptr, 0, LoadMoreThreadProc, d, 0, nullptr);
    if (!t) {
        delete d;
        g_ytLoadingMore.store(false);
    } else {
        CloseHandle(t);
    }
}

// Play selected result
static void PlaySelected(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) return;

    const YouTubeResult& result = g_ytResults[sel];
    // v1.60 — preset YouTube channel + video title BEFORE the engine
    // pipeline so the window shows it even before mpv/yt-dlp finishes
    // resolving the stream. The (still-empty) item gets refreshed later
    // when MPVGetMediaTitle returns something concrete.
    SetNowPlaying(SourceType::YouTube, result.channel, result.title);
    YouTubePlayById(result.videoId);
}

// Permanently download the selected result to Downloads\MediaAccess\YouTube.
static void DownloadSelected(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) return;

    const YouTubeResult& result = g_ytResults[sel];
    if (result.videoId.empty()) {
        Speak(Ts("Cannot download this item"));
        return;
    }

    Speak(Ts("Downloading"));
    std::wstring saved;
    if (YouTubeDownloadPermanent(result.videoId, result.title, saved)) {
        // Speak the saved filename so the user knows where it landed.
        const wchar_t* fname = wcsrchr(saved.c_str(), L'\\');
        std::wstring name = fname ? (fname + 1) : saved;
        SpeakW((std::wstring(T("Downloaded: ")) + name).c_str());
    } else {
        Speak(Ts("Download failed"));
    }
}


// ============================================================
// Format picker (v1.95) — "Download with options..."
// ============================================================
//
// Lets the user pick any format yt-dlp offers (resolutions, codecs,
// audio-only, video-only, sizes) before downloading. The simple m4a
// Download button above is untouched.

// Column layout for the SysListView32 (LVS_REPORT). Widths in pixels.
// v1.97 — The internal yt-dlp format id (e.g. "135") was removed from the
// display: it is meaningless to users and only used internally. Selection is
// mapped by row index into the formats vector, not by the id column, so
// dropping it is safe. Columns now start at Extension.
static const struct { const char* en; int width; } kYtfColumns[] = {
    { "Extension",   80 },
    { "Resolution",  100 },
    { "Video codec", 120 },
    { "Audio codec", 120 },
    { "Size",        100 },
    { "Note",        150 },
};

// Dialog proc for IDD_YT_FORMATS. lParam at WM_INITDIALOG points to the
// std::vector<YtFormat>* to display. The chosen format_id is stored in
// g_ytfChosenFormatId on IDOK.
static std::wstring g_ytfChosenFormatId;
// True when the chosen format is video-only (acodec == "none"); the download
// path then merges in the best audio track.
static bool g_ytfChosenVideoOnly = false;

static INT_PTR CALLBACK YtFormatsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static const std::vector<YtFormat>* s_formats = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            s_formats = reinterpret_cast<const std::vector<YtFormat>*>(lParam);

            HWND hList = GetDlgItem(hwnd, IDC_YTF_LIST);
            ListView_SetExtendedListViewStyle(
                hList, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

            // Columns
            for (int c = 0; c < static_cast<int>(_countof(kYtfColumns)); ++c) {
                LVCOLUMNW col = {};
                col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                col.iSubItem = c;
                col.cx = kYtfColumns[c].width;
                std::wstring hdr = T(kYtfColumns[c].en);
                col.pszText = const_cast<wchar_t*>(hdr.c_str());
                ListView_InsertColumn(hList, c, &col);
            }

            // Rows
            const wchar_t* dash = L"—";  // em dash for "none"
            if (s_formats) {
                int row = 0;
                for (const auto& f : *s_formats) {
                    // Column 0 is now Extension (Format ID column removed).
                    LVITEMW it = {};
                    it.mask = LVIF_TEXT;
                    it.iItem = row;
                    it.iSubItem = 0;
                    it.pszText = const_cast<wchar_t*>(f.ext.c_str());
                    int idx = ListView_InsertItem(hList, &it);

                    ListView_SetItemText(hList, idx, 1,
                        const_cast<wchar_t*>(f.resolution.c_str()));
                    ListView_SetItemText(hList, idx, 2,
                        const_cast<wchar_t*>(f.vcodec == L"none" ? dash : f.vcodec.c_str()));
                    ListView_SetItemText(hList, idx, 3,
                        const_cast<wchar_t*>(f.acodec == L"none" ? dash : f.acodec.c_str()));
                    ListView_SetItemText(hList, idx, 4,
                        const_cast<wchar_t*>(f.sizeStr.c_str()));
                    ListView_SetItemText(hList, idx, 5,
                        const_cast<wchar_t*>(f.note.c_str()));
                    ++row;
                }
            }

            // Preselect the first row (best quality) and focus the list.
            if (s_formats && !s_formats->empty()) {
                ListView_SetItemState(hList, 0,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            SetFocus(hList);

            // Announce the format count for screen-reader users.
            if (s_formats) {
                char buf[64];
                snprintf(buf, sizeof(buf), Ts("%d formats available").c_str(),
                         static_cast<int>(s_formats->size()));
                Speak(buf);
            }
            return FALSE;  // focus set manually
        }

        case WM_NOTIFY: {
            LPNMHDR nh = reinterpret_cast<LPNMHDR>(lParam);
            if (nh && nh->idFrom == IDC_YTF_LIST &&
                (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE)) {
                // Double-click / Enter on an item == OK.
                SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    HWND hList = GetDlgItem(hwnd, IDC_YTF_LIST);
                    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                    if (sel < 0) sel = 0;  // fall back to first
                    if (s_formats && sel >= 0 &&
                        sel < static_cast<int>(s_formats->size())) {
                        g_ytfChosenFormatId = (*s_formats)[sel].formatId;
                        g_ytfChosenVideoOnly = ((*s_formats)[sel].acodec == L"none");
                        EndDialog(hwnd, IDOK);
                    } else {
                        EndDialog(hwnd, IDCANCEL);
                    }
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Show the modal format picker. Returns true and fills chosenFormatId when the
// user confirms a format; false on cancel or when no formats are available.
static bool ShowFormatPicker(HWND parent, const std::wstring& videoId,
                             const std::wstring& title, std::wstring& chosenFormatId,
                             bool& chosenVideoOnly) {
    (void)title;  // reserved for a future caption; download uses it separately
    std::vector<YtFormat> formats = ParseFormatsArray(videoId);
    if (formats.empty()) {
        Speak(Ts("No formats available"));
        return false;
    }

    g_ytfChosenFormatId.clear();
    g_ytfChosenVideoOnly = false;
    INT_PTR r = DialogBoxParamW(GetModuleHandle(nullptr),
                                MAKEINTRESOURCEW(IDD_YT_FORMATS),
                                parent, YtFormatsDialogProc,
                                reinterpret_cast<LPARAM>(&formats));
    if (r == IDOK && !g_ytfChosenFormatId.empty()) {
        chosenFormatId = g_ytfChosenFormatId;
        chosenVideoOnly = g_ytfChosenVideoOnly;
        return true;
    }
    return false;
}

// "Download with options..." — let the user pick a format, then download it.
static void DownloadSelectedWithOptions(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) return;

    const YouTubeResult& result = g_ytResults[sel];
    if (result.videoId.empty()) {
        Speak(Ts("Cannot download this item"));
        return;
    }

    std::wstring formatId;
    bool videoOnly = false;
    if (!ShowFormatPicker(hwnd, result.videoId, result.title, formatId, videoOnly)) {
        // User cancelled, or no formats — picker already spoke if needed.
        return;
    }

    Speak(Ts("Downloading"));
    std::wstring saved;
    if (YouTubeDownloadFormat(result.videoId, result.title, formatId, videoOnly, saved)) {
        const wchar_t* fname = wcsrchr(saved.c_str(), L'\\');
        std::wstring name = fname ? (fname + 1) : saved;
        SpeakW((std::wstring(T("Downloaded: ")) + name).c_str());
    } else {
        Speak(Ts("Download failed"));
    }
}


// Dialog procedure
INT_PTR CALLBACK YouTubeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            g_ytDialog = hwnd;
            g_ytResults.clear();
            g_ytNextPageToken.clear();
            SetFocus(GetDlgItem(hwnd, IDC_YT_SEARCH));
            return FALSE;  // We set focus manually

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK:
                    // Enter pressed - check if search box has focus
                    if (GetFocus() == GetDlgItem(hwnd, IDC_YT_SEARCH)) {
                        DoSearch(hwnd);
                        return TRUE;
                    }
                    // If results list has focus, play selected
                    if (GetFocus() == GetDlgItem(hwnd, IDC_YT_RESULTS)) {
                        PlaySelected(hwnd);
                        return TRUE;
                    }
                    return TRUE;  // Prevent dialog from closing

                case IDC_YT_RESULTS:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        PlaySelected(hwnd);
                    } else if (HIWORD(wParam) == LBN_SELCHANGE) {
                        // Auto-load more when the user reaches the last
                        // item (AYDP-style infinite scroll).
                        HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
                        if (sel >= 0 && sel == count - 1) {
                            TriggerAutoLoadMore(hwnd, sel);
                        }
                    }
                    break;

                case IDC_YT_LOADMORE:
                    DoLoadMore(hwnd);
                    break;

                case IDC_YT_DOWNLOAD:
                    DownloadSelected(hwnd);
                    break;

                case IDC_YT_DOWNLOAD_OPTS:
                    DownloadSelectedWithOptions(hwnd);
                    break;

                case IDCANCEL:
                    DestroyWindow(hwnd);
                    g_ytDialog = nullptr;
                    return TRUE;
            }
            break;

        case WM_YT_LOAD_MORE_DONE: {
            // Background load-more finished. Append the new results to
            // the listbox, restore the user's selection, and announce a
            // short count. If the dialog has been closed or a different
            // search has started, we silently drop the result.
            YtLoadMoreData* d = reinterpret_cast<YtLoadMoreData*>(lParam);
            if (d) {
                if (d->hDlg == hwnd) {
                    g_ytNextPageToken = d->newToken;
                    for (const auto& r : d->results) g_ytResults.push_back(r);
                    UpdateResultsList(hwnd);

                    // LB_SETCURSEL does NOT fire LBN_SELCHANGE, so this
                    // won't re-trigger the loader.
                    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
                    SendMessageW(hList, LB_SETCURSEL, d->previousSelection, 0);

                    if (!d->results.empty()) {
                        char buf[64];
                        snprintf(buf, sizeof(buf),
                                 Ts("%d more loaded").c_str(),
                                 static_cast<int>(d->results.size()));
                        Speak(buf);
                    }
                }
                delete d;
            }
            g_ytLoadingMore.store(false);
            return TRUE;
        }

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Resize controls
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_SEARCH), nullptr, 7, 22, width - 14, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_RESULTS), nullptr, 7, 54, width - 14, height - 90, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_LOADMORE), nullptr, 7, height - 30, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_DOWNLOAD), nullptr, 73, height - 30, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_DOWNLOAD_OPTS), nullptr, 139, height - 30, 110, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, width - 57, height - 30, 50, 14, SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 400;
            mmi->ptMinTrackSize.y = 300;
            return TRUE;
        }

        case WM_DESTROY:
            g_ytDialog = nullptr;
            break;
    }
    return FALSE;
}

// Show YouTube dialog
void ShowYouTubeDialog(HWND parent) {
    if (g_ytDialog) {
        // Already open, bring to front
        SetForegroundWindow(g_ytDialog);
        return;
    }

    // Check if the YouTube extractor is available (bundled — should always be)
    if (!IsYtdlpAvailable()) {
        MessageBoxW(parent, T("The YouTube extractor was not found. Please reinstall MediaAccess."),
                    T("YouTube"), MB_ICONWARNING);
        return;
    }

    g_ytDialog = CreateDialogW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_YOUTUBE),
                               parent, YouTubeDlgProc);
    if (g_ytDialog) {
        ShowWindow(g_ytDialog, SW_SHOW);
    }
}

// Get YouTube dialog handle
HWND GetYouTubeDialog() {
    return g_ytDialog;
}

// Cleanup temporary files and resources
void YouTubeCleanup() {
    CleanupYouTubeTempFiles();
    g_ytResults.clear();
    g_ytNextPageToken.clear();
    g_ytCurrentQuery.clear();
    g_ytCurrentPlaylistId.clear();
}
