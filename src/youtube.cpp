#include "youtube.h"
#include "globals.h"
#include "utils.h"
#include "player.h"
#include "accessibility.h"
#include "translations.h"
#include "video_engine.h"  // IsMPVAvailable() — used as YouTube playback fallback
#include "resource.h"
#include <wininet.h>
#include <shlwapi.h>
#include <shlobj.h>      // SHGetFolderPathW, CSIDL_*
#include <knownfolders.h>// FOLDERID_Downloads
#include <regex>
#include <sstream>

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
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results);
static std::wstring RunYtdlp(const std::wstring& args);
static std::wstring UrlEncode(const std::wstring& str);
static std::wstring HttpGet(const std::wstring& url);
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key);
static std::vector<std::wstring> ParseJsonArray(const std::wstring& json, const std::wstring& arrayKey);

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

// Simple JSON string value parser (not a full JSON parser)
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

    std::wstring value = json.substr(startQuote + 1, endQuote - startQuote - 1);
    // Unescape basic sequences
    size_t pos = 0;
    while ((pos = value.find(L"\\n", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L" ");
    }
    pos = 0;
    while ((pos = value.find(L"\\\"", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L"\"");
    }
    return value;
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

// Search using yt-dlp
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results) {
    // Use yt-dlp to search YouTube
    std::wstring safeQuery = SanitizeForCommandLine(query);
    std::wstring args = L"--flat-playlist --dump-json \"ytsearch25:" + safeQuery + L"\"";
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

    // Fall back to yt-dlp (only for first page, no pagination support)
    if (pageToken.empty() && IsYtdlpAvailable()) {
        OutputDebugStringW(L"[YT] Trying yt-dlp search\n");
        return SearchWithYtdlp(query, results);
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
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring args = L"-f \"bestaudio[ext=m4a]/bestaudio[acodec=aac]/140\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    RunYtdlp(args);

    cached = FindCachedAudio(safeId);
    if (!cached.empty()) { outFilePath = cached; return true; }
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

// Returns Downloads\MediaAccess\YouTube, creating directories as needed.
static std::wstring GetDownloadsTargetDir() {
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
                        L"--no-overwrites "
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

    // Snapshot the mpv playback position so we can resume seamlessly.
    // GetCurrentPosition() returns mpv's reading via the player.cpp facade.
    double pos = GetCurrentPosition();

    if (!LoadFile(localFile.c_str())) return;
    if (pos > 1.0) SeekToPosition(pos);
    Speak(Ts("Effects activated"));
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
    // -----------------------------------------------------------------
    if (YouTubeIsAudioCached(videoId)) {
        std::wstring localFile;
        if (YouTubeDownloadAudio(videoId, localFile) && LoadFile(localFile.c_str())) {
            Speak(Ts("Playing from cache"));
            return true;
        }
    }

    // -----------------------------------------------------------------
    // Cache miss + libmpv available → HYBRID: stream now, BASS later.
    // -----------------------------------------------------------------
    if (IsMPVAvailable()) {
        std::wstring rawUrl;
        if (YouTubeGetVideoURL(videoId, rawUrl) &&
            LoadURL(rawUrl.c_str(), /*silentOnFail=*/true))
        {
            Speak(Ts("Streaming, effects will activate shortly"));
            g_ytHybrid.videoId = videoId;
            g_ytHybrid.active.store(true);
            std::wstring* idCopy = new std::wstring(videoId);
            HANDLE t = CreateThread(nullptr, 0, HybridDownloadThread, idCopy, 0, nullptr);
            if (t) CloseHandle(t);
            return true;
        }
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

// Play selected result
static void PlaySelected(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) return;

    const YouTubeResult& result = g_ytResults[sel];
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
                    }
                    break;

                case IDC_YT_LOADMORE:
                    DoLoadMore(hwnd);
                    break;

                case IDC_YT_DOWNLOAD:
                    DownloadSelected(hwnd);
                    break;

                case IDCANCEL:
                    DestroyWindow(hwnd);
                    g_ytDialog = nullptr;
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            // Resize controls
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_SEARCH), nullptr, 7, 22, width - 14, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_RESULTS), nullptr, 7, 54, width - 14, height - 90, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_YT_LOADMORE), nullptr, 7, height - 30, 60, 14, SWP_NOZORDER);
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

    // Check if yt-dlp is available
    if (!IsYtdlpAvailable()) {
        MessageBoxW(parent, T("yt-dlp is not configured. Please set the yt-dlp path in Options > YouTube tab."),
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
