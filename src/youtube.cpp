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
// v2.07 — UIA notification approach (v2.06) removed: on a plain Win32 dialog
// the host provider is silently ignored by NVDA. AnnounceStatus now uses the
// reliable deferred-Speak path; no UIAutomation include needed.
#include <oleauto.h>     // v2.06 — SysAllocString / SysFreeString for the UIA BSTRs
#include <regex>
#include <sstream>
#include <algorithm>
#include <vector>
#include <atomic>    // B (v2.00): g_ytLastFailureKey is shared across threads
#include <cwctype>   // towupper / towlower for filename + stderr classification

#pragma comment(lib, "wininet.lib")

// m4 (v1.99): the "[YT] ..." debug traces below log the full yt-dlp command
// line (which embeds the user's search query) and the bodies of API/yt-dlp
// responses. Shipping those unconditionally leaks user activity to any tool
// that captures OutputDebugString (DebugView, attached debuggers). Gate them
// behind a build-time switch so release binaries stay silent. Define _DEBUG
// (default in debug builds) or YT_TRACE to re-enable.
#if defined(_DEBUG) || defined(YT_TRACE)
#define YT_DBG(msg) OutputDebugStringW((msg))
#else
#define YT_DBG(msg) ((void)0)
#endif

// Sanitize user input for safe inclusion in command-line arguments.
// Removes characters that could break out of quoting or enable command injection.
//
// CONTRACT / INVARIANT (m5, v2.03): the returned value is NOT self-quoting. Every
// caller MUST embed the result inside double quotes ("...RESULT...") on the
// command line. This function only guarantees that the result cannot break OUT of
// such quoting: it drops the shell metacharacters that matter for CreateProcess
// (" & | > < ^ %) and strips any trailing backslash run (which would otherwise
// escape the closing quote). It does NOT add the surrounding quotes itself — an
// unquoted result containing spaces would still split into multiple arguments.
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
    // m2 (v1.99): drop any run of trailing backslashes. The sanitized value is
    // always embedded inside double quotes ("...VALUE..."), and on Windows a
    // backslash immediately before the closing quote escapes it (\" → literal
    // quote), which would swallow the quote and malform the whole command line.
    // Removing trailing backslashes makes the closing quote always literal.
    while (!result.empty() && result.back() == L'\\') {
        result.pop_back();
    }
    return result;
}

// I (v2.00 / m3 v2.01): max length (wide chars) of a YouTube id at the public
// boundary. Real video ids are 11 chars; playlist ids are longer but short —
// 64 is a generous ceiling. Shared by IsValidVideoId / IsValidFormatId.
static constexpr int YT_ID_MAX_LEN = 64;

// C (v2.00): strict WHITELIST validator for a YouTube video / playlist id at the
// PUBLIC API boundary. SanitizeForCommandLine (above) is a *blacklist* — it
// drops a handful of shell metacharacters but PRESERVES an interior backslash,
// so an id like "..\\..\\evil" passed straight into YouTubeDownloadAudio /
// PlayById / DownloadPermanent / DownloadFormat / ParseFormatsArray /
// GetStreamURL would survive and become part of a cache/output FILE PATH or a
// watch?v= URL → path traversal on the writes. The UI never feeds such an id
// (paste goes through SanitizeYtId, which is a whitelist), but every public
// entry point must defend its own boundary rather than trust the caller.
//
// Accepts only the YouTube id alphabet [A-Za-z0-9_-], non-empty, with a sane
// length cap (real video ids are 11 chars; playlist ids are longer but still
// short — 64 is a generous ceiling). Anything else → reject, and the caller
// bails out BEFORE building any path or command line. Mirrors the alphabet used
// by IsYtIdChar / SanitizeYtId (defined later) and IsValidFormatId.
static bool IsValidVideoId(const std::wstring& id) {
    if (id.empty() || id.size() > YT_ID_MAX_LEN) return false;
    for (wchar_t c : id) {
        bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                  (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

// Dialog state
static HWND g_ytDialog = nullptr;
static std::vector<YouTubeResult> g_ytResults;
static std::wstring g_ytNextPageToken;
static std::wstring g_ytCurrentQuery;
static bool g_ytIsPlaylistView = false;
static std::wstring g_ytCurrentPlaylistId;

// Holds the spoken-failure key produced by the most recent yt-dlp / API query,
// so the caller can voice a specific reason instead of a blanket "no results".
// Set by the query functions (SearchWithAPI / SearchWithYtdlp / ParseFormats-
// Array / the download fns); consumed by the UI / worker handlers. Each worker
// reads it on its OWN thread immediately after the query (queries/downloads are
// serialized by their respective atomic flags), then copies it into the heap
// result so the UI thread never races a concurrent query. Declared here (ahead
// of SearchWithAPI) so every query function can set it.
//
// B (v2.00): made std::atomic. The PLAYBACK download path runs YouTubeDownload-
// Audio on HybridDownloadThread / RefreshCacheThread — threads NOT serialized by
// any of the foreground atomic flags (g_ytSearching / g_ytDownloading / …). That
// thread WRITES this global while a foreground worker may READ it. The values
// are always immortal string literals (from ClassifyYtdlpFailure or the inline
// "Network error…" constants), so there is no lifetime hazard — but plain
// non-atomic access is still a data race (UB) and could publish a stale/torn
// pointer, attributing the wrong reason to a failure. An atomic<const char*>
// with relaxed-default store/load makes every publish/consume well-defined while
// staying a single word load/store. Each worker still copies ITS key into the
// heap result immediately after its own query, so the UI never reads it late.
static std::atomic<const char*> g_ytLastFailureKey{nullptr};

// Forward declarations
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken);
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results, int count = 50);
static std::wstring UrlEncode(const std::wstring& str);
static std::wstring HttpGet(const std::wstring& url);
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key);

// RunYtdlp is declared in youtube.h (constants + signature) so the async lot 2
// can drive it from another translation unit.

// ============================================================
// Shared constants
// ============================================================

// Read-pipe buffer size for draining yt-dlp's stdout/stderr. 64 KiB is large
// enough to swallow a multi-format JSON line in a couple of reads without
// over-allocating. Used by both RunYtdlp pipe drains.
static constexpr DWORD YTDLP_PIPE_BUFFER = 64 * 1024;

// Wake interval (ms) for the pipe-drain loop. The loop blocks on the child
// handle for this long each iteration (WaitForSingleObject), so it returns
// INSTANTLY when the child exits (no fixed latency) and otherwise wakes at most
// once per interval to drain the pipes (no busy-poll). Small enough that a
// timeout fires promptly, large enough that an active download costs ~no CPU.
static constexpr DWORD YTDLP_POLL_INTERVAL_MS = 50;

// I (v2.00): hard cap on captured stdout. Every RunYtdlp caller wants either a
// single JSON line, a formats array, or a URL — all comfortably under a few MiB.
// Without a cap, a pathological / hostile yt-dlp stdout (or a runaway --print)
// could grow the std::string until the process OOMs. 32 MiB is orders of
// magnitude above any legitimate payload; crossing it means the output is junk,
// so we kill the child and return what we have. (stderr is bounded the same way.)
static constexpr size_t YTDLP_MAX_STDOUT_BYTES = 32u * 1024u * 1024u;

// Maximum length (in wide chars) of a sanitized download filename. Windows
// MAX_PATH is 260; capping the base name at 150 leaves comfortable room for
// the destination directory path and the file extension yt-dlp appends.
static constexpr size_t MAX_FILENAME_LEN = 150;

// Shared yt-dlp audio format selector for every AUDIO path (stream-URL probe,
// cache fill, background refresh, permanent download). Prefer M4A/AAC because
// BASS decodes that natively (bass_aac.dll); plain "bestaudio" usually returns
// WebM/Opus which BASS cannot demux. Fallback chain:
//   1. Any m4a container (AAC).
//   2. Any AAC codec regardless of container.
//   3. mp3 (rare on YouTube, but BASS-decodable and a valid cache ext).
//   4. itag 140 — YouTube's universal 128 kbps AAC stream.
// m3 (v2.01): hoisted to a single source of truth. Previously YouTubeGetStreamURL
// carried the mp3 step while the three download paths omitted it — an accidental
// drift, not a deliberate difference (all four want the same BASS-friendly audio
// and every probe set already includes .mp3). Unified here so they can't diverge.
static const wchar_t* const kAudioFormatSelector =
    L"bestaudio[ext=m4a]/bestaudio[acodec=aac]/bestaudio[ext=mp3]/140";

// Extensions yt-dlp may emit for an AUDIO cache/download (probed in order when
// locating the file it produced). Hoisted here so every probe site agrees.
static const wchar_t* const kAudioExts[] = {
    L".m4a", L".mp4", L".aac", L".mp3", L".webm", L".opus"
};
// Extensions the cache-refresh path produces (audio, no webm/opus — refresh
// always asks for an AAC/M4A-friendly stream).
static const wchar_t* const kRefreshExts[] = {
    L".m4a", L".mp4", L".aac", L".mp3"
};
// Extensions YouTubeDownloadFormat may emit (audio OR video OR merged).
static const wchar_t* const kFormatExts[] = {
    L".mp4", L".m4a", L".aac", L".mp3", L".webm",
    L".mkv", L".flac", L".opus", L".wav"
};

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

// Simple HTTP GET request. m2 (v2.03): optional out-param `httpStatus` receives
// the HTTP response status code (e.g. 200, 400, 403) so the caller can tell an
// API-level rejection (bad key → 400, quota exceeded → 403) apart from a genuine
// network/DNS/TLS failure (no response at all → status stays 0). On any transport
// failure the status is left at 0 and the returned body is empty.
static std::wstring HttpGet(const std::wstring& url, int* httpStatus = nullptr) {
    if (httpStatus) *httpStatus = 0;
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"MediaAccess/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (hConnect) {
        // Read the HTTP status code (HTTP_QUERY_STATUS_CODE) before draining the
        // body. A 4xx/5xx still has a (JSON error) body we read below.
        if (httpStatus) {
            DWORD code = 0;
            DWORD sz = sizeof(code);
            DWORD idx = 0;
            if (HttpQueryInfoW(hConnect,
                    HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                    &code, &sz, &idx)) {
                *httpStatus = static_cast<int>(code);
            }
        }
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

    // m1 (v2.03): find the CLOSING quote. A quote is only the terminator if it is
    // NOT escaped. The previous test (json[endQuote-1] != '\\') wrongly treated a
    // quote preceded by an ESCAPED backslash ("...\\\\\"") as escaped, so a value
    // ending in a backslash (e.g. a Windows path or "foo\\\\") over-captured past
    // its real end. Mirror SplitJsonObjects: count the run of backslashes
    // immediately before the quote — an EVEN count (incl. zero) means the quote is
    // a real terminator; an ODD count means it is escaped (\") and we continue.
    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == L'"') {
            size_t backslashes = 0;
            size_t b = endQuote;
            while (b > startQuote + 1 && json[b - 1] == L'\\') { ++backslashes; --b; }
            if ((backslashes % 2) == 0) break;  // unescaped quote → string ends
        }
        endQuote++;
    }

    return JsonUnescape(json.substr(startQuote + 1, endQuote - startQuote - 1));
}

// Convert a raw UTF-8 byte buffer (as produced by yt-dlp on its pipes) into a
// wide string. Tolerant of an empty buffer (returns L"").
static std::wstring Utf8BytesToWide(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(),
                                  static_cast<int>(bytes.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(),
                        static_cast<int>(bytes.size()), &result[0], len);
    return result;
}

// m3 (v2.03): safe substitution of a single integer into a TRANSLATED template.
// The spoken count strings ("%d results", "%d more loaded", "%d formats
// available") were fed straight to snprintf with a TRANSLATION as the format
// string. A translator who accidentally typed "%s" (or any non-%d specifier, or
// a stray "%") would turn that into undefined behavior / a crash, because the
// vararg (an int) would not match. This helper never invokes printf on the
// translated text: it replaces the FIRST "%d" with the number itself and
// neutralizes nothing else (any other text, including stray '%', is copied
// verbatim and harmless). If the template lacks "%d" we append the number so the
// count is never silently lost.
static std::wstring FormatCount(const std::wstring& templ, int n) {
    std::wstring num = std::to_wstring(n);
    size_t pos = templ.find(L"%d");
    if (pos == std::wstring::npos) {
        // Defensive: translator dropped the placeholder — still voice the count.
        std::wstring out = templ;
        if (!out.empty() && out.back() != L' ') out += L' ';
        out += num;
        return out;
    }
    return templ.substr(0, pos) + num + templ.substr(pos + 2);
}

// Drain whatever bytes are currently buffered on a pipe WITHOUT blocking.
// PeekNamedPipe tells us how many bytes are available; we only ReadFile that
// many, so the call never parks the thread waiting for the child to write more.
// This is the key to honoring a timeout: a plain blocking ReadFile would sit in
// the kernel until EOF, making any timeout downstream dead code (the old bug).
// Returns false if the pipe is broken (child exited and closed its write end).
static bool DrainPipeNonBlocking(HANDLE hPipe, std::string& sink) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            // Broken pipe == child closed its write handle (usually on exit).
            return false;
        }
        if (avail == 0) return true;  // nothing buffered right now

        // m4 (v2.01): thread_local, not a stack array. A 64 KiB on-stack buffer
        // was paid TWICE per loop iteration (stdout + stderr drain) on whatever
        // thread runs RunYtdlp — including background download/refresh threads
        // with modest default stacks. thread_local moves it off the stack while
        // staying lock-free and per-thread (no sharing hazard across the two
        // pipe drains, which run sequentially on the same thread).
        static thread_local char buffer[YTDLP_PIPE_BUFFER];
        DWORD toRead = (avail < sizeof(buffer)) ? avail : (DWORD)sizeof(buffer);
        DWORD got = 0;
        if (!ReadFile(hPipe, buffer, toRead, &got, nullptr) || got == 0) {
            return false;
        }
        sink.append(buffer, got);
    }
}

// Run yt-dlp with an industrial-strength process model. See youtube.h for the
// full contract. Design notes:
//   * stdout and stderr go to SEPARATE pipes so captured stdout is clean JSON
//     /URL text (the old code merged stderr into stdout, polluting the JSON).
//   * The pipes are drained with PeekNamedPipe in a poll loop instead of a
//     blocking ReadFile, so a finite timeout actually fires.
//   * On timeout the child is TerminateProcess'd, preventing orphaned
//     yt-dlp/ffmpeg processes that the old code leaked.
//   * Every handle (both pipe ends ×2, process, thread) is closed on every
//     exit path — no leaks.
std::wstring RunYtdlp(const std::wstring& args,
                      int timeoutMs,
                      int* exitCode,
                      std::wstring* stderrOut) {
    // Default the out-params so callers see a sane value on early return.
    if (exitCode)  *exitCode = YTDLP_EXIT_LAUNCH_FAILED;
    if (stderrOut) stderrOut->clear();

    if (!IsYtdlpAvailable()) return L"";

    std::wstring cmdLine = L"\"" + g_ytdlpPath + L"\" " + args;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;          // child inherits the pipe write ends
    sa.lpSecurityDescriptor = nullptr;

    // Create two pipes: one for stdout, one for stderr. Keeping them separate
    // is what guarantees clean JSON on stdout.
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    HANDLE hErrRead = nullptr, hErrWrite = nullptr;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) return L"";
    if (!CreatePipe(&hErrRead, &hErrWrite, &sa, 0)) {
        CloseHandle(hOutRead);
        CloseHandle(hOutWrite);
        return L"";
    }
    // The READ ends must NOT be inherited by the child — only the write ends.
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput  = hOutWrite;
    si.hStdError   = hErrWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hOutRead);  CloseHandle(hOutWrite);
        CloseHandle(hErrRead);  CloseHandle(hErrWrite);
        return L"";
    }

    // Close OUR copies of the write ends. If we kept them open, the read ends
    // would never see a broken pipe (EOF) because *we* would still be a writer.
    CloseHandle(hOutWrite);  hOutWrite = nullptr;
    CloseHandle(hErrWrite);  hErrWrite = nullptr;

    std::string outBytes;
    std::string errBytes;
    bool timedOut = false;
    const DWORD startTick = GetTickCount();

    // Poll loop: drain both pipes, then check whether the child has exited or
    // we've blown the timeout. We keep draining after the child exits so we
    // don't lose the tail of its output that's still buffered in the pipe.
    bool overflowed = false;  // I (v2.00): stdout/stderr blew the size cap
    for (;;) {
        DrainPipeNonBlocking(hOutRead, outBytes);
        DrainPipeNonBlocking(hErrRead, errBytes);

        // I (v2.00): bound memory. If either capture buffer crosses the cap the
        // output is pathological — kill the child and stop draining so a runaway
        // stream can't OOM us. Treated as a failure (overflowed → non-success).
        if (outBytes.size() > YTDLP_MAX_STDOUT_BYTES ||
            errBytes.size() > YTDLP_MAX_STDOUT_BYTES) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            overflowed = true;
            break;
        }

        // Block up to one poll interval for the child to exit. This returns
        // INSTANTLY (WAIT_OBJECT_0) the moment yt-dlp terminates — so a fast
        // query no longer eats a fixed 50 ms of latency — while a long-running
        // download wakes us at most once per interval (no busy-poll). On the
        // last reawakening we still drain at the top of the loop, so no trailing
        // output is lost; the post-exit branch below adds a final drain too.
        DWORD waitStatus = WaitForSingleObject(pi.hProcess, YTDLP_POLL_INTERVAL_MS);
        if (waitStatus == WAIT_OBJECT_0) {
            // Child has exited — one final drain to capture any trailing bytes.
            DrainPipeNonBlocking(hOutRead, outBytes);
            DrainPipeNonBlocking(hErrRead, errBytes);
            break;
        }

        // Enforce the timeout only when one was requested (timeoutMs > 0).
        // timeoutMs == 0 means "wait forever" — used for downloads, which can
        // legitimately run for minutes.
        if (timeoutMs > 0 &&
            (GetTickCount() - startTick) >= static_cast<DWORD>(timeoutMs)) {
            // Kill the child so we don't leak an orphaned yt-dlp/ffmpeg.
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);  // let it tear down
            timedOut = true;
            break;
        }
    }

    // Read the real exit code (unless we already know it timed out).
    int code = YTDLP_EXIT_TIMED_OUT;
    if (!timedOut) {
        DWORD ec = 0;
        code = GetExitCodeProcess(pi.hProcess, &ec) ? static_cast<int>(ec)
                                                    : YTDLP_EXIT_LAUNCH_FAILED;
    }
    // I (v2.00): a buffer-cap overflow is a hard failure regardless of the
    // process's own exit code. Report it as a generic launch/exec failure and
    // drop the (truncated, pathological) capture so no parser is fed junk.
    if (overflowed) {
        code = YTDLP_EXIT_LAUNCH_FAILED;
        outBytes.clear();
        errBytes.clear();
    }
    if (exitCode) *exitCode = code;

    // Close every remaining handle — process, thread, read ends.
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hOutRead);
    CloseHandle(hErrRead);

    if (stderrOut) *stderrOut = Utf8BytesToWide(errBytes);
    return Utf8BytesToWide(outBytes);
}

// ============================================================
// Honest failure messages (M3)
// ============================================================
//
// Classifies a failed yt-dlp invocation into a SPECIFIC, translatable spoken
// message based on its exit code and stderr text. We only claim a cause that
// yt-dlp actually reports — anything we can't pin down gets an honest generic
// "operation failed" rather than a misleading "no results".
//
// Returns a translation KEY (English source string) suitable for Ts()/Speak().
// The corresponding FR/EN pairs are registered in translations_player.cpp.
static const char* ClassifyYtdlpFailure(int exitCode, const std::wstring& stderrText) {
    // Process never launched (binary missing / pipe failure) — distinct from a
    // network or content problem.
    if (exitCode == YTDLP_EXIT_LAUNCH_FAILED) {
        return "YouTube tool could not start";
    }
    // We killed it after the query timeout elapsed.
    if (exitCode == YTDLP_EXIT_TIMED_OUT) {
        return "YouTube request timed out";
    }

    // Lower-case the stderr once so substring checks are case-insensitive.
    std::wstring s = stderrText;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });

    auto has = [&](const wchar_t* needle) {
        return s.find(needle) != std::wstring::npos;
    };

    // A (v2.00): live stream / scheduled-premiere. yt-dlp emits messages like
    // "This live event will begin in N", "This is a live stream", "Premieres in",
    // "live event has not started". A live stream never terminates, so playing or
    // downloading it would otherwise hang the transfer until the bounded playback
    // timeout (or, on an explicit download, forever). Detect it FIRST so a live
    // message that also mentions "unavailable" isn't misclassified below.
    if (has(L"is live") || has(L"this live event") ||
        has(L"live event will begin") || has(L"live event has not") ||
        has(L"live stream") || has(L"is currently live") ||
        has(L"premieres in") || has(L"will begin in") ||
        has(L"this live stream recording is not available")) {
        return "This is a live stream and is not supported";
    }
    // Private / members-only / sign-in-required videos.
    if (has(L"private video") || has(L"members-only") ||
        has(L"sign in to confirm") || has(L"login required")) {
        return "This video is private";
    }
    // Removed / deleted / generally unavailable content.
    if (has(L"video unavailable") || has(L"has been removed") ||
        has(L"this video is no longer available") || has(L"unavailable")) {
        return "This video is unavailable";
    }
    // Geo-blocking.
    if (has(L"geo") || has(L"not available in your country") ||
        has(L"blocked it in your country")) {
        return "This video is blocked in your region";
    }
    // Network layer: DNS, connection, timeouts, TLS, "unable to download".
    if (has(L"urlopen error") || has(L"getaddrinfo") ||
        has(L"connection") || has(L"network is unreachable") ||
        has(L"timed out") || has(L"temporary failure in name resolution") ||
        has(L"ssl") || has(L"unable to download webpage")) {
        return "Network error, check your connection";
    }

    // Exited non-zero for a reason we can't classify — be honest, don't fake
    // "no results".
    return "YouTube operation failed";
}

// Search using YouTube Data API
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken) {
    if (!HasApiKey()) return false;

    std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
    url += UrlEncode(query);
    // E (v2.00): percent-encode the API key and page token before splicing them
    // into the query string. A key/token containing a URL-reserved char (&, =,
    // +, /, whitespace) would otherwise break the parameter boundary or corrupt
    // the request. They are drawn from a safe alphabet in practice, but encoding
    // at the boundary is correct and matches how `query` is already handled.
    url += L"&key=" + UrlEncode(g_ytApiKey);
    if (!pageToken.empty()) {
        url += L"&pageToken=" + UrlEncode(pageToken);
    }

    int httpStatus = 0;
    std::wstring response = HttpGet(url, &httpStatus);
    // m2 (v2.03): distinguish an API-level rejection from a transport failure.
    // 400 = malformed/invalid API key; 403 = quota exceeded / key forbidden.
    // Both come back with a status code (and usually a JSON error body); a true
    // network failure (DNS/TLS/connection) returns status 0 and an empty body.
    if (httpStatus == 400 || httpStatus == 403) {
        g_ytLastFailureKey = "YouTube API key is invalid or quota exceeded";
        return false;
    }
    if (response.empty()) {
        // m3 (v1.99): the HTTP GET failed (no connection, DNS, TLS). Previously
        // this returned false silently and — when no yt-dlp fallback was
        // available — the user heard a misleading "No results". m2 (v2.03): a
        // status >= 400 was already handled above as an API error; reaching here
        // with an empty body means a genuine transport failure (status 0).
        g_ytLastFailureKey = "Network error, check your connection";
        return false;
    }

    // Parse results (simple parsing, not full JSON)
    nextPageToken = ParseJsonString(response, L"nextPageToken");

    // Find items array and parse each item
    size_t itemsPos = response.find(L"\"items\"");
    if (itemsPos == std::wstring::npos) {
        // m2 (v2.03): a non-empty body with no "items" array is an API error
        // envelope. If we have a 4xx/5xx status it's specifically a key/quota
        // problem (already caught above for 400/403); any other no-items body
        // is an unclassifiable API error rather than a genuine empty result.
        g_ytLastFailureKey = (httpStatus >= 400)
            ? "YouTube API key is invalid or quota exceeded"
            : "YouTube operation failed";
        return false;
    }

    // m6 (v2.02): single FORWARD pass. The old loop did a `rfind("snippet")`
    // (a backward scan from each videoId to the start of the response) PLUS two
    // `substr` copies PER item — O(n²) work and O(n) garbage per item. Both
    // `"videoId"` and `"snippet"` occur in document order, so we advance ONE
    // forward cursor for snippet and remember the most recent snippet position
    // that precedes the current videoId. Behavior is identical: each videoId is
    // paired with the nearest preceding snippet (snippetPos > itemsPos), titles
    // and channels parsed from the same bounded window.
    size_t searchStart = itemsPos;
    size_t snippetCursor = itemsPos;     // next "snippet" not yet consumed
    size_t lastSnippetPos = std::wstring::npos;
    while ((searchStart = response.find(L"\"videoId\"", searchStart)) != std::wstring::npos) {
        // Advance the snippet cursor to the last "snippet" before this videoId.
        for (;;) {
            size_t next = response.find(L"\"snippet\"", snippetCursor);
            if (next == std::wstring::npos || next >= searchStart) break;
            lastSnippetPos = next;
            snippetCursor = next + 9;  // length of "\"snippet\""
        }

        YouTubeResult result;
        result.videoId = ParseJsonString(
            response.substr(searchStart, 500), L"videoId");

        if (lastSnippetPos != std::wstring::npos && lastSnippetPos > itemsPos) {
            std::wstring snippet = response.substr(
                lastSnippetPos, searchStart - lastSnippetPos + 1000);
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

// m5 (v2.02): shared parser for yt-dlp's line-delimited JSON output. Both the
// search path (SearchWithYtdlp) and the playlist path (YouTubeGetPlaylistContents)
// emit one JSON object per line via --dump-json and parsed them with byte-for-byte
// identical loops. Extracted here so the two can never drift. Each line is one
// video; we strip a trailing CR, skip non-object lines, and append any item that
// has both an id and a title. Field-name fallbacks cover yt-dlp version drift.
static void ParseYtdlpJsonLines(const std::wstring& output,
                                std::vector<YouTubeResult>& results) {
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
    // Use yt-dlp to search YouTube. --quiet/--no-warnings keep stdout pure
    // JSON (the "%(...)j"/--dump-json lines), so warnings can't corrupt our
    // line-by-line parser below.
    std::wstring safeQuery = SanitizeForCommandLine(query);
    wchar_t prefix[32];
    swprintf(prefix, 32, L"ytsearch%d:", count);
    std::wstring args = L"--flat-playlist --dump-json --quiet --no-warnings \""
                        + std::wstring(prefix) + safeQuery + L"\"";
    YT_DBG((L"[YT] Running: " + g_ytdlpPath + L" " + args + L"\n").c_str());
    int exitCode = 0;
    std::wstring stderrText;
    std::wstring output = RunYtdlp(args, YTDLP_QUERY_TIMEOUT_MS, &exitCode, &stderrText);
    // Record a specific failure reason when yt-dlp errored or produced nothing,
    // so the caller can distinguish "network/tool failure" from "no matches".
    g_ytLastFailureKey = nullptr;
    if (exitCode != 0 || output.empty()) {
        g_ytLastFailureKey = ClassifyYtdlpFailure(exitCode, stderrText);
    }
    YT_DBG((L"[YT] Output length: " + std::to_wstring(output.length()) + L"\n").c_str());
    if (output.length() < 500) {
        YT_DBG((L"[YT] Output: " + output + L"\n").c_str());
    }
    if (output.empty()) return false;

    // Parse JSON lines (each line is a video). m5 (v2.02): shared helper.
    ParseYtdlpJsonLines(output, results);

    return !results.empty();
}

// Public search function
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken,
                   const std::vector<std::wstring>* seenIdsSnapshot) {
    results.clear();
    nextPageToken.clear();

    YT_DBG(L"[YT] YouTubeSearch called\n");
    YT_DBG((L"[YT] HasApiKey: " + std::wstring(HasApiKey() ? L"yes" : L"no") + L"\n").c_str());
    YT_DBG((L"[YT] IsYtdlpAvailable: " + std::wstring(IsYtdlpAvailable() ? L"yes" : L"no") + L"\n").c_str());

    // Try API first if available
    if (HasApiKey() && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        YT_DBG(L"[YT] API search succeeded\n");
        return true;
    }

    // yt-dlp fallback. yt-dlp doesn't have native pagination — we simulate
    // it by asking for a growing count each time and only surfacing the
    // items the user hasn't seen yet (deduplicated by video id below).
    if (IsYtdlpAvailable()) {
        YT_DBG(L"[YT] Trying yt-dlp search\n");
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
            // Build the set of ids already known to the caller so we don't
            // re-emit them. Dedup against the new batch too in case yt-dlp
            // returns dups.
            //
            // M4 (v1.98): when a snapshot is supplied (the async load-more
            // worker passes one), dedup against THAT immutable copy. Reading
            // the live g_ytResults from a worker thread while the UI thread can
            // clear it during a fresh search was a use-after-free / data race.
            // Synchronous UI-thread callers pass nullptr and keep the original
            // behavior of reading g_ytResults directly (no other thread touches
            // it at that moment).
            std::vector<std::wstring> seen;
            if (seenIdsSnapshot) {
                seen = *seenIdsSnapshot;
            } else {
                seen.reserve(g_ytResults.size());
                for (const auto& r : g_ytResults) seen.push_back(r.videoId);
            }
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

    YT_DBG(L"[YT] No search method available\n");
    return false;
}

// Get playlist contents
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    // m1 (v2.02): WHITELIST the playlist id at this PUBLIC boundary, mirroring
    // every other public entry point (PlayById / DownloadAudio / GetStreamURL /
    // ParseFormatsArray / …). Until now this entry defended itself only via the
    // SanitizeForCommandLine *blacklist*, which preserves an interior backslash —
    // so a hostile id like "..\\..\\evil" would survive into the watch/playlist
    // URL. Not exploitable from the UI (paste goes through SanitizeYtId), but the
    // doctrine of this file is that every public boundary validates its own input.
    // Playlist ids use the same alphabet as video ids ([A-Za-z0-9_-]); they are
    // longer than the 11-char video id but still comfortably under the shared
    // YT_ID_MAX_LEN (64) ceiling, so IsValidVideoId's constraint fits playlists.
    if (!IsValidVideoId(playlistId)) return false;

    if (!IsYtdlpAvailable()) return false;

    std::wstring safePlaylistId = SanitizeForCommandLine(playlistId);
    std::wstring url = L"https://www.youtube.com/playlist?list=" + safePlaylistId;
    // --quiet/--no-warnings keep stdout pure JSON for the line parser below.
    std::wstring args = L"--flat-playlist --dump-json --quiet --no-warnings \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return false;

    // m5 (v2.02): shared line parser (same logic as SearchWithYtdlp).
    ParseYtdlpJsonLines(output, results);

    return !results.empty();
}

// YouTube video mode toggle
static bool g_ytVideoMode = false;
void SetYouTubeVideoMode(bool mode) { g_ytVideoMode = mode; }
bool GetYouTubeVideoMode() { return g_ytVideoMode; }

// Get raw YouTube URL for video playback (libmpv handles yt-dlp internally)
bool YouTubeGetVideoURL(const std::wstring& videoId, std::wstring& url) {
    if (!IsValidVideoId(videoId)) return false;  // C (v2.00): boundary whitelist
    std::wstring safeId = SanitizeForCommandLine(videoId);
    url = L"https://www.youtube.com/watch?v=" + safeId;
    return true;
}

// Get stream URL for a video
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl) {
    if (!IsValidVideoId(videoId)) return false;  // C (v2.00): boundary whitelist
    if (!IsYtdlpAvailable()) return false;

    // Get best audio format URL using the shared BASS-friendly selector
    // (kAudioFormatSelector — m4a/aac/mp3/140; see its definition). m3 (v2.01):
    // this site and the three download paths now use that single constant, so
    // the mp3 fallback (formerly only here) can't drift between them again.
    std::wstring safeVideoId = SanitizeForCommandLine(videoId);
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeVideoId;
    std::wstring args = L"-f \"" + std::wstring(kAudioFormatSelector) + L"\" --get-url \"" + url + L"\"";
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
    for (auto ext : kAudioExts) {
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
    std::wstring newFile;
    std::wstring newExt;
    for (auto ext : kRefreshExts) {
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
    std::wstring args = L"-f \"" + std::wstring(kAudioFormatSelector) + L"\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--embed-chapters --no-overwrites "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    // A (v2.00): BOUNDED timeout. This is a background PLAYBACK-support refresh
    // (re-fetch a cached file to embed chapters), not an explicit user download.
    // An unlimited cap here meant a video that became a never-ending LIVE stream
    // would keep this thread (and a yt-dlp child) alive forever. The bounded
    // playback timeout kills a live/pathological fetch; a normal audio refresh
    // finishes well within it.
    // m2 (v2.01): DELIBERATELY call RunYtdlp WITHOUT the exitCode/stderr out-
    // params and DO NOT write g_ytLastFailureKey. This is a silent background
    // refresh — its only job is to embed chapters into an already-playable
    // cached file. If it fails (live stream, transient network), the user still
    // has the working cached audio and nothing is spoken. Classifying the
    // failure into the shared global key here would let this background thread
    // clobber the failure reason a FOREGROUND worker is about to read, mis-
    // attributing a wrong cause to that worker's own failure. Keep it silent.
    RunYtdlp(args, YTDLP_PLAYBACK_TIMEOUT_MS);
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
    for (auto ext : kRefreshExts) {
        std::wstring c = cacheDir + L"\\yt-" + videoId + L".refresh" + ext;
        if (PathFileExistsW(c.c_str())) return;
    }
    std::wstring* idCopy = new std::wstring(videoId);
    HANDLE t = CreateThread(nullptr, 0, RefreshCacheThread, idCopy, 0, nullptr);
    if (t) CloseHandle(t);
}

// M5 (v2.04): shared epilogue for a FAILED download (non-zero yt-dlp exit).
// The three download paths — YouTubeDownloadAudio (cache fill), YouTube-
// DownloadPermanent (m4a to Downloads), YouTubeDownloadFormat (chosen format,
// maybe merged) — each ended with the same three steps: classify the failure
// into the shared spoken key, delete the partial file yt-dlp may have left
// behind (so it can't be mistaken for a good cache/download later), and return
// false. They differ ONLY in HOW they locate that partial (FindCachedAudio vs
// FindProducedFile over a different ext set), so each caller passes the partial
// path it already found (empty if none). Folding the common tail here keeps the
// classify+delete+false contract byte-identical across all three and removes the
// drift risk of three hand-maintained copies.
static bool HandleDownloadFailure(int exitCode,
                                  const std::wstring& stderrText,
                                  const std::wstring& partialPath) {
    g_ytLastFailureKey = ClassifyYtdlpFailure(exitCode, stderrText);
    if (!partialPath.empty()) DeleteFileW(partialPath.c_str());
    return false;
}

// Downloads YouTube audio into the persistent cache and returns the local path.
// If the audio is already cached we return immediately — no network round-trip,
// no yt-dlp invocation, no Speak("Downloading audio") needed by the caller.
bool YouTubeDownloadAudio(const std::wstring& videoId, std::wstring& outFilePath) {
    // C (v2.00): reject anything outside the id alphabet BEFORE any path/URL is
    // built from it (this runs on HybridDownloadThread too).
    if (!IsValidVideoId(videoId)) return false;
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
    std::wstring args = L"-f \"" + std::wstring(kAudioFormatSelector) + L"\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--embed-chapters "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    // A (v2.00): BOUNDED playback timeout (not the unlimited download timeout).
    // This function feeds the PLAYBACK path only — cache fill, the hybrid
    // background download (HybridDownloadThread), and the last-resort blocking
    // download inside YouTubePlayById. None of those should ever wait forever:
    // a YouTube LIVE stream never finishes, so an unlimited cap leaked the hybrid
    // thread + its yt-dlp child indefinitely and froze the last-resort path. The
    // 60 s ceiling is far longer than any normal audio fetch but guarantees a
    // live/pathological transfer is killed and reported as a clean failure.
    // EXPLICIT user downloads ("Download" / "Download with options") go through
    // YouTubeDownloadPermanent / YouTubeDownloadFormat, which KEEP the unlimited
    // timeout so a deliberately-large transfer is never cut off.
    // M1 (v1.99): capture the exit code so a non-zero result (incl. a timeout
    // sentinel) is treated as a genuine failure even if a partial/stray file was
    // left behind. SUCCESS contract unchanged (bool true + outFilePath).
    int exitCode = 0;
    std::wstring stderrText;
    RunYtdlp(args, YTDLP_PLAYBACK_TIMEOUT_MS, &exitCode, &stderrText);

    if (exitCode != 0) {
        // Failure: record a specific reason and remove any partial file so it
        // can't be mistaken for a good cache entry on the next play. M5 (v2.04):
        // shared epilogue; partial located via FindCachedAudio for this path.
        return HandleDownloadFailure(exitCode, stderrText, FindCachedAudio(safeId));
    }

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
    // M5 (v2.04): WHITELIST the id at this PUBLIC boundary (youtube.h:55) before
    // it becomes part of a cache FILE PATH inside FindCachedAudio. Every other
    // public entry point validates with IsValidVideoId; this one historically
    // relied on the SanitizeForCommandLine *blacklist*, which preserves an
    // interior backslash, so a hostile id ("..\\..\\x") could probe outside the
    // cache dir. Not exploitable today (the sole caller — YouTubePlayById —
    // validates first), but every boundary must defend itself. Real 11-char ids
    // pass the whitelist unchanged.
    if (!IsValidVideoId(videoId)) return false;
    return !FindCachedAudio(SanitizeForCommandLine(videoId)).empty();
}

// Returns true if `name` (case-insensitive, extension ignored) is a Windows
// reserved device name: CON, PRN, AUX, NUL, COM1-9, LPT1-9. A file literally
// named "NUL.m4a" can't be created — the OS routes it to the null device and
// the download silently vanishes. We detect these so the caller can prefix an
// underscore and dodge the trap.
static bool IsWindowsReservedName(const std::wstring& name) {
    // Compare only the part before the first dot (the "base"), upper-cased.
    std::wstring base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(),
                   [](wchar_t c) { return (wchar_t)towupper(c); });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") {
        return true;
    }
    // COM1-COM9 and LPT1-LPT9.
    if (base.size() == 4 &&
        (base.compare(0, 3, L"COM") == 0 || base.compare(0, 3, L"LPT") == 0) &&
        base[3] >= L'1' && base[3] <= L'9') {
        return true;
    }
    return false;
}

// Sanitize a video title for use as a Windows filename. Layered hardening:
//   1. Strip control chars and the NTFS-forbidden set \ / : * ? " < > |
//      (replaced with a space).
//   2. Trim leading/trailing whitespace, then cap the length.
//   3. Strip trailing dots/spaces — Windows silently DROPS them, so "foo."
//      and "foo " both resolve to "foo", which can collide or confuse.
//   4. If the base name is a reserved device name (NUL, CON, COM1...), prefix
//      "_" — otherwise the file would be routed to a device and disappear.
//   5. If the name starts with '-', prefix "_" so it can never be mistaken for
//      a command-line option by any tool that later receives the path.
//   6. If nothing survives, fall back to `fallback` (caller passes the video id
//      or "video") so we never produce an empty filename.
static std::wstring SanitizeForFilename(const std::wstring& title,
                                        const std::wstring& fallback = L"video") {
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
    // Trim leading/trailing whitespace.
    while (!out.empty() && iswspace(out.front())) out.erase(out.begin());
    while (!out.empty() && iswspace(out.back()))  out.pop_back();
    // Cap length (Windows MAX_PATH is 260; keep room for path + ext).
    if (out.size() > MAX_FILENAME_LEN) out.resize(MAX_FILENAME_LEN);
    // Strip trailing dots and spaces — Windows discards them, so a title like
    // "Episode 1." would otherwise create "Episode 1" with surprising
    // collisions. Do this AFTER the length cap (the cap can expose a new tail).
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) {
        out.pop_back();
    }
    // Reserved device name → prefix underscore so the file is creatable.
    if (IsWindowsReservedName(out)) out = L"_" + out;
    // Leading '-' → prefix underscore (avoid option-injection ambiguity).
    if (!out.empty() && out.front() == L'-') out = L"_" + out;
    // Empty after all cleaning → honest fallback supplied by the caller.
    if (out.empty()) out = fallback;
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

// Probe for a file yt-dlp may have produced at `outBase` + one of `exts`.
// yt-dlp picks the extension from the actual stream/container, so we can't
// know it up front — we test each candidate. Returns true and fills
// outFilePath on the first hit. Shared by the permanent + format downloaders.
template <size_t N>
static bool FindProducedFile(const std::wstring& outBase,
                             const wchar_t* const (&exts)[N],
                             std::wstring& outFilePath) {
    for (auto ext : exts) {
        std::wstring candidate = outBase + ext;
        if (PathFileExistsW(candidate.c_str())) {
            outFilePath = candidate;
            return true;
        }
    }
    return false;
}

// Permanently download a video to the user's Downloads folder.
// Filename uses the video's real title (sanitized) so users can browse the
// folder and recognize their tracks. Returns the absolute path on success.
//
// NOTE on the deliberate duplication with YouTubeDownloadFormat: the two share
// destination + naming + the file-probe helper, but their yt-dlp invocations
// differ fundamentally (this one is a fixed AAC/M4A audio selector with no
// merge/ffmpeg; the other takes a user-chosen format_id and may merge video).
// The simple m4a path here is the most-used, most-tested code in the whole
// subsystem; folding it into a parameterized super-function would risk a subtle
// regression for marginal gain, so the invocation stays isolated on purpose.
bool YouTubeDownloadPermanent(const std::wstring& videoId,
                              const std::wstring& title,
                              std::wstring& outFilePath) {
    if (!IsValidVideoId(videoId)) return false;  // C (v2.00): boundary whitelist
    if (!IsYtdlpAvailable()) return false;
    std::wstring safeId = SanitizeForCommandLine(videoId);

    std::wstring dir = GetDownloadsTargetDir();
    std::wstring base = SanitizeForFilename(title, safeId);
    std::wstring outBase = dir + L"\\" + base;

    // If the file already exists (user downloaded this before), don't
    // overwrite — yt-dlp's default behavior with -o is to skip if file
    // exists, but be explicit by adding --no-overwrites.
    std::wstring outArg = outBase + L".%(ext)s";
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring args = L"-f \"" + std::wstring(kAudioFormatSelector) + L"\" "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--no-overwrites --embed-chapters "
                        L"-o \"" + outArg + L"\" \"" + url + L"\"";
    // Unlimited timeout — see RunYtdlp/YTDLP_DOWNLOAD_TIMEOUT_MS rationale.
    // M1 (v1.99): a non-zero exit code is a HARD failure. Previously success
    // was inferred from FindProducedFile alone, so a stale/partial file made us
    // lie ("Downloaded") about a failed transfer. Now we classify the reason
    // and delete any partial so it isn't mistaken for a good file later.
    int exitCode = 0;
    std::wstring stderrText;
    RunYtdlp(args, YTDLP_DOWNLOAD_TIMEOUT_MS, &exitCode, &stderrText);

    if (exitCode != 0) {
        // M5 (v2.04): shared epilogue; partial located via FindProducedFile over
        // the audio ext set for this path.
        std::wstring partial;
        FindProducedFile(outBase, kAudioExts, partial);  // partial = "" if none
        return HandleDownloadFailure(exitCode, stderrText, partial);
    }

    return FindProducedFile(outBase, kAudioExts, outFilePath);
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

    // 3. ffmpeg.exe on the system PATH — LAST resort.
    //
    // SECURITY: SearchPathW(nullptr, ...) without safe-search includes the
    // *current working directory* in its search order, which is a classic
    // binary-planting vector (a malicious "ffmpeg.exe" dropped in whatever
    // folder the app was launched from would be picked up and run). We enable
    // BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE first, which moves the CWD to
    // AFTER the system directories so a planted binary can't shadow the real
    // one. The bundled copy in step 2 should always win in practice; this
    // branch only fires on a broken/partial install.
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE |
                      BASE_SEARCH_PATH_PERMANENT);
    wchar_t found[MAX_PATH] = {0};
    if (SearchPathW(nullptr, L"ffmpeg.exe", L".exe", MAX_PATH, found, nullptr) > 0) {
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

// A format is "video-only" when it carries no audio track (acodec == "none").
// Such a stream must have audio merged in at download time. yt-dlp reports an
// absent codec as the literal string "none" (occasionally empty), so we treat
// both as "no audio". Single source of truth for this test across the file.
static bool IsVideoOnly(const YtFormat& f) {
    return (f.acodec == L"none" || f.acodec.empty()) &&
           !(f.vcodec == L"none" || f.vcodec.empty());
}

// Mirror of IsVideoOnly for the audio-only case (no video track).
static bool IsAudioOnly(const YtFormat& f) {
    return (f.vcodec == L"none" || f.vcodec.empty()) &&
           !(f.acodec == L"none" || f.acodec.empty());
}

// Query yt-dlp for all formats of a video and parse them, best first.
std::vector<YtFormat> ParseFormatsArray(const std::wstring& videoId) {
    std::vector<YtFormat> formats;
    if (!IsValidVideoId(videoId)) return formats;  // C (v2.00): boundary whitelist
    if (!IsYtdlpAvailable()) return formats;

    std::wstring safeId = SanitizeForCommandLine(videoId);
    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    // "%(formats)j" prints the full formats array as one compact JSON line.
    // RunYtdlp accumulates stdout into a growing std::string (no fixed buffer
    // cap), so a 30+ format JSON is captured in full. --quiet/--no-warnings
    // keep stdout clean so the JSON parser isn't fed stray warning lines.
    std::wstring args = L"--quiet --no-warnings --no-playlist "
                        L"--print \"%(formats)j\" \"" + url + L"\"";
    int exitCode = 0;
    std::wstring stderrText;
    std::wstring output = RunYtdlp(args, YTDLP_QUERY_TIMEOUT_MS, &exitCode, &stderrText);
    // Surface a specific reason (private/unavailable/network) to the picker.
    g_ytLastFailureKey = nullptr;
    if (exitCode != 0 || output.empty()) {
        g_ytLastFailureKey = ClassifyYtdlpFailure(exitCode, stderrText);
    }
    // M5 (v2.04): a non-zero exit code means the query FAILED — even when yt-dlp
    // emitted a partial "[...]" JSON array on stdout first (e.g. a live stream
    // that prints some metadata, then errors). Returning the parsed-but-bogus
    // formats here would open the picker for a request that already errored, and
    // the format the user then picks would fail at download time ("pick then
    // fail"). Return an EMPTY vector instead: YouTubeOnFormatsReady sees
    // formats.empty(), reads the failureKey ParseFormatsArray just set, and
    // speaks the specific reason (live/private/network) — no misleading picker.
    if (exitCode != 0) return formats;  // formats is still empty here
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

        bool audioOnly = IsAudioOnly(f);
        bool videoOnly = IsVideoOnly(f);

        // Human resolution label.
        // G (v2.00): the "audio only" / "video only" labels were hard-coded
        // English under otherwise-translated column headers. Wrap them in T() so
        // they localize (FR: "audio seul" / "vidéo seule"). T() is read-only at
        // runtime (the maps are filled once at startup), so calling it here on
        // the format worker thread is safe.
        if (audioOnly) {
            f.resolution = T("audio only");
        } else if (f.height > 0) {
            wchar_t res[40];
            swprintf(res, 40, L"%dp", f.height);
            f.resolution = res;
            if (videoOnly) f.resolution += L" " + std::wstring(T("(video only)"));
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
    if (id.empty() || id.size() > YT_ID_MAX_LEN) return false;
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
    if (!IsValidVideoId(videoId)) return false;    // C (v2.00): boundary whitelist
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
    // M2 (v1.99): include the chosen format id in the output base name, e.g.
    // "My Video [137].mp4". Without a discriminant, re-downloading a DIFFERENT
    // resolution of an already-downloaded title hit --no-overwrites: yt-dlp
    // skipped (exit 0), FindProducedFile found the OLD file, and we lied
    // ("Downloaded") while the user still had the old quality. A per-format
    // base name gives each quality its own file, so no silent skip occurs.
    // formatId is already validated to [A-Za-z0-9_+.-] (IsValidFormatId), every
    // char of which is legal in a Windows filename, so it needs no further
    // sanitizing — but we run it through SanitizeForFilename anyway for the
    // title portion and append the bracketed id afterwards.
    std::wstring base = SanitizeForFilename(title, safeId) + L" [" + formatId + L"]";
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
    // Unlimited timeout — a video download + merge can take minutes.
    // M1 (v1.99): treat a non-zero exit code (e.g. a failed ffmpeg merge that
    // leaves only the muted video-only stream) as a HARD failure. Previously
    // success was inferred from FindProducedFile alone, so a merge that failed
    // but left a silent video file was announced as "Downloaded".
    int exitCode = 0;
    std::wstring stderrText;
    RunYtdlp(args, YTDLP_DOWNLOAD_TIMEOUT_MS, &exitCode, &stderrText);

    if (exitCode != 0) {
        // M5 (v2.04): shared epilogue; partial located via FindProducedFile over
        // the wider format ext set (audio/video/merged) for this path.
        std::wstring partial;
        FindProducedFile(outBase, kFormatExts, partial);  // partial = "" if none
        return HandleDownloadFailure(exitCode, stderrText, partial);
    }

    // yt-dlp may produce any of kFormatExts depending on the chosen stream and
    // whether a merge happened.
    return FindProducedFile(outBase, kFormatExts, outFilePath);
}

// Download SEVERAL streams of one video and mux them into ONE .mkv (v2.10).
// See the header for the contract. This is the multi-select counterpart of
// YouTubeDownloadFormat: the user ticked >=2 formats in the picker (typically
// one video + one or more alternate-language audio tracks), and we keep ALL of
// them in a single file via yt-dlp's --audio-multistreams/--video-multistreams.
bool YouTubeDownloadMultiFormat(const std::wstring& videoId,
                                const std::wstring& title,
                                const std::vector<std::wstring>& formatIds,
                                std::wstring& outFilePath) {
    if (!IsValidVideoId(videoId)) return false;
    if (!IsYtdlpAvailable()) return false;
    if (formatIds.size() < 2) return false;  // caller routes singles elsewhere

    // Validate EVERY id before building the selector, then join with '+'. Each id
    // is whitelisted to [A-Za-z0-9_+.-] (IsValidFormatId), and '+' is the literal
    // yt-dlp stream-combine operator — no shell metacharacter can slip in.
    std::wstring selector;
    std::wstring idTag;  // for a disambiguating, filename-legal suffix
    for (size_t i = 0; i < formatIds.size(); ++i) {
        if (!IsValidFormatId(formatIds[i])) return false;
        if (i) { selector += L'+'; idTag += L'+'; }
        selector += formatIds[i];
        idTag    += formatIds[i];
    }

    std::wstring safeId = SanitizeForCommandLine(videoId);
    std::wstring dir = GetDownloadsTargetDir();
    // Bracketed id list keeps each combination in its own file (no silent
    // --no-overwrites skip across different selections). '+' is legal in a
    // Windows filename; the title portion is sanitized as usual.
    std::wstring base = SanitizeForFilename(title, safeId) + L" [" + idTag + L"]";
    std::wstring outBase = dir + L"\\" + base;
    std::wstring outArg = outBase + L".%(ext)s";

    std::wstring url = L"https://www.youtube.com/watch?v=" + safeId;
    std::wstring ffmpeg = GetFfmpegLocation();
    // ffmpeg is MANDATORY for a multi-stream download: muxing several tracks into
    // one container is an ffmpeg job. Without it, yt-dlp cannot merge and would
    // silently write each stream to its OWN file (.f137.mp4, .f251.webm, …) while
    // STILL exiting 0 — and because we pass --no-warnings/--quiet, the user would
    // never see the warning. We would then "find" only one of those files and
    // falsely report success with the other audio/video tracks lost. The single-
    // stream path can tolerate a missing ffmpeg (a combined format needs no
    // merge); this path cannot. Abort up front so the caller reports a clean
    // failure instead of a half-downloaded lie.
    if (ffmpeg.empty()) return false;

    // --audio-multistreams / --video-multistreams: without them yt-dlp collapses
    // the selector to AT MOST one audio + one video, silently dropping the extra
    // language track the user explicitly asked for. Matroska (.mkv) is the merge
    // container: it holds any number of audio tracks and any codec (Opus included,
    // which mp4 cannot), so no selection the picker allows can fail to mux.
    std::wstring args = L"-f \"" + selector + L"\" "
                        L"--audio-multistreams --video-multistreams "
                        L"--no-playlist --no-progress --no-warnings --quiet "
                        L"--no-overwrites --embed-chapters "
                        L"--merge-output-format mkv ";
    // ffmpeg is guaranteed non-empty here (we returned false above otherwise).
    args += L"--ffmpeg-location \"" + ffmpeg + L"\" ";
    args += L"-o \"" + outArg + L"\" \"" + url + L"\"";

    int exitCode = 0;
    std::wstring stderrText;
    RunYtdlp(args, YTDLP_DOWNLOAD_TIMEOUT_MS, &exitCode, &stderrText);

    if (exitCode != 0) {
        std::wstring partial;
        FindProducedFile(outBase, kFormatExts, partial);
        return HandleDownloadFailure(exitCode, stderrText, partial);
    }
    return FindProducedFile(outBase, kFormatExts, outFilePath);
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

// Enforce a cache size cap: delete the OLDEST cached videos (by last-write
// time) until the total cache size is below the limit.
//
// m1 (v2.01): eviction is GROUPED BY VIDEO ID, not file-by-file. Every cache
// entry is named "yt-<id>.<...>" — the media file ("yt-<id>.m4a"), its version
// marker ("yt-<id>.v2"), and any pending refresh ("yt-<id>.refresh.<ext>") all
// share the same <id>. The previous version sorted individual files and deleted
// them one at a time, which could evict the audio while orphaning its .v2 marker
// (or vice-versa), leaving the cache inconsistent — and the old comment falsely
// claimed companions were removed together. We now bucket all files by <id>,
// order buckets by their OLDEST member's mtime, and evict a whole bucket at once
// so a video and all its sidecars always leave together.
// Pass 0 or negative to disable (no-op). Returns the number of FILES removed.
int EnforceYouTubeCacheLimit(int limitMB) {
    if (limitMB <= 0) return 0;
    unsigned long long limitBytes = (unsigned long long)limitMB * 1024ULL * 1024ULL;
    unsigned long long current = GetYouTubeCacheSize();
    if (current <= limitBytes) return 0;

    std::wstring dir = GetYouTubeCacheDir();
    std::wstring pattern = dir + L"\\yt-*.*";

    // One bucket per video id: all its files, the bucket's total size, and the
    // OLDEST mtime across its members (the bucket's eviction priority).
    struct CacheGroup {
        std::vector<std::wstring> names;  // file names within `dir`
        ULONGLONG oldestMtime = 0xFFFFFFFFFFFFFFFFULL;
        unsigned long long size = 0;
    };
    std::vector<std::pair<std::wstring, CacheGroup>> order;  // id → group, stable

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;

        // Derive the <id>: strip the "yt-" prefix, then take everything up to
        // the first '.'. So "yt-ABC123.m4a", "yt-ABC123.v2" and
        // "yt-ABC123.refresh.m4a" all map to id "ABC123".
        std::wstring id;
        if (name.compare(0, 3, L"yt-") == 0) {
            size_t dot = name.find(L'.', 3);
            id = name.substr(3, (dot == std::wstring::npos) ? std::wstring::npos
                                                            : dot - 3);
        } else {
            id = name;  // unexpected layout — treat as its own group
        }

        ULARGE_INTEGER sz; sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
        ULARGE_INTEGER mt; mt.LowPart = fd.ftLastWriteTime.dwLowDateTime; mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;

        // Find or create the bucket for this id.
        CacheGroup* g = nullptr;
        for (auto& kv : order) {
            if (kv.first == id) { g = &kv.second; break; }
        }
        if (!g) {
            order.emplace_back(id, CacheGroup{});
            g = &order.back().second;
        }
        g->names.push_back(name);
        g->size += sz.QuadPart;
        if (mt.QuadPart < g->oldestMtime) g->oldestMtime = mt.QuadPart;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    // Evict whole buckets, oldest first (by the bucket's oldest member).
    std::sort(order.begin(), order.end(),
              [](const std::pair<std::wstring, CacheGroup>& a,
                 const std::pair<std::wstring, CacheGroup>& b) {
                  return a.second.oldestMtime < b.second.oldestMtime;
              });

    int removed = 0;
    for (const auto& kv : order) {
        if (current <= limitBytes) break;
        const CacheGroup& g = kv.second;
        unsigned long long freed = 0;
        for (const auto& n : g.names) {
            std::wstring full = dir + L"\\" + n;
            if (DeleteFileW(full.c_str())) removed++;
            // (a marker is 0 bytes; size accounting below uses the bucket total)
        }
        freed = g.size;
        current = (current > freed) ? (current - freed) : 0;
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
    // A/B (v2.00): clear the shared failure key before the call so a stale key
    // can't be misattributed to this transfer (the key is atomic — see decl).
    g_ytLastFailureKey = nullptr;
    if (YouTubeDownloadAudio(videoId, localFile)) {
        // Heap-allocate the id again so it survives until the main thread
        // pops the message off the queue.
        std::wstring* postId = new std::wstring(videoId);
        PostMessageW(g_hwnd, WM_YT_HYBRID_READY, 0, reinterpret_cast<LPARAM>(postId));
    } else {
        // A (v2.00): the BASS download failed (e.g. this is a LIVE stream, which
        // never terminates and is now killed by the bounded playback timeout, or
        // the video is private/unavailable). The mpv stream may still be playing
        // in the background, but the user must know effects won't activate AND
        // why. Voice the specific classified reason. Speak() is thread-safe (it
        // queues + PostMessage(WM_SPEAK) to the main window). Capture the atomic
        // key locally first so we read it exactly once.
        const char* key = g_ytLastFailureKey.load();
        if (key) Speak(Ts(key));
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
    // C (v2.00): validate the id at this public entry point BEFORE building any
    // URL (video mode) or invoking yt-dlp. A hostile id can never reach a path
    // or command line. Speak a clean failure instead of silently doing nothing.
    if (!IsValidVideoId(videoId)) {
        Speak(Ts("Failed to play this video"));
        return false;
    }

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
    // A/B (v2.00): clear the shared failure key first so we can voice the SPECIFIC
    // reason this blocking fetch failed (live stream killed by the bounded
    // playback timeout, private, unavailable, network…) instead of a blanket
    // message. The bounded timeout inside YouTubeDownloadAudio guarantees this
    // call returns even on a never-ending live stream (no UI-thread freeze).
    g_ytLastFailureKey = nullptr;
    if (YouTubeDownloadAudio(videoId, localFile) && LoadFile(localFile.c_str())) {
        Speak(Ts("Playing"));
        return true;
    }

    const char* key = g_ytLastFailureKey.load();
    Speak(Ts(key ? key : "Failed to play this video"));
    return false;
}

// Check if input is a YouTube URL
bool IsYouTubeURL(const std::wstring& input) {
    return input.find(L"youtube.com") != std::wstring::npos ||
           input.find(L"youtu.be") != std::wstring::npos;
}

// Returns true if `c` is in the YouTube ID alphabet: [A-Za-z0-9_-].
// Video and playlist IDs are drawn exclusively from this set.
static bool IsYtIdChar(wchar_t c) {
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
}

// DEFENSE IN DEPTH (security): keep only the leading run of valid ID
// characters, dropping anything else. Even though SanitizeForCommandLine runs
// later on the consumer side, validating at the PARSING boundary means a
// malformed/hostile URL can never propagate a junk id deeper into the system.
// Video IDs are an exact 11 chars; if `exactEleven` is set we additionally
// require that length (a shorter/longer run is treated as no id).
static std::wstring SanitizeYtId(const std::wstring& raw, bool exactEleven) {
    std::wstring clean;
    for (wchar_t c : raw) {
        if (!IsYtIdChar(c)) break;  // stop at first out-of-alphabet char
        clean.push_back(c);
    }
    if (exactEleven && clean.size() != 11) return L"";
    return clean;
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
        std::wstring raw = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        // Playlist ids use the YouTube alphabet but have no fixed length.
        id = SanitizeYtId(raw, /*exactEleven=*/false);
        isPlaylist = true;
        return !id.empty();
    }

    // Check for channel. M2 (v2.03): detect ALL four channel-URL shapes —
    //   youtube.com/channel/UC...   (canonical channel id)
    //   youtube.com/@handle         (handles may contain '.', '-', '_')
    //   youtube.com/c/Name          (legacy custom URL)
    //   youtube.com/user/Name       (legacy username)
    // We only need to RELIABLY flag isChannel so DoSearch's M1 branch can speak
    // an honest "channel browsing not supported" message. Channel browsing is a
    // non-feature, so we do NOT produce a usable channel id: SanitizeYtId would
    // truncate a handle like "@news.channel" at the first dot anyway, and no path
    // or command line is ever built from a channel id. Order matters — this runs
    // AFTER the list= (playlist) check above, so a "/channel/...?list=" URL is
    // still treated as a playlist; and BEFORE the v=/youtu.be video checks, but a
    // real channel URL never carries a v= id, so detection can't steal a video.
    {
        size_t pos = std::wstring::npos;
        if ((pos = url.find(L"/channel/")) != std::wstring::npos) { pos += 9; }
        else if ((pos = url.find(L"/@"))    != std::wstring::npos) { pos += 2; }
        else if ((pos = url.find(L"/c/"))   != std::wstring::npos) { pos += 3; }
        else if ((pos = url.find(L"/user/"))!= std::wstring::npos) { pos += 6; }

        if (pos != std::wstring::npos) {
            isChannel = true;
            // Best-effort echo of the channel reference for the caller's logs/UI.
            // Handles/custom names allow '.', '-', '_' in addition to the id
            // alphabet, so we stop only at a true URL delimiter rather than at the
            // first non-id char. This value is NEVER used to build a path or a
            // command line (channel browsing is unsupported), so the wider char
            // set is safe here; the M1 branch simply discards it and speaks.
            size_t end = url.find_first_of(L"/?# ", pos);
            id = url.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            return true;  // detected a channel (id may legitimately be a handle)
        }
    }

    // Check for video ID
    size_t vPos = url.find(L"v=");
    if (vPos != std::wstring::npos) {
        size_t start = vPos + 2;
        size_t end = url.find_first_of(L"&# ", start);
        std::wstring raw = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        // YouTube video ids are exactly 11 chars from the ID alphabet.
        id = SanitizeYtId(raw, /*exactEleven=*/true);
        return !id.empty();
    }

    // youtu.be format
    size_t bePos = url.find(L"youtu.be/");
    if (bePos != std::wstring::npos) {
        size_t start = bePos + 9;
        size_t end = url.find_first_of(L"?# ", start);
        std::wstring raw = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        id = SanitizeYtId(raw, /*exactEleven=*/true);
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

    // M5 (v2.04): NEVER disable the visible "Load more" button. Greying a
    // visible control violates the project doctrine — NVDA announces it as
    // "unavailable" and Tab skips it, stranding a keyboard/screen-reader user on
    // a button they can't reach or understand. The button stays enabled at all
    // times; when there are no further results, activating it speaks a clear
    // "No more results" (see DoLoadMore) instead of being silently inert. We
    // therefore no longer call EnableWindow(hLoadMore, ...) here.
}

// ============================================================
// Background first-page search / playlist load (v1.99) — anti-freeze
// ============================================================
//
// M3/M4: the first-page search and the playlist-URL paste used to call
// YouTubeSearch / YouTubeGetPlaylistContents SYNCHRONOUSLY on the UI thread,
// freezing the window and muting the screen reader for up to YTDLP_QUERY_-
// TIMEOUT_MS (30 s). Both now run on a worker thread that posts
// WM_YT_SEARCH_DONE; the handler populates the list on the UI thread and
// announces the count or the classified failure reason.
//
// A single atomic flag (g_ytSearching) refuses a second concurrent search so
// a double-Enter / key-repeat can't spawn two competing workers writing the
// shared result globals. The "load more" manual button reuses the existing
// TriggerAutoLoadMore async path (forward-declared just below).

static std::atomic<bool> g_ytSearching{false};

// Forward decl — defined further down with the infinite-scroll machinery.
static void TriggerAutoLoadMore(HWND hwnd, int selection);

enum class YtSearchKind { Search, Playlist };

// Request handed to the search worker (heap, owned by the worker).
struct YtSearchRequest {
    YtSearchKind kind = YtSearchKind::Search;
    std::wstring query;        // Search kind: the user's query
    std::wstring playlistId;   // Playlist kind: the playlist id
};

// Result posted back to the UI (heap, freed by the handler). Carries the parsed
// results + the new page token + (on failure) the classified spoken key,
// captured INSIDE the worker right after the query (g_ytLastFailureKey is a
// process global; reading it on the UI thread later would race another query).
struct YtSearchResult {
    YtSearchKind kind = YtSearchKind::Search;
    std::wstring query;        // echoed back so a stale result can be discarded
    std::wstring playlistId;   // echoed back (Playlist kind)
    bool ok = false;
    std::vector<YouTubeResult> results;
    std::wstring nextPageToken;
    const char* failureKey = nullptr;   // static literal — safe to copy/keep
};

static DWORD WINAPI SearchThreadProc(LPVOID arg) {
    YtSearchRequest* req = static_cast<YtSearchRequest*>(arg);

    YtSearchResult* res = new YtSearchResult;
    res->kind       = req->kind;
    res->query      = req->query;
    res->playlistId = req->playlistId;

    g_ytLastFailureKey = nullptr;
    if (req->kind == YtSearchKind::Playlist) {
        res->ok = YouTubeGetPlaylistContents(req->playlistId, res->results,
                                             res->nextPageToken, L"");
    } else {
        // First page: no seen-ids snapshot needed (results are starting fresh).
        res->ok = YouTubeSearch(req->query, res->results,
                                res->nextPageToken, L"", nullptr);
    }
    if (!res->ok) res->failureKey = g_ytLastFailureKey;

    delete req;
    PostMessageW(g_hwnd, WM_YT_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(res));
    return 0;
}

// Handler for WM_YT_SEARCH_DONE (MAIN window proc). Populates the results list
// on the UI thread and announces. Robust if the dialog closed mid-fetch (frees
// the payload regardless). Discards a stale result if the user has since
// started a different search.
void YouTubeOnSearchDone(LPARAM lParam) {
    YtSearchResult* res = reinterpret_cast<YtSearchResult*>(lParam);
    g_ytSearching.store(false);
    if (!res) return;

    HWND dlg = GetYouTubeDialog();
    // Only apply if the dialog is still open AND this result matches the search
    // the user is currently viewing (a fresh search started mid-fetch changed
    // g_ytCurrentQuery / g_ytCurrentPlaylistId, so stale results are dropped).
    bool current =
        dlg &&
        ((res->kind == YtSearchKind::Playlist)
            ? (g_ytIsPlaylistView && res->playlistId == g_ytCurrentPlaylistId)
            : (!g_ytIsPlaylistView && res->query == g_ytCurrentQuery));

    if (current) {
        g_ytResults = res->results;
        g_ytNextPageToken = res->nextPageToken;
        UpdateResultsList(dlg);

        if (res->kind == YtSearchKind::Playlist) {
            if (res->ok) {
                Speak(Ts("Playlist loaded"));
            } else if (res->failureKey) {
                Speak(Ts(res->failureKey));
            } else {
                Speak(Ts("No results found"));
            }
        } else {
            if (res->ok) {
                // m3 (v2.03): substitute the count WITHOUT printf on a translated
                // string (a stray "%s" in a translation would otherwise be UB).
                Speak(WideToUtf8(FormatCount(T("%d results"),
                                             static_cast<int>(res->results.size()))));
            } else if (res->failureKey) {
                // yt-dlp errored — voice the SPECIFIC reason instead of a
                // misleading "no results".
                Speak(Ts(res->failureKey));
            } else {
                Speak(Ts("No results found"));
            }
        }
    }
    delete res;
}

// Spawn the background search/playlist worker. Snapshots all request state on
// the UI thread, announces "Searching…" immediately, returns at once.
static void StartSearchAsync(YtSearchRequest* req) {
    if (g_ytSearching.exchange(true)) {
        // One already running — ignore the second trigger (double-Enter).
        delete req;
        return;
    }
    Speak(Ts("Searching"));
    HANDLE t = CreateThread(nullptr, 0, SearchThreadProc, req, 0, nullptr);
    if (!t) {
        delete req;
        g_ytSearching.store(false);
    } else {
        CloseHandle(t);
    }
}

// Perform search — M3/M4: never blocks the UI thread on yt-dlp.
static void DoSearch(HWND hwnd) {
    // m2 (v2.02): size the read buffer to the ACTUAL text length. A fixed
    // wchar_t[512] truncated a very long pasted URL, corrupting the parsed video
    // /playlist id ("no results"). GetWindowTextLength gives the length (sans NUL);
    // read into a std::wstring of length+1 so even a multi-KB URL is captured whole.
    HWND hEdit = GetDlgItem(hwnd, IDC_YT_SEARCH);
    int len = GetWindowTextLengthW(hEdit);
    std::wstring query(static_cast<size_t>(len) + 1, L'\0');
    int got = GetWindowTextW(hEdit, &query[0], len + 1);
    query.resize(static_cast<size_t>(got));  // drop the trailing NUL slot

    if (query.empty()) return;

    // Check if it's a YouTube URL FIRST — a single-video paste plays directly
    // and must NOT clobber the current search state / results.
    if (IsYouTubeURL(query)) {
        std::wstring id;
        bool isPlaylist, isChannel;
        if (ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
            if (isPlaylist) {
                // Playlist paste — async (it runs yt-dlp). Reset view state on
                // the UI thread, then hand the fetch to the worker.
                g_ytCurrentQuery = query;
                g_ytResults.clear();
                g_ytNextPageToken.clear();
                g_ytIsPlaylistView = true;
                g_ytCurrentPlaylistId = id;
                UpdateResultsList(hwnd);   // clear the visible list immediately
                YtSearchRequest* req = new YtSearchRequest;
                req->kind = YtSearchKind::Playlist;
                req->playlistId = id;
                StartSearchAsync(req);
                return;
            } else if (isChannel) {
                // M1 (v2.03): a channel URL was pasted. Channel browsing is not a
                // feature. Speak an honest message instead of falling through to
                // the keyword-search fallback below, which would treat the raw URL
                // as a search query and surface misleading noise / "no results".
                Speak(Ts("Channel browsing is not supported yet. Paste a video or "
                         "playlist URL, or type keywords."));
                return;
            } else if (!isPlaylist && !isChannel) {
                // Single video — same path as double-clicking a result row
                // (PlaySelected): YouTubePlayById uses the hybrid (non-blocking)
                // stream-then-download model, so it does not freeze the UI.
                // v1.60 — channel/title unknown at this point (no result row).
                SetNowPlaying(SourceType::YouTube, L"YouTube", L"");
                YouTubePlayById(id);
                return;
            }
        }
    }

    // Regular search — reset view state on the UI thread, fetch on a worker.
    g_ytCurrentQuery = query;
    g_ytResults.clear();
    g_ytNextPageToken.clear();
    g_ytIsPlaylistView = false;
    UpdateResultsList(hwnd);   // clear the visible list immediately

    YtSearchRequest* req = new YtSearchRequest;
    req->kind = YtSearchKind::Search;
    req->query = query;
    StartSearchAsync(req);
}

// "Load more" button — M4: reuse the existing async infinite-scroll path so the
// manual button never blocks the UI either. The async worker dedups against the
// snapshot of already-seen ids and appends on completion.
static void DoLoadMore(HWND hwnd) {
    // M5 (v2.04): the button is ALWAYS enabled now (it is never greyed — see
    // UpdateResultsList). When there is no next page, activating it must give
    // honest spoken feedback rather than doing nothing silently.
    if (g_ytNextPageToken.empty()) {
        Speak(Ts("No more results"));
        return;
    }
    // -1 = no selection to restore (button press, not arrow navigation).
    TriggerAutoLoadMore(hwnd, -1);
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
    // M4 (v1.98): request state snapshotted on the UI thread BEFORE the worker
    // starts, so the worker never reads the live UI globals.
    bool isPlaylist = false;
    std::wstring playlistId;
    std::wstring query;
    std::wstring pageToken;
    std::vector<std::wstring> seenIds;   // ids already shown — for dedup snapshot
};

static DWORD WINAPI LoadMoreThreadProc(LPVOID arg) {
    YtLoadMoreData* d = static_cast<YtLoadMoreData*>(arg);

    // M4 (v1.98): all request state was snapshotted into `d` on the UI thread
    // by TriggerAutoLoadMore BEFORE this thread was created. The worker reads
    // ONLY from `d` — never from g_ytResults / g_ytIsPlaylistView / etc., which
    // the UI thread may be mutating concurrently (fresh search). If the user
    // changed search while we're mid-fetch, the message handler discards the
    // result via the query check below.
    if (d->isPlaylist) {
        YouTubeGetPlaylistContents(d->playlistId, d->results, d->newToken, d->pageToken);
    } else {
        YouTubeSearch(d->query, d->results, d->newToken, d->pageToken, &d->seenIds);
    }
    // Post to the always-alive main window. The main wndproc forwards to the YT
    // dialog only if it still exists, and frees `d` unconditionally — so a
    // dialog closed mid-fetch can never leak the heap struct (a PostMessage to
    // a destroyed HWND is silently dropped by Windows).
    PostMessageW(g_hwnd, WM_YT_LOAD_MORE_DONE, 0, reinterpret_cast<LPARAM>(d));
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
    // M4: snapshot ALL request state here, on the UI thread, while it is stable.
    d->isPlaylist = g_ytIsPlaylistView;
    d->playlistId = g_ytCurrentPlaylistId;
    d->query      = g_ytCurrentQuery;
    d->pageToken  = g_ytNextPageToken;
    d->seenIds.reserve(g_ytResults.size());
    for (const auto& r : g_ytResults) d->seenIds.push_back(r.videoId);

    HANDLE t = CreateThread(nullptr, 0, LoadMoreThreadProc, d, 0, nullptr);
    if (!t) {
        delete d;
        g_ytLoadingMore.store(false);
    } else {
        CloseHandle(t);
    }
}

// Handler for WM_YT_LOAD_MORE_DONE, called from the MAIN window proc (the
// worker now posts to g_hwnd so the heap struct is freed even if the dialog
// closed mid-fetch). Appends the new results, restores selection, announces.
void YouTubeOnLoadMoreDone(LPARAM lParam) {
    YtLoadMoreData* d = reinterpret_cast<YtLoadMoreData*>(lParam);
    if (!d) { g_ytLoadingMore.store(false); return; }

    HWND dlg = GetYouTubeDialog();
    // Only apply the update if (a) the dialog is still open AND (b) the query
    // that produced these results is still the one being viewed. A fresh search
    // started mid-fetch changes g_ytCurrentQuery, so stale results are dropped.
    if (dlg && d->hDlg == dlg &&
        d->isPlaylist == g_ytIsPlaylistView &&
        (d->isPlaylist ? (d->playlistId == g_ytCurrentPlaylistId)
                       : (d->query == g_ytCurrentQuery))) {
        g_ytNextPageToken = d->newToken;
        for (const auto& r : d->results) g_ytResults.push_back(r);
        UpdateResultsList(dlg);

        // LB_SETCURSEL does NOT fire LBN_SELCHANGE, so this won't re-trigger
        // the loader.
        HWND hList = GetDlgItem(dlg, IDC_YT_RESULTS);
        SendMessageW(hList, LB_SETCURSEL, d->previousSelection, 0);

        if (!d->results.empty()) {
            // m3 (v2.03): printf-free substitution into the translated template.
            Speak(WideToUtf8(FormatCount(T("%d more loaded"),
                                         static_cast<int>(d->results.size()))));
        }
    }
    delete d;
    g_ytLoadingMore.store(false);
}

// ============================================================
// Background downloads (v1.98) — anti-freeze
// ============================================================
//
// Both download paths (simple m4a and download-by-format) used to block the UI
// thread for the ENTIRE transfer (potentially minutes), freezing the window and
// silencing the screen reader. They now run on a worker thread that posts
// WM_YT_DOWNLOAD_DONE to the main window when finished.
//
// A single atomic flag serializes downloads: starting a second one while one is
// in flight is refused with a spoken notice. This keeps state trivially correct
// (no overlapping yt-dlp writes to the same Downloads folder, no interleaved
// announcements) and is what blind users expect — one explicit action at a time.

static std::atomic<bool> g_ytDownloading{false};

// Which download engine the worker should invoke.
enum class YtDlKind { Permanent, Format, MultiFormat };

// Request handed to the download worker. Heap-allocated, owned by the worker,
// which copies the success/failure into a YtDownloadResult before exiting.
struct YtDownloadRequest {
    YtDlKind kind = YtDlKind::Permanent;
    std::wstring videoId;
    std::wstring title;
    std::wstring formatId;     // Format kind only
    bool videoOnly = false;    // Format kind only
    std::vector<std::wstring> formatIds;  // MultiFormat kind only (>=2 ids)
};

// Result posted back to the UI. Heap-allocated by the worker, freed by the
// handler on the UI thread.
struct YtDownloadResult {
    bool success = false;
    std::wstring fileName;     // base name only (for "Downloaded: <name>")
    // M1 (v1.99): on failure, the SPECIFIC spoken-failure key the download
    // function classified (network/private/unavailable/timeout/…). Captured on
    // the worker thread right after the download call, so the UI announces the
    // REAL reason instead of a blanket "Download failed". Points at a static
    // literal owned by ClassifyYtdlpFailure — safe to copy/keep.
    const char* failureKey = nullptr;
};

static DWORD WINAPI DownloadThreadProc(LPVOID arg) {
    YtDownloadRequest* req = static_cast<YtDownloadRequest*>(arg);

    std::wstring saved;
    bool ok = false;
    // M1: clear the global failure key before the call so a stale key from an
    // earlier query can't be misattributed to this download.
    g_ytLastFailureKey = nullptr;
    if (req->kind == YtDlKind::MultiFormat) {
        ok = YouTubeDownloadMultiFormat(req->videoId, req->title,
                                        req->formatIds, saved);
    } else if (req->kind == YtDlKind::Format) {
        ok = YouTubeDownloadFormat(req->videoId, req->title, req->formatId,
                                   req->videoOnly, saved);
    } else {
        ok = YouTubeDownloadPermanent(req->videoId, req->title, saved);
    }

    YtDownloadResult* res = new YtDownloadResult;
    res->success = ok;
    if (ok) {
        const wchar_t* fname = wcsrchr(saved.c_str(), L'\\');
        res->fileName = fname ? (fname + 1) : saved;
    } else {
        // Capture the classified reason on THIS thread (downloads are
        // serialized by g_ytDownloading, so g_ytLastFailureKey still belongs to
        // this transfer).
        res->failureKey = g_ytLastFailureKey;
    }
    delete req;

    // Post to the always-alive main window — robust if the dialog has closed.
    PostMessageW(g_hwnd, WM_YT_DOWNLOAD_DONE, 0, reinterpret_cast<LPARAM>(res));
    return 0;
}

// ============================================================
// AnnounceStatus (v2.07) — focus-proof transient status announcement
// ============================================================
//
// Problem: after the modal format picker closes with EndDialog, focus returns
// to the results listbox. That focus transfer raises a SYNCHRONOUS focus event
// which NVDA treats as top priority — it cancels any speech we fire at the same
// instant. The "Download started" Speak() launched right after the picker
// closed was therefore swallowed.
//
// We tried UiaRaiseNotificationEvent (v2.06) as a focus-independent channel,
// but on a plain Win32 DialogBox the host provider is not a UIA element NVDA
// tracks, so the notification was silently dropped (and nothing was spoken).
//
// Fix (v2.07): defer the announcement. Post WM_APP_DEFERRED_SPEAK to the parent
// so it arms a ~250 ms one-shot timer; once the focus has settled (NVDA has
// read the dialog name + the focused results row), the timer speaks the message
// with interrupt=false so it appends after, instead of fighting the focus
// event. Reader-agnostic (NVDA/JAWS/Narrator); pattern from
// feedback_nvda_deferred_announce.md.

// Storage for the deferred-Speak timer path (see AnnounceStatus / the simple
// path), stashed in a file-static and picked up by the timer.
static std::wstring g_deferredSpeakText;

// v2.08 — Debounce for the download-completion announcement on the OPTIONS
// path. When the user confirms a format, the modal closes and focus returns to
// the results list (NVDA reads the focused item). If the download finishes
// almost immediately (cached / tiny file), "Downloaded: X" could land during
// that focus read. We record when the options-path download was kicked off; if
// completion arrives within DL_DEBOUNCE_MS, we defer the announcement past the
// focus read instead of racing it. A normal (multi-second) download finishes
// well after focus has settled, so it is announced immediately with no contention.
static DWORD g_ytDlKickoffTick = 0;
static bool  g_ytDlFromModal   = false;
static const DWORD DL_DEBOUNCE_MS = 300;

void AnnounceStatus(HWND hwndParent, const std::wstring& text) {
    // v2.07 — Deferred-Speak is the PRIMARY (and reliable) method.
    //
    // We originally (v2.06) tried UiaRaiseNotificationEvent as the primary
    // channel because it is, in theory, focus-independent. In practice, on a
    // plain Win32 DialogBox the host provider obtained from the HWND is not a
    // UIA element NVDA is actively tracking, so the notification is silently
    // dropped — and because the provider call SUCCEEDED, the fallback never
    // ran, leaving the message completely unspoken. This is the exact "silent
    // drop" risk the research flagged.
    //
    // The robust, reader-agnostic method (NVDA/JAWS/Narrator) is to DEFER the
    // announcement past the focus event that fires when the modal closes and
    // focus returns to the parent's list. We post a message to the parent's
    // window proc, which arms a short one-shot timer; by the time it fires,
    // the screen reader has already processed the focus change (it speaks the
    // dialog name + the focused result row), so our queued, non-interrupting
    // Speak appends cleanly after it instead of being cancelled. This is the
    // pattern prescribed by feedback_nvda_deferred_announce.md.
    if (hwndParent) {
        g_deferredSpeakText = text;
        PostMessageW(hwndParent, WM_APP_DEFERRED_SPEAK, 0, 0);
    } else {
        // No parent window to host the timer: speak now, non-interrupting.
        SpeakW(text, false);
    }
}

// Spawn a background download. Refuses a second concurrent download. Takes
// ownership of `req` (frees it on failure to launch). The matching
// WM_YT_DOWNLOAD_DONE handler announces the outcome.
//
// v2.06: the "Download started" announcement no longer lives here. The simple
// path (DownloadSelected) and the options path (YouTubeOnFormatsReady) announce
// it themselves — the simple path with an immediate Speak (no modal, never
// clipped), the options path via AnnounceStatus (focus-proof) because a modal
// just closed and would otherwise clip the speech.
static void StartDownloadAsync(YtDownloadRequest* req) {
    if (g_ytDownloading.exchange(true)) {
        // One already running — don't corrupt state with a parallel transfer.
        Speak(Ts("A download is already in progress"));
        delete req;
        return;
    }
    HANDLE t = CreateThread(nullptr, 0, DownloadThreadProc, req, 0, nullptr);
    if (!t) {
        delete req;
        g_ytDownloading.store(false);
        Speak(Ts("Download failed"));
    } else {
        CloseHandle(t);
    }
}

// Handler for WM_YT_DOWNLOAD_DONE (MAIN window proc). Announces the outcome and
// clears the in-flight flag. Robust if the dialog closed: the spoken feedback
// is global, so the user still hears it.
void YouTubeOnDownloadDone(LPARAM lParam) {
    YtDownloadResult* res = reinterpret_cast<YtDownloadResult*>(lParam);
    g_ytDownloading.store(false);
    if (!res) return;

    // Build the single completion message and decide its assertiveness:
    // success = polite (interrupt=false, queues behind any read in progress);
    // failure = assertive (interrupt=true) since an error matters more.
    std::wstring msg;
    bool assertive;
    if (res->success) {
        msg = std::wstring(T("Downloaded: ")) + res->fileName;
        assertive = false;
    } else if (res->failureKey) {
        // Voice the SPECIFIC reason (network down, video private, timeout,
        // geo-blocked, live stream, …) instead of a blanket "Download failed".
        msg = T(res->failureKey);
        assertive = true;
    } else {
        msg = T("Download failed");
        assertive = true;
    }
    delete res;

    // v2.08 — Debounce only for the options/modal path, and only when the file
    // finished almost instantly (cached/tiny) while the post-close focus read is
    // still in flight. In that case, defer the message ~250 ms so it lands AFTER
    // the focus read instead of racing it. Otherwise announce immediately —
    // a normal download finishes long after focus has settled, no contention.
    bool justFromModal = g_ytDlFromModal;
    DWORD elapsed = GetTickCount() - g_ytDlKickoffTick;
    g_ytDlFromModal = false;  // consume the flag

    HWND dlg = GetYouTubeDialog();
    if (justFromModal && elapsed < DL_DEBOUNCE_MS && dlg) {
        g_deferredSpeakText = msg;
        PostMessageW(dlg, WM_APP_DEFERRED_SPEAK, 0, 0);  // timer → Speak(interrupt=false)
        return;
    }

    SpeakW(msg.c_str(), assertive);
}

// ============================================================
// Background format query (v1.98) — anti-freeze
// ============================================================
//
// ShowFormatPicker used to call ParseFormatsArray (a 1–5 s, up to 30 s yt-dlp
// query) directly on the UI thread, freezing the YouTube window and muting NVDA
// while it ran. The query now runs on a worker thread that posts
// WM_YT_FORMATS_READY; the handler opens the modal picker on the UI thread.

static std::atomic<bool> g_ytFormatsLoading{false};

// Request for the format worker (heap, owned by the worker).
struct YtFormatsRequest {
    std::wstring videoId;
    std::wstring title;
};

// Result posted back (heap, owned by the UI handler). Carries the parsed
// formats plus, on failure, the specific spoken-failure key captured INSIDE the
// worker right after ParseFormatsArray (g_ytLastFailureKey is a process global;
// reading it on the UI thread later would race with another query).
struct YtFormatsResult {
    std::wstring videoId;
    std::wstring title;
    std::vector<YtFormat> formats;
    const char* failureKey = nullptr;   // points to a static literal — safe to copy
};

static DWORD WINAPI FormatsThreadProc(LPVOID arg) {
    YtFormatsRequest* req = static_cast<YtFormatsRequest*>(arg);

    YtFormatsResult* res = new YtFormatsResult;
    res->videoId = req->videoId;
    res->title   = req->title;

    g_ytLastFailureKey = nullptr;
    res->formats = ParseFormatsArray(req->videoId);
    // Capture the failure reason (if any) immediately, on this thread, while it
    // still belongs to this query. ParseFormatsArray set it for us.
    if (res->formats.empty()) res->failureKey = g_ytLastFailureKey;

    delete req;
    PostMessageW(g_hwnd, WM_YT_FORMATS_READY, 0, reinterpret_cast<LPARAM>(res));
    return 0;
}

// Forward decls — the modal picker + download launcher are defined further down,
// but YouTubeOnFormatsReady (also below) needs them.
static bool ShowFormatPickerFromList(HWND parent,
                                     const std::vector<YtFormat>& formats,
                                     std::wstring& chosenFormatId,
                                     bool& chosenVideoOnly,
                                     std::vector<std::wstring>& chosenFormatIds,
                                     bool& chosenMulti);

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
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) {
        // m5 (v1.99): give blind users explicit feedback instead of silence.
        Speak(Ts("No item selected"));
        return;
    }

    const YouTubeResult& result = g_ytResults[sel];
    if (result.videoId.empty()) {
        Speak(Ts("Cannot download this item"));
        return;
    }

    // v2.06 — simple path has no modal, so an immediate Speak is never clipped
    // by a focus event; keep the instant feedback.
    Speak(Ts("Download started"));

    // v1.98 — run on a worker thread so the UI never freezes during transfer.
    YtDownloadRequest* req = new YtDownloadRequest;
    req->kind    = YtDlKind::Permanent;
    req->videoId = result.videoId;
    req->title   = result.title;
    StartDownloadAsync(req);
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
// v2.10 — multi-select (redlaf): when the user TICKS two or more rows, these
// hold every chosen format id and g_ytfChosenMulti is set. The single-row path
// (g_ytfChosenFormatId / g_ytfChosenVideoOnly) is used when exactly one stream
// is selected so its smart video-only +bestaudio merge is preserved.
static std::vector<std::wstring> g_ytfChosenFormatIds;
static bool g_ytfChosenMulti = false;

static INT_PTR CALLBACK YtFormatsDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static const std::vector<YtFormat>* s_formats = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            s_formats = reinterpret_cast<const std::vector<YtFormat>*>(lParam);

            HWND hList = GetDlgItem(hwnd, IDC_YTF_LIST);
            // v2.10 — LVS_EX_CHECKBOXES lets the user TICK several streams
            // (e.g. one video + two language audio tracks) with the Space bar;
            // NVDA/JAWS announce the "checked/unchecked" state automatically.
            // Enter still confirms. If NOTHING is ticked, the focused row is
            // downloaded — the original single-pick behaviour is preserved.
            ListView_SetExtendedListViewStyle(
                hList, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_CHECKBOXES);

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
            // m4 (v2.02): an absent codec was shown as the em dash "—". At a
            // non-verbose NVDA punctuation level the dash is silent, so the cell
            // read as EMPTY (ambiguous: missing data vs. no audio/video track).
            // Use a localized word ("none" / "aucun") so the screen reader always
            // speaks something meaningful. Held in a std::wstring (T() returns a
            // wide string) so the c_str() outlives every ListView_SetItemText call.
            const std::wstring noneLabel = T("none");
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
                        const_cast<wchar_t*>(f.vcodec == L"none" ? noneLabel.c_str() : f.vcodec.c_str()));
                    ListView_SetItemText(hList, idx, 3,
                        const_cast<wchar_t*>(f.acodec == L"none" ? noneLabel.c_str() : f.acodec.c_str()));
                    ListView_SetItemText(hList, idx, 4,
                        const_cast<wchar_t*>(f.sizeStr.c_str()));
                    ListView_SetItemText(hList, idx, 5,
                        const_cast<wchar_t*>(f.note.c_str()));
                    // v2.10 fix — explicitly force the checkbox to "unchecked".
                    // Inserting with mask=LVIF_TEXT only does NOT set a state
                    // image, so an un-realized (never-scrolled-into-view) row can
                    // keep state image 0. ListView_GetCheckState then returns
                    // (UINT)-1 for it (truthy) and the row reads as "checked" even
                    // though the user never ticked it — which falsely inflated the
                    // video-track count and blocked legitimate selections. Setting
                    // it here guarantees every row starts unchecked AND announces a
                    // consistent "unchecked" state to the screen reader on arrival.
                    ListView_SetCheckState(hList, idx, FALSE);
                    ++row;
                }
            }

            // Encode the context (count) into the DIALOG TITLE rather than a
            // separate Speak (v2.05). A screen reader reads a modal dialog's
            // title FIRST on open, then the focused control, then its first row —
            // one ordered sequence with no race. The previous non-interrupting
            // Speak still competed with the list's synchronous UIA focus event
            // and was clipped, so the user landed straight on row 0 without ever
            // hearing the context. FormatCount() does printf-free substitution
            // (a stray specifier in a translation can never crash here).
            const int nCount =
                s_formats ? static_cast<int>(s_formats->size()) : 0;
            std::wstring title =
                FormatCount(T("Choose a format to download — %d formats"), nCount);
            SetWindowTextW(hwnd, title.c_str());
            // Safety net: if NVDA already cached the old caption, force a reread.
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_WINDOW,
                           CHILDID_SELF);

            // Preselect the first row (best quality) and focus the list.
            if (s_formats && !s_formats->empty()) {
                ListView_SetItemState(hList, 0,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            SetFocus(hList);

            // No Speak here on purpose (v2.05): the count now lives in the title,
            // which is read before the focused list — no clipped announcement.
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
                    g_ytfChosenFormatIds.clear();
                    g_ytfChosenMulti = false;
                    g_ytfChosenFormatId.clear();
                    g_ytfChosenVideoOnly = false;

                    // v2.10 — gather every TICKED row, in display order. With no
                    // tick, fall back to the focused/selected row (original
                    // single-pick behaviour).
                    std::vector<int> ticked;
                    if (s_formats) {
                        int n = static_cast<int>(s_formats->size());
                        for (int i = 0; i < n; ++i) {
                            // Test the state image EXPLICITLY (checked == image 2,
                            // 0x2000). ListView_GetCheckState returns
                            // (stateImage>>12)-1, so an un-initialized row (image 0)
                            // yields (UINT)-1 which is truthy — a plain
                            // `if (GetCheckState(...))` would wrongly count it.
                            UINT st = ListView_GetItemState(hList, i,
                                                            LVIS_STATEIMAGEMASK);
                            if ((st & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2))
                                ticked.push_back(i);
                        }
                    }
                    if (ticked.empty()) {
                        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                        if (sel < 0) sel = 0;
                        if (s_formats && sel < static_cast<int>(s_formats->size())) {
                            ticked.push_back(sel);
                        }
                    }

                    if (!s_formats || ticked.empty()) {
                        EndDialog(hwnd, IDCANCEL);
                        return TRUE;
                    }

                    if (ticked.size() == 1) {
                        // Single stream: keep the smart video-only +bestaudio
                        // merge of the existing YouTubeDownloadFormat path.
                        const YtFormat& f = (*s_formats)[ticked[0]];
                        g_ytfChosenFormatId  = f.formatId;
                        g_ytfChosenVideoOnly = IsVideoOnly(f);
                    } else {
                        // Two or more streams: mux them all into one .mkv.
                        // Guard: at most ONE video-bearing track. A format that
                        // is not audio-only carries video (combined or video-only;
                        // entries with neither codec were filtered at parse time).
                        // Ticking two video tracks would tell yt-dlp, via
                        // --video-multistreams, to keep BOTH — producing a bloated
                        // file with a redundant second (or mismatched-resolution)
                        // video track that no user intends. The supported shape is
                        // "(zero or one) video + one or more audio" (e.g. one video
                        // + two language audio tracks). Reject the nonsensical case
                        // with a spoken reason and KEEP the dialog open so the user
                        // can untick the extra video.
                        int videoCount = 0;
                        for (int idx : ticked) {
                            if (!IsAudioOnly((*s_formats)[idx])) ++videoCount;
                        }
                        if (videoCount > 1) {
                            Speak(Ts("Select at most one video track. You can add several audio tracks."));
                            return TRUE;  // do not close — let the user fix it
                        }
                        for (int idx : ticked) {
                            g_ytfChosenFormatIds.push_back((*s_formats)[idx].formatId);
                        }
                        g_ytfChosenMulti = true;
                    }
                    EndDialog(hwnd, IDOK);
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

// Show the modal format picker for an ALREADY-PARSED list. Returns true and
// fills chosenFormatId when the user confirms; false on cancel. The blocking
// yt-dlp query that produced `formats` now happens on a worker thread (see
// FormatsThreadProc), so this function never blocks on the network.
static bool ShowFormatPickerFromList(HWND parent,
                                     const std::vector<YtFormat>& formats,
                                     std::wstring& chosenFormatId,
                                     bool& chosenVideoOnly,
                                     std::vector<std::wstring>& chosenFormatIds,
                                     bool& chosenMulti) {
    if (formats.empty()) return false;
    g_ytfChosenFormatId.clear();
    g_ytfChosenVideoOnly = false;
    g_ytfChosenFormatIds.clear();
    g_ytfChosenMulti = false;
    INT_PTR r = DialogBoxParamW(GetModuleHandle(nullptr),
                                MAKEINTRESOURCEW(IDD_YT_FORMATS),
                                parent, YtFormatsDialogProc,
                                reinterpret_cast<LPARAM>(&formats));
    if (r != IDOK) return false;
    if (g_ytfChosenMulti && g_ytfChosenFormatIds.size() >= 2) {
        chosenFormatIds = g_ytfChosenFormatIds;
        chosenMulti = true;
        return true;
    }
    if (!g_ytfChosenFormatId.empty()) {
        chosenFormatId = g_ytfChosenFormatId;
        chosenVideoOnly = g_ytfChosenVideoOnly;
        chosenMulti = false;
        return true;
    }
    return false;
}

// Handler for WM_YT_FORMATS_READY (MAIN window proc). The background query
// finished; open the modal picker on the UI thread. If the dialog was closed
// mid-fetch, drop the UI but still free the payload. On confirm, kick off a
// background download.
void YouTubeOnFormatsReady(LPARAM lParam) {
    YtFormatsResult* res = reinterpret_cast<YtFormatsResult*>(lParam);
    g_ytFormatsLoading.store(false);
    if (!res) return;

    HWND dlg = GetYouTubeDialog();
    if (!dlg) {                       // user closed the window while we fetched
        delete res;
        return;
    }

    if (res->formats.empty()) {
        // Voice the SPECIFIC reason yt-dlp gave (private/unavailable/network),
        // captured on the worker thread; else the honest generic fallback.
        Speak(Ts(res->failureKey ? res->failureKey : "No formats available"));
        delete res;
        return;
    }

    std::wstring formatId;
    bool videoOnly = false;
    std::vector<std::wstring> formatIds;
    bool multi = false;
    bool picked = ShowFormatPickerFromList(dlg, res->formats, formatId, videoOnly,
                                           formatIds, multi);
    if (picked) {
        YtDownloadRequest* req = new YtDownloadRequest;
        req->videoId   = res->videoId;
        req->title     = res->title;
        if (multi) {
            req->kind       = YtDlKind::MultiFormat;
            req->formatIds  = formatIds;
        } else {
            req->kind       = YtDlKind::Format;
            req->formatId   = formatId;
            req->videoOnly  = videoOnly;
        }
        StartDownloadAsync(req);
        // v2.08 — Do NOT announce "Download started" here. The modal picker has
        // just closed and focus is returning to the results list; dismissing the
        // dialog on the user's Enter IS the acceptance receipt (per WCAG 4.1.3
        // "announce the outcome, not the process" and the screen-reader community
        // consensus that stacking a status on the focus-return read is redundant
        // clutter). The ONLY announcement for this path is the single completion
        // message ("Downloaded: X" / failure) from YouTubeOnDownloadDone, which
        // arrives later in its own quiet window. We mark this download as coming
        // from the modal so the completion handler can debounce it if the file
        // finishes near-instantly (cached) while focus is still settling.
        g_ytDlKickoffTick = GetTickCount();
        g_ytDlFromModal   = true;
    }
    delete res;
}

// "Download with options..." — query formats in the BACKGROUND, then (via
// WM_YT_FORMATS_READY) show the picker and download. Never blocks the UI.
static void DownloadSelectedWithOptions(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_YT_RESULTS);
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(g_ytResults.size())) {
        // m5 (v1.99): explicit feedback instead of silence.
        Speak(Ts("No item selected"));
        return;
    }

    const YouTubeResult& result = g_ytResults[sel];
    if (result.videoId.empty()) {
        Speak(Ts("Cannot download this item"));
        return;
    }

    // Guard against double-trigger (key repeat / fast Enter) like g_ytLoadingMore.
    if (g_ytFormatsLoading.exchange(true)) {
        Speak(Ts("Fetching formats"));
        return;
    }

    // Tell the blind user work has started before the (now background) query.
    Speak(Ts("Fetching formats"));

    YtFormatsRequest* req = new YtFormatsRequest;
    req->videoId = result.videoId;
    req->title   = result.title;
    HANDLE t = CreateThread(nullptr, 0, FormatsThreadProc, req, 0, nullptr);
    if (!t) {
        delete req;
        g_ytFormatsLoading.store(false);
        Speak(Ts("No formats available"));
    } else {
        CloseHandle(t);
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

        // v2.07 — AnnounceStatus deferred path. Arm a one-shot timer so the
        // deferred Speak fires AFTER the focus events that fire when the modal
        // closes (NVDA reads the parent dialog name + the focused results row)
        // have settled, instead of being clipped by them. 250 ms covers both
        // utterances; the Speak is non-interrupting so it appends cleanly.
        case WM_APP_DEFERRED_SPEAK:
            SetTimer(hwnd, IDT_DEFERRED_SPEAK, 250, nullptr);
            return TRUE;

        case WM_TIMER:
            if (wParam == IDT_DEFERRED_SPEAK) {
                KillTimer(hwnd, IDT_DEFERRED_SPEAK);
                if (!g_deferredSpeakText.empty()) {
                    // interrupt=false: let the now-settled focus announcement
                    // finish rather than fighting it.
                    SpeakW(g_deferredSpeakText, false);
                }
                return TRUE;
            }
            break;

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
