/*
 * video_engine.cpp -- libmpv integration for MediaAccess
 *
 * All mpv functions are loaded at runtime via LoadLibrary / GetProcAddress
 * so the build does not link against mpv at compile time.  If mpv-1.dll (or
 * mpv-2.dll) is not present the video engine simply reports "unavailable" and
 * all entry points become safe no-ops.
 */

#include "mediaaccess/video_engine.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/utils.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/logger.h"
#include "mpv/client.h"

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>

/* ================================================================
 *  Runtime linking
 * ================================================================ */
static HMODULE g_mpvDll = nullptr;

static pfn_mpv_create              fn_mpv_create              = nullptr;
static pfn_mpv_initialize          fn_mpv_initialize          = nullptr;
static pfn_mpv_destroy             fn_mpv_destroy             = nullptr;
static pfn_mpv_terminate_destroy   fn_mpv_terminate_destroy   = nullptr;
static pfn_mpv_set_option          fn_mpv_set_option          = nullptr;
static pfn_mpv_set_option_string   fn_mpv_set_option_string   = nullptr;
static pfn_mpv_command             fn_mpv_command             = nullptr;
static pfn_mpv_command_async       fn_mpv_command_async       = nullptr;
static pfn_mpv_command_string      fn_mpv_command_string      = nullptr;
static pfn_mpv_set_property        fn_mpv_set_property        = nullptr;
static pfn_mpv_set_property_string fn_mpv_set_property_string = nullptr;
static pfn_mpv_get_property        fn_mpv_get_property        = nullptr;
static pfn_mpv_get_property_string fn_mpv_get_property_string = nullptr;
static pfn_mpv_observe_property    fn_mpv_observe_property    = nullptr;
static pfn_mpv_wait_event          fn_mpv_wait_event          = nullptr;
static pfn_mpv_free                fn_mpv_free                = nullptr;
static pfn_mpv_free_node_contents  fn_mpv_free_node_contents  = nullptr;
static pfn_mpv_error_string        fn_mpv_error_string        = nullptr;

// Diagnostic: populated by LoadMPVLibrary on failure. Surfaced verbatim
// in the user-visible "please reinstall" dialog so we can diagnose without
// asking the user for log files.
std::wstring g_lastMpvLoadError;

static std::wstring FormatWin32Error(DWORD err)
{
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr);
    std::wstring out;
    if (len && buf) {
        out.assign(buf, len);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' ||
                                out.back() == L' '  || out.back() == L'.'))
            out.pop_back();
    }
    if (buf) LocalFree(buf);
    wchar_t numbuf[32];
    swprintf(numbuf, 32, L" (0x%08lX)", err);
    out += numbuf;
    return out;
}

#define LOAD_FN(handle, name)                                                  \
    do {                                                                       \
        fn_##name = (pfn_##name)GetProcAddress(handle, #name);                 \
        if (!fn_##name) {                                                      \
            DWORD err = GetLastError();                                        \
            g_lastMpvLoadError = L"GetProcAddress(\"" L#name L"\") failed: " + \
                                 FormatWin32Error(err);                        \
            LogF("MPV", "GetProcAddress(\"%s\") failed: 0x%08lX", #name, err); \
            FreeLibrary(g_mpvDll);                                             \
            g_mpvDll = nullptr;                                                \
            return false;                                                      \
        }                                                                      \
    } while (0)

static bool LoadMPVLibrary()
{
    if (g_mpvDll) return true;

    g_lastMpvLoadError.clear();
    std::wstring lastAttemptError;

    // libmpv-2.dll is shipped in {exeDir}\lib\. We MUST load it by absolute
    // path: the bare-name LoadLibraryW("libmpv-2.dll") path used to fail
    // because SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS) makes
    // Windows ignore the directory set via SetDllDirectoryW — only
    // AddDllDirectory-registered ones count under that flag. Loading by
    // full path sidesteps the search order entirely.
    //
    // Buffers are sized generously (32k) instead of MAX_PATH (260) because
    // install paths can exceed 260 on Win10+ with long-path support, and
    // silent swprintf truncation would otherwise produce an invalid path
    // that LoadLibraryExW rejects with a misleading error.
    auto tryLoadFromLibDir = [&](const wchar_t* name) -> HMODULE {
        wchar_t exePath[32768];
        DWORD got = GetModuleFileNameW(nullptr, exePath, 32768);
        if (got == 0 || got >= 32768) return nullptr;
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (!slash) return nullptr;
        *(slash + 1) = L'\0';
        wchar_t full[32768];
        int n = swprintf(full, 32768, L"%slib\\%s", exePath, name);
        if (n < 0 || n >= 32768) return nullptr;

        LogF("MPV", "Trying LoadLibraryExW: %ls", full);
        // Also check existence so we can report "missing" vs "won't load"
        DWORD attrs = GetFileAttributesW(full);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            LogF("MPV", "  -> file not present at this path");
            lastAttemptError = std::wstring(L"File not found: ") + full;
            return nullptr;
        }
        HMODULE h = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!h) {
            DWORD err = GetLastError();
            lastAttemptError = L"LoadLibraryExW(\"" + std::wstring(full) +
                               L"\") failed: " + FormatWin32Error(err);
            LogF("MPV", "  -> failed: 0x%08lX", err);
        } else {
            LogF("MPV", "  -> loaded OK");
        }
        return h;
    };

    auto tryLoadBare = [&](const wchar_t* name) -> HMODULE {
        LogF("MPV", "Trying LoadLibraryW (bare): %ls", name);
        HMODULE h = LoadLibraryW(name);
        if (!h) {
            DWORD err = GetLastError();
            lastAttemptError = L"LoadLibraryW(\"" + std::wstring(name) +
                               L"\") failed: " + FormatWin32Error(err);
            LogF("MPV", "  -> failed: 0x%08lX", err);
        } else {
            LogF("MPV", "  -> loaded OK");
        }
        return h;
    };

    // Bundled location first (current and legacy DLL names)
    g_mpvDll = tryLoadFromLibDir(L"libmpv-2.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadFromLibDir(L"mpv-2.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadFromLibDir(L"mpv-1.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadFromLibDir(L"libmpv.dll");

    // Fallback: let Windows search (in case a user dropped the DLL elsewhere)
    if (!g_mpvDll) g_mpvDll = tryLoadBare(L"libmpv-2.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadBare(L"mpv-2.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadBare(L"mpv-1.dll");
    if (!g_mpvDll) g_mpvDll = tryLoadBare(L"libmpv.dll");

    if (!g_mpvDll) {
        g_lastMpvLoadError = lastAttemptError.empty()
            ? std::wstring(L"libmpv-2.dll not found")
            : lastAttemptError;
        Log("MPV", std::wstring(L"All load attempts failed. Last error: ") +
                   g_lastMpvLoadError);
        return false;
    }

    LogF("MPV", "DLL loaded, resolving exports...");

    LOAD_FN(g_mpvDll, mpv_create);
    LOAD_FN(g_mpvDll, mpv_initialize);
    LOAD_FN(g_mpvDll, mpv_destroy);
    LOAD_FN(g_mpvDll, mpv_terminate_destroy);
    LOAD_FN(g_mpvDll, mpv_set_option);
    LOAD_FN(g_mpvDll, mpv_set_option_string);
    LOAD_FN(g_mpvDll, mpv_command);
    LOAD_FN(g_mpvDll, mpv_command_async);
    LOAD_FN(g_mpvDll, mpv_command_string);
    LOAD_FN(g_mpvDll, mpv_set_property);
    LOAD_FN(g_mpvDll, mpv_set_property_string);
    LOAD_FN(g_mpvDll, mpv_get_property);
    LOAD_FN(g_mpvDll, mpv_get_property_string);
    LOAD_FN(g_mpvDll, mpv_observe_property);
    LOAD_FN(g_mpvDll, mpv_wait_event);
    LOAD_FN(g_mpvDll, mpv_free);
    LOAD_FN(g_mpvDll, mpv_free_node_contents);
    LOAD_FN(g_mpvDll, mpv_error_string);

    return true;
}

#undef LOAD_FN

/* ================================================================
 *  Module state
 * ================================================================ */
static mpv_handle* g_mpv         = nullptr;
static HANDLE      g_mpvThread   = nullptr;

static std::atomic<bool>   g_mpvThreadRunning{false};
static std::atomic<double> g_mpvPosition{0.0};
static std::atomic<double> g_mpvDuration{0.0};
static std::atomic<bool>   g_mpvPaused{true};
static std::atomic<bool>   g_mpvIdle{true};
static std::atomic<bool>   g_mpvEofReached{false};

/* Fullscreen */
static RECT  g_savedWindowRect = {};
static LONG  g_savedWindowStyle = 0;
static bool  g_fullscreen = false;

/* Aspect-ratio cycle */
static int          g_aspectIndex = 0;
static const char*  g_aspectRatios[] = {"-1", "4:3", "16:9", "16:10", "2.35:1"};
static const wchar_t* g_aspectNames[] = {L"Auto", L"4:3", L"16:9", L"16:10", L"2.35:1"};
static const int    g_aspectCount = 5;

/* ================================================================
 *  Helpers -- track enumeration by type
 * ================================================================ */

/* Count tracks whose "type" matches |wantType| ("sub", "audio", "video"). */
static int CountTracksByType(const char* wantType)
{
    if (!g_mpv) return 0;
    __int64 count = 0;
    fn_mpv_get_property(g_mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    int matched = 0;
    for (__int64 i = 0; i < count; i++) {
        char key[80];
        _snprintf_s(key, sizeof(key), _TRUNCATE, "track-list/%lld/type", (long long)i);
        char* type = fn_mpv_get_property_string(g_mpv, key);
        if (type) {
            if (strcmp(type, wantType) == 0) matched++;
            fn_mpv_free(type);
        }
    }
    return matched;
}

/* Return the display name of the Nth track of |wantType|.  Falls back to
   lang, then "Track N". */
static std::wstring GetTrackNameByType(const char* wantType, int index)
{
    if (!g_mpv) return L"";
    __int64 count = 0;
    fn_mpv_get_property(g_mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    int idx = 0;
    for (__int64 i = 0; i < count; i++) {
        char typeKey[80];
        _snprintf_s(typeKey, sizeof(typeKey), _TRUNCATE, "track-list/%lld/type", (long long)i);
        char* type = fn_mpv_get_property_string(g_mpv, typeKey);
        bool match = (type && strcmp(type, wantType) == 0);
        if (type) fn_mpv_free(type);
        if (!match) continue;

        if (idx == index) {
            /* Try title first */
            char titleKey[80];
            _snprintf_s(titleKey, sizeof(titleKey), _TRUNCATE, "track-list/%lld/title", (long long)i);
            char* title = fn_mpv_get_property_string(g_mpv, titleKey);
            if (title) {
                std::wstring result = Utf8ToWide(title);
                fn_mpv_free(title);
                return result;
            }
            /* Try lang */
            char langKey[80];
            _snprintf_s(langKey, sizeof(langKey), _TRUNCATE, "track-list/%lld/lang", (long long)i);
            char* lang = fn_mpv_get_property_string(g_mpv, langKey);
            if (lang) {
                std::wstring result = Utf8ToWide(lang);
                fn_mpv_free(lang);
                return result;
            }
            return L"Track " + std::to_wstring(index + 1);
        }
        idx++;
    }
    return L"";
}

/* ================================================================
 *  Event thread
 * ================================================================ */
static DWORD WINAPI MPVEventThread(LPVOID /*param*/)
{
    while (g_mpvThreadRunning.load()) {
        mpv_event* ev = fn_mpv_wait_event(g_mpv, 0.1);
        if (!ev || ev->event_id == MPV_EVENT_NONE) continue;

        switch (ev->event_id) {

        case MPV_EVENT_PROPERTY_CHANGE: {
            auto* prop = static_cast<mpv_event_property*>(ev->data);
            if (!prop) break;

            if (strcmp(prop->name, "time-pos") == 0 &&
                prop->format == MPV_FORMAT_DOUBLE && prop->data)
            {
                g_mpvPosition.store(*static_cast<double*>(prop->data));
            }
            else if (strcmp(prop->name, "duration") == 0 &&
                     prop->format == MPV_FORMAT_DOUBLE && prop->data)
            {
                g_mpvDuration.store(*static_cast<double*>(prop->data));
            }
            else if (strcmp(prop->name, "pause") == 0 &&
                     prop->format == MPV_FORMAT_FLAG && prop->data)
            {
                g_mpvPaused.store(*static_cast<int*>(prop->data) != 0);
            }
            else if (strcmp(prop->name, "idle-active") == 0 &&
                     prop->format == MPV_FORMAT_FLAG && prop->data)
            {
                g_mpvIdle.store(*static_cast<int*>(prop->data) != 0);
            }
            else if (strcmp(prop->name, "eof-reached") == 0 &&
                     prop->format == MPV_FORMAT_FLAG && prop->data)
            {
                bool eof = *static_cast<int*>(prop->data) != 0;
                g_mpvEofReached.store(eof);
                if (eof)
                    PostMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(204, 0), 0);
            }
            break;
        }

        case MPV_EVENT_END_FILE: {
            auto* ef = static_cast<mpv_event_end_file*>(ev->data);
            if (ef && ef->reason == MPV_END_FILE_REASON_EOF)
                PostMessageW(g_hwnd, WM_COMMAND, MAKEWPARAM(204, 0), 0);
            break;
        }

        case MPV_EVENT_SHUTDOWN:
            g_mpvThreadRunning.store(false);
            break;

        default:
            break;
        }
    }
    return 0;
}

/* ================================================================
 *  Initialization / teardown
 * ================================================================ */

bool IsMPVAvailable()
{
    static int cached = -1;
    if (cached < 0) cached = LoadMPVLibrary() ? 1 : 0;
    return cached == 1;
}

bool InitMPV(HWND parentHwnd)
{
    if (g_mpv) return true;
    if (!LoadMPVLibrary()) return false;

    g_mpv = fn_mpv_create();
    if (!g_mpv) return false;

    /* Embed video inside our window */
    __int64 wid = (__int64)(intptr_t)parentHwnd;
    fn_mpv_set_option(g_mpv, "wid", MPV_FORMAT_INT64, &wid);

    /* Video output & hardware decoding */
    fn_mpv_set_option_string(g_mpv, "vo", "gpu");
    fn_mpv_set_option_string(g_mpv, "hwdec", "auto");

    /* Keep the window open when playback ends (we handle auto-advance) */
    fn_mpv_set_option_string(g_mpv, "keep-open", "yes");

    /* Disable mpv's own key bindings -- MediaAccess handles all hotkeys */
    fn_mpv_set_option_string(g_mpv, "input-default-bindings", "no");
    fn_mpv_set_option_string(g_mpv, "input-vo-keyboard", "no");

    /* OSD level 1: show seek/time on OSD */
    fn_mpv_set_option_string(g_mpv, "osd-level", "1");

    /* Enable yt-dlp integration for URL playback */
    fn_mpv_set_option_string(g_mpv, "ytdl", "yes");

    /* Tell mpv's ytdl_hook where our bundled yt-dlp.exe lives so it does not
     * rely on PATH lookup. g_ytdlpPath is populated by settings.cpp auto-
     * detection (prefers the auto-updated %LOCALAPPDATA% copy if present,
     * otherwise falls back to the bundled <install>\lib\yt-dlp.exe). */
    if (!g_ytdlpPath.empty()) {
        int len = WideCharToMultiByte(CP_UTF8, 0, g_ytdlpPath.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len > 1) {
            std::string utf8Path(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, g_ytdlpPath.c_str(), -1,
                                &utf8Path[0], len, nullptr, nullptr);
            /* mpv treats backslashes as escape characters inside script-opts
             * values; forward slashes work fine on Windows for file paths. */
            for (auto& c : utf8Path) if (c == '\\') c = '/';

            std::string scriptOpts = "ytdl_hook-ytdl_path=" + utf8Path;
            fn_mpv_set_option_string(g_mpv, "script-opts", scriptOpts.c_str());
        }
    }

    /* Network cache settings for smoother streaming (especially livestreams
     * and long YouTube videos). */
    fn_mpv_set_option_string(g_mpv, "cache", "yes");
    fn_mpv_set_option_string(g_mpv, "demuxer-max-bytes", "150MiB");

    /* Subtitle auto-detection */
    fn_mpv_set_option_string(g_mpv, "sub-auto", "fuzzy");

    if (fn_mpv_initialize(g_mpv) < 0) {
        fn_mpv_destroy(g_mpv);
        g_mpv = nullptr;
        return false;
    }

    /* Observe properties we need to track */
    fn_mpv_observe_property(g_mpv, 0, "time-pos",     MPV_FORMAT_DOUBLE);
    fn_mpv_observe_property(g_mpv, 0, "duration",     MPV_FORMAT_DOUBLE);
    fn_mpv_observe_property(g_mpv, 0, "pause",        MPV_FORMAT_FLAG);
    fn_mpv_observe_property(g_mpv, 0, "idle-active",  MPV_FORMAT_FLAG);
    fn_mpv_observe_property(g_mpv, 0, "eof-reached",  MPV_FORMAT_FLAG);

    /* Start event-processing thread */
    g_mpvThreadRunning.store(true);
    g_mpvThread = CreateThread(nullptr, 0, MPVEventThread, nullptr, 0, nullptr);
    return true;
}

void FreeMPV()
{
    if (!g_mpv) return;

    g_mpvThreadRunning.store(false);
    if (g_mpvThread) {
        WaitForSingleObject(g_mpvThread, 3000);
        CloseHandle(g_mpvThread);
        g_mpvThread = nullptr;
    }

    fn_mpv_terminate_destroy(g_mpv);
    g_mpv = nullptr;

    /* Reset cached state */
    g_mpvPosition.store(0.0);
    g_mpvDuration.store(0.0);
    g_mpvPaused.store(true);
    g_mpvIdle.store(true);
    g_mpvEofReached.store(false);
}

bool IsMPVInitialized()
{
    return g_mpv != nullptr;
}

/* ================================================================
 *  Video file detection
 * ================================================================ */

bool IsVideoFile(const std::wstring& path)
{
    if (!IsMPVAvailable()) return false;

    const wchar_t* ext = wcsrchr(path.c_str(), L'.');
    if (!ext) return false;

    /* Unambiguous video extensions.  .mp4 and .wmv are excluded because
       BASS already handles those as audio containers. */
    static const wchar_t* videoExts[] = {
        L".mkv", L".avi", L".mov", L".webm", L".flv", L".ts", L".m2ts",
        L".vob", L".ogv", L".3gp", L".mpg", L".mpeg", L".m4v", L".divx",
        L".rmvb"
    };
    for (auto ve : videoExts) {
        if (_wcsicmp(ext, ve) == 0) return true;
    }
    return false;
}

/* ================================================================
 *  Playback
 * ================================================================ */

bool MPVLoadFile(const wchar_t* path)
{
    if (!g_mpv) return false;
    std::string utf8 = WideToUtf8(std::wstring(path));
    const char* cmd[] = {"loadfile", utf8.c_str(), nullptr};
    g_mpvEofReached.store(false);
    g_mpvIdle.store(false);
    // Force pause off before loadfile: mpv's `pause` property is process-
    // wide and persists across loadfile calls. If the previous media was
    // paused (user pressed Space, opened a different file, etc.), the new
    // media would silently load and stay paused. Always start fresh.
    int flag = 0;
    fn_mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &flag);
    return fn_mpv_command(g_mpv, cmd) == 0;
}

bool MPVLoadURL(const wchar_t* url)
{
    /* mpv handles URLs the same way as local files */
    return MPVLoadFile(url);
}

void MPVPlay()
{
    if (!g_mpv) return;
    int flag = 0;
    fn_mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MPVPause()
{
    if (!g_mpv) return;
    int flag = 1;
    fn_mpv_set_property(g_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MPVPlayPause()
{
    if (!g_mpv) return;
    if (g_mpvPaused.load()) MPVPlay(); else MPVPause();
}

void MPVStop()
{
    if (!g_mpv) return;
    const char* cmd[] = {"stop", nullptr};
    fn_mpv_command(g_mpv, cmd);
    g_mpvPosition.store(0.0);
    g_mpvDuration.store(0.0);
    g_mpvIdle.store(true);
}

// Toggle mpv's video output. Setting "vid" to "no" disables video decoding
// entirely (no demuxing, no decoder, no window). Useful for YouTube hybrid
// streaming where we only want the audio. Should be called BEFORE the next
// LoadFile/LoadURL — mpv applies the property to the next loaded media.
void MPVSetAudioOnly(bool audioOnly)
{
    if (!g_mpv) return;
    fn_mpv_set_property_string(g_mpv, "vid", audioOnly ? "no" : "auto");
}

/* ================================================================
 *  Seeking
 * ================================================================ */

void MPVSeek(double seconds)
{
    if (!g_mpv) return;
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f", seconds);
    const char* cmd[] = {"seek", buf, "relative", nullptr};
    fn_mpv_command(g_mpv, cmd);
}

void MPVSeekToPosition(double seconds)
{
    if (!g_mpv) return;
    char buf[64];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%.1f", seconds);
    const char* cmd[] = {"seek", buf, "absolute", nullptr};
    fn_mpv_command(g_mpv, cmd);
}

double MPVGetPosition()
{
    return g_mpvPosition.load();
}

double MPVGetLength()
{
    return g_mpvDuration.load();
}

/* ================================================================
 *  Volume
 * ================================================================ */

void MPVSetVolume(float vol)
{
    if (!g_mpv) return;
    /* MediaAccess volume range is 0.0-4.0 (with amplify); mpv expects 0-400. */
    double v = static_cast<double>(vol) * 100.0;
    fn_mpv_set_property(g_mpv, "volume", MPV_FORMAT_DOUBLE, &v);
}

void MPVSetMute(bool mute)
{
    if (!g_mpv) return;
    int flag = mute ? 1 : 0;
    fn_mpv_set_property(g_mpv, "mute", MPV_FORMAT_FLAG, &flag);
}

/* ================================================================
 *  State queries
 * ================================================================ */

bool MPVIsPlaying()
{
    return g_mpv && !g_mpvPaused.load() && !g_mpvIdle.load();
}

bool MPVIsPaused()
{
    return g_mpv && g_mpvPaused.load() && !g_mpvIdle.load();
}

bool MPVIsStopped()
{
    return !g_mpv || g_mpvIdle.load();
}

/* ================================================================
 *  Fullscreen
 * ================================================================ */

void MPVToggleFullscreen(HWND mainHwnd)
{
    g_fullscreen = !g_fullscreen;

    if (g_fullscreen) {
        /* Save current geometry */
        GetWindowRect(mainHwnd, &g_savedWindowRect);
        g_savedWindowStyle = GetWindowLongW(mainHwnd, GWL_STYLE);

        /* Switch to borderless fullscreen on the current monitor */
        SetWindowLongW(mainHwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);

        HMONITOR hMon = MonitorFromWindow(mainHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);

        SetWindowPos(mainHwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right  - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);

        /* Hide chrome (status bar, menu) */
        ShowWindow(g_statusBar, SW_HIDE);
        SetMenu(mainHwnd, nullptr);

        Speak(Ts("Fullscreen"));
    } else {
        /* Restore windowed mode */
        SetWindowLongW(mainHwnd, GWL_STYLE, g_savedWindowStyle);
        SetWindowPos(mainHwnd, nullptr,
                     g_savedWindowRect.left,  g_savedWindowRect.top,
                     g_savedWindowRect.right  - g_savedWindowRect.left,
                     g_savedWindowRect.bottom - g_savedWindowRect.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER);

        ShowWindow(g_statusBar, SW_SHOW);
        /* Reload the main menu from resources (IDM_MAIN_MENU = 100) */
        SetMenu(mainHwnd, LoadMenuW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(100)));

        Speak(Ts("Windowed"));
    }

    /* Trigger WM_SIZE so the video area is relaid out */
    PostMessageW(mainHwnd, WM_SIZE, 0, 0);
}

bool MPVIsFullscreen()
{
    return g_fullscreen;
}

/* ================================================================
 *  Subtitles
 * ================================================================ */

int MPVGetSubtitleTrackCount()
{
    return CountTracksByType("sub");
}

std::wstring MPVGetSubtitleTrackName(int index)
{
    return GetTrackNameByType("sub", index);
}

void MPVSetSubtitleTrack(int index)
{
    if (!g_mpv) return;
    if (index <= 0) {
        fn_mpv_set_property_string(g_mpv, "sid", "no");
        Speak(Ts("Subtitles off"));
    } else {
        /* Map our zero-based visible index to mpv track ID.
           Walk track-list to find the Nth sub track's ID. */
        __int64 count = 0;
        fn_mpv_get_property(g_mpv, "track-list/count", MPV_FORMAT_INT64, &count);
        int subIdx = 0;
        for (__int64 i = 0; i < count; i++) {
            char typeKey[80];
            _snprintf_s(typeKey, sizeof(typeKey), _TRUNCATE,
                        "track-list/%lld/type", (long long)i);
            char* type = fn_mpv_get_property_string(g_mpv, typeKey);
            bool isSub = (type && strcmp(type, "sub") == 0);
            if (type) fn_mpv_free(type);

            if (isSub) {
                if (subIdx == index - 1) {
                    char idKey[80];
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "track-list/%lld/id", (long long)i);
                    __int64 id = 0;
                    fn_mpv_get_property(g_mpv, idKey, MPV_FORMAT_INT64, &id);
                    char buf[16];
                    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", (long long)id);
                    fn_mpv_set_property_string(g_mpv, "sid", buf);
                    return;
                }
                subIdx++;
            }
        }
    }
}

void MPVCycleSubtitles()
{
    if (!g_mpv) return;
    const char* cmd[] = {"cycle", "sub", nullptr};
    fn_mpv_command(g_mpv, cmd);

    /* Announce the new subtitle track for accessibility */
    char* title = fn_mpv_get_property_string(g_mpv, "current-tracks/sub/title");
    if (title) {
        SpeakW((std::wstring(T("Subtitle: ")) + Utf8ToWide(title)).c_str());
        fn_mpv_free(title);
    } else {
        char* lang = fn_mpv_get_property_string(g_mpv, "current-tracks/sub/lang");
        if (lang) {
            SpeakW((std::wstring(T("Subtitle: ")) + Utf8ToWide(lang)).c_str());
            fn_mpv_free(lang);
        } else {
            Speak(Ts("Subtitles off"));
        }
    }
}

bool MPVLoadExternalSubtitle(const wchar_t* path)
{
    if (!g_mpv) return false;
    std::string utf8 = WideToUtf8(std::wstring(path));
    const char* cmd[] = {"sub-add", utf8.c_str(), nullptr};
    int err = fn_mpv_command(g_mpv, cmd);
    if (err == 0)
        Speak(Ts("Subtitle loaded"));
    else
        Speak(Ts("Failed to load subtitle"));
    return err == 0;
}

/* ================================================================
 *  Audio tracks
 * ================================================================ */

int MPVGetAudioTrackCount()
{
    return CountTracksByType("audio");
}

std::wstring MPVGetAudioTrackName(int index)
{
    return GetTrackNameByType("audio", index);
}

void MPVSetAudioTrack(int index)
{
    if (!g_mpv) return;
    if (index <= 0) {
        fn_mpv_set_property_string(g_mpv, "aid", "auto");
        return;
    }

    /* Map visible index to mpv track ID (same logic as subtitle) */
    __int64 count = 0;
    fn_mpv_get_property(g_mpv, "track-list/count", MPV_FORMAT_INT64, &count);
    int audIdx = 0;
    for (__int64 i = 0; i < count; i++) {
        char typeKey[80];
        _snprintf_s(typeKey, sizeof(typeKey), _TRUNCATE,
                    "track-list/%lld/type", (long long)i);
        char* type = fn_mpv_get_property_string(g_mpv, typeKey);
        bool isAudio = (type && strcmp(type, "audio") == 0);
        if (type) fn_mpv_free(type);

        if (isAudio) {
            if (audIdx == index - 1) {
                char idKey[80];
                _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                            "track-list/%lld/id", (long long)i);
                __int64 id = 0;
                fn_mpv_get_property(g_mpv, idKey, MPV_FORMAT_INT64, &id);
                char buf[16];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", (long long)id);
                fn_mpv_set_property_string(g_mpv, "aid", buf);
                return;
            }
            audIdx++;
        }
    }
}

void MPVCycleAudioTracks()
{
    if (!g_mpv) return;
    const char* cmd[] = {"cycle", "audio", nullptr};
    fn_mpv_command(g_mpv, cmd);

    /* Announce the new audio track for accessibility */
    char* title = fn_mpv_get_property_string(g_mpv, "current-tracks/audio/title");
    if (title) {
        SpeakW((std::wstring(T("Audio: ")) + Utf8ToWide(title)).c_str());
        fn_mpv_free(title);
    } else {
        char* lang = fn_mpv_get_property_string(g_mpv, "current-tracks/audio/lang");
        if (lang) {
            SpeakW((std::wstring(T("Audio: ")) + Utf8ToWide(lang)).c_str());
            fn_mpv_free(lang);
        }
    }
}

/* ================================================================
 *  Aspect ratio
 * ================================================================ */

void MPVCycleAspectRatio()
{
    if (!g_mpv) return;
    g_aspectIndex = (g_aspectIndex + 1) % g_aspectCount;
    fn_mpv_set_property_string(g_mpv, "video-aspect-override",
                               g_aspectRatios[g_aspectIndex]);
    SpeakW((std::wstring(T("Aspect: ")) + g_aspectNames[g_aspectIndex]).c_str());
}

std::wstring MPVGetCurrentAspectRatio()
{
    return g_aspectNames[g_aspectIndex];
}

/* ================================================================
 *  Speed
 * ================================================================ */

void MPVSetSpeed(double speed)
{
    if (!g_mpv) return;
    fn_mpv_set_property(g_mpv, "speed", MPV_FORMAT_DOUBLE, &speed);
}

/* ================================================================
 *  OSD
 * ================================================================ */

void MPVShowOSD(const wchar_t* text, int durationMs)
{
    if (!g_mpv) return;
    std::string utf8 = WideToUtf8(std::wstring(text));
    char durBuf[16];
    _snprintf_s(durBuf, sizeof(durBuf), _TRUNCATE, "%d", durationMs);
    const char* cmd[] = {"show-text", utf8.c_str(), durBuf, nullptr};
    fn_mpv_command(g_mpv, cmd);
}

/* ================================================================
 *  Screenshot
 * ================================================================ */

void MPVTakeScreenshot()
{
    if (!g_mpv) return;
    const char* cmd[] = {"screenshot", nullptr};
    fn_mpv_command(g_mpv, cmd);
    Speak(Ts("Screenshot saved"));
}

/* ================================================================
 *  Chapters
 * ================================================================ */

int MPVGetChapterCount()
{
    if (!g_mpv) return 0;
    __int64 count = 0;
    fn_mpv_get_property(g_mpv, "chapter-list/count", MPV_FORMAT_INT64, &count);
    return static_cast<int>(count);
}

std::wstring MPVGetChapterTitle(int index)
{
    if (!g_mpv) return L"";
    char key[80];
    _snprintf_s(key, sizeof(key), _TRUNCATE, "chapter-list/%d/title", index);
    char* title = fn_mpv_get_property_string(g_mpv, key);
    std::wstring result;
    if (title) {
        result = Utf8ToWide(title);
        fn_mpv_free(title);
    } else {
        result = std::wstring(T("Chapter ")) + std::to_wstring(index + 1);
    }
    return result;
}

void MPVSeekToChapter(int index)
{
    if (!g_mpv) return;
    __int64 ch = index;
    fn_mpv_set_property(g_mpv, "chapter", MPV_FORMAT_INT64, &ch);
    SpeakW(MPVGetChapterTitle(index).c_str());
}

/* ================================================================
 *  Metadata
 * ================================================================ */

std::wstring MPVGetMediaTitle()
{
    if (!g_mpv) return L"";
    char* title = fn_mpv_get_property_string(g_mpv, "media-title");
    std::wstring result;
    if (title) {
        result = Utf8ToWide(title);
        fn_mpv_free(title);
    }
    return result;
}
