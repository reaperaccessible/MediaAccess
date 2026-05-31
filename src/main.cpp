// =============================================================================
// main.cpp — wWinMain entry point, single-instance plumbing, and WndProc.
//
// Responsibilities, in order:
//   1) wWinMain: DLL search hardening, single-instance gate (mutex + WM_COPYDATA
//      relay using dwData sentinels 1/2/3/4 — see WM_COPYDATA below for the
//      schema), translations + keymap startup, message loop with conditional
//      accelerator translation.
//   2) WndProc: dispatch for every message the main window handles —
//      WM_CREATE init chain, WM_KEYDOWN keymap lookup, WM_COMMAND giant menu
//      switch, WM_DESTROY ordered shutdown (audio MUST die first), plus all
//      the niche handlers documented inline at each case.
//   3) Help-menu helpers (manual/readme/contact/donate/etc.) extracted here so
//      WM_COMMAND stays readable.
//
// Anything heavier (player, settings, video, accessibility, ...) lives in its
// own translation unit — this file is the wiring, not the logic.
// =============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>   // CoInitializeEx for TTS

#include "globals.h"
#include "player.h"
#include "settings.h"
#include "translations.h"
#include "hotkeys.h"
#include "tray.h"
#include "accessibility.h"
#include "ui.h"
#include "effects.h"
#include "database.h"
#include "youtube.h"
#include "download_manager.h"
#include "updater.h"
#include "resource.h"
#include "video_engine.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/keyboard_help.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/keymap.h"
#include "mediaaccess/actions_window.h"
#include "mediaaccess/daisy_book.h"
#include "mediaaccess/daisy_player.h"
#include "mediaaccess/sleep_timer.h"
#include "mediaaccess/books_dialog.h"
#include "mediaaccess/tts_player.h"
#include "mediaaccess/book_text_window.h"
#include "mediaaccess/cli_switches.h"   // v1.63
#include "mediaaccess/audio_slots.h"    // v1.63
#include <utility>  // for std::pair

// Custom message posted from daisy_player.cpp BASS sync (worker thread).
#define WM_DAISY_NEXT_CLIP_LOCAL (WM_USER + 50)
extern "C" void DaisyOnClipEnded(int endedClipIndex);
extern "C" void DaisyOnTtsEndOfStream();

#pragma comment(lib, "bass.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ParseCommandLine();

// Declarations from ui.cpp
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles);

// -----------------------------------------------------------------------------
// Named magic numbers
// -----------------------------------------------------------------------------
// Initial window size on first launch (before settings restore). Picked to be
// tall enough for the menu bar + a one-line status bar.
static constexpr int  DEFAULT_WINDOW_WIDTH  = 500;
static constexpr int  DEFAULT_WINDOW_HEIGHT = 150;
// Auto-hide cursor timer used during fullscreen video playback.
static constexpr UINT IDT_FULLSCREEN_CURSOR_HIDE = 410;
static constexpr UINT FULLSCREEN_CURSOR_HIDE_MS  = 3000;
// Sleep-timer quick-pick presets (resource.h defines IDM_SLEEP_PRESET_BASE = 7300;
// each menu slot is BASE + index into kSleepPresetMinutes).
static constexpr int kSleepPresetMinutes[] = { 15, 30, 45, 60, 90, 120 };

// -----------------------------------------------------------------------------
// Help-menu command helpers — moved out of the WM_COMMAND switch so each long
// path-building / Shell-execute block stays readable. Each helper is a direct
// extraction with no behavior change.
// -----------------------------------------------------------------------------
static void OpenHelpManual(HWND hwnd) {
    // Open the bilingual HTML manual in the user's default browser.
    // We pick the file matching the current app language; user can
    // switch languages from the link inside the manual.
    wchar_t manualPath[MAX_PATH];
    GetModuleFileNameW(nullptr, manualPath, MAX_PATH);
    wchar_t* slash = wcsrchr(manualPath, L'\\');
    if (!slash) return;
    *(slash + 1) = L'\0';
    const char* lang = GetCurrentLanguage();
    const wchar_t* file = (lang && strcmp(lang, "fr") == 0)
        ? L"docs\\manual_fr.html"
        : L"docs\\manual_en.html";
    wcscat_s(manualPath, MAX_PATH, file);
    HINSTANCE r = ShellExecuteW(hwnd, L"open", manualPath, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(hwnd,
            T("Could not open the manual. Make sure the docs folder is present alongside MediaAccess.exe."),
            T("Manual"), MB_OK | MB_ICONWARNING);
    }
}

static void OpenHelpReadme(HWND hwnd) {
    wchar_t readmePath[MAX_PATH];
    GetModuleFileNameW(nullptr, readmePath, MAX_PATH);
    wchar_t* slash = wcsrchr(readmePath, L'\\');
    if (!slash) return;
    *(slash + 1) = L'\0';
    wcscat_s(readmePath, MAX_PATH, L"docs\\readme.txt");
    HINSTANCE r = ShellExecuteW(hwnd, L"open", readmePath, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(hwnd,
            T("Could not open readme.txt. Make sure the docs folder is present alongside MediaAccess.exe."),
            T("Readme"), MB_OK | MB_ICONWARNING);
    }
}

static void OpenHelpContact(HWND hwnd) {
    // mailto: link — Windows opens the default mail client pre-filled with
    // our address + a subject line that helps us triage.
    const wchar_t* url =
        L"mailto:reaperaccessible@gmail.com"
        L"?subject=MediaAccess%20request";
    HINSTANCE r = ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(hwnd,
            T("Could not open your email client. Please write to "
              "reaperaccessible@gmail.com manually."),
            T("Contact us"), MB_OK | MB_ICONINFORMATION);
    }
}

static void OpenHelpDonate(HWND hwnd) {
    // PayPal donation page for reaperaccessible. Opens in the user's default browser.
    const wchar_t* url =
        L"https://www.paypal.com/donate/"
        L"?business=6RZ8Y2Q39B9LN"
        L"&no_recurring=0"
        L"&item_name=Thanks+to+your+donation%2C+REAPERACCESSIBLE"
        L"+provides+training%2C+tutorials%2C+and+tools+to+make"
        L"+REAPER+accessible+to+blind+users."
        L"&currency_code=USD";
    HINSTANCE r = ShellExecuteW(hwnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) {
        MessageBoxW(hwnd,
            T("Could not open the donation page in your browser."),
            T("Make a donation"), MB_OK | MB_ICONWARNING);
    }
}

static void HelpSetAsDefault(HWND hwnd) {
    // Microsoft removed the programmatic "set as default" APIs in Windows 8+
    // to prevent app hijacking. Every media player (VLC, foobar2000, …) has
    // the same limitation. The best we can do is take the user straight to
    // the right Settings page and tell them exactly which buttons to click.
    MessageBoxW(hwnd,
        T("Windows does not allow any application to set itself as the default "
          "automatically — this is a security restriction Microsoft added in "
          "Windows 8.\n\n"
          "When you click OK, Windows will open the Default apps page on the "
          "MediaAccess entry. From there, click each file type (.mp3, .mp4, "
          ".mkv, .flac, .mid, etc.) and choose MediaAccess to make it the "
          "default. On Windows 11 you can also use the \"Set default\" button "
          "near the top of the MediaAccess page to assign all supported types "
          "at once."),
        T("Set as default media player"),
        MB_OK | MB_ICONINFORMATION);
    ShellExecuteW(hwnd, L"open",
                  L"ms-settings:defaultapps?registeredAppMachine=MediaAccess",
                  nullptr, nullptr, SW_SHOWNORMAL);
}

static void HelpClearYouTubeCache(HWND hwnd) {
    unsigned long long bytes = GetYouTubeCacheSize();
    double mb = bytes / (1024.0 * 1024.0);
    wchar_t prompt[256];
    swprintf(prompt, 256, L"%s\n\n%.1f MB", T("Clear all cached YouTube audio?"), mb);
    if (MessageBoxW(hwnd, prompt, T("Clear YouTube cache"),
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    int n = ClearYouTubeCache();
    wchar_t done[128];
    swprintf(done, 128, T("Removed %d cached files."), n);
    MessageBoxW(hwnd, done, T("Clear YouTube cache"), MB_OK | MB_ICONINFORMATION);
}

static void PasteMediaFromClipboard() {
    extern std::vector<std::wstring> GetFilesFromClipboard();
    // Ctrl+V on the main window: paste media files/URLs from the clipboard.
    // Replaces the current playlist (matching the "Open file" semantics —
    // paste behaves like a fresh open).
    std::vector<std::wstring> files;
    try {
        files = GetFilesFromClipboard();
    } catch (...) {}
    if (files.empty()) {
        Speak(Ts("No media in clipboard"));
        return;
    }
    g_playlist.clear();
    g_currentTrack = -1;
    for (const auto& f : files) g_playlist.push_back(f);
    PlayTrack(0);
    if (files.size() == 1) {
        Speak(Ts("Pasted 1 item"));
    } else {
        Speak(std::to_string(files.size()) + " " + Ts("items pasted"));
    }
}

static void OpenSubtitleFile(HWND hwnd) {
    if (g_activeEngine != PlaybackEngine::MPV) return;
    wchar_t fname[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Subtitle Files\0*.srt;*.ass;*.ssa;*.sub;*.vtt\0All Files\0*.*\0";
    ofn.lpstrFile = fname;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) MPVLoadExternalSubtitle(fname);
}

// Dispatch dynamic-range command IDs (audio devices, recent files, effect
// presets). Returns true if the cmdId was handled.
static bool DispatchDynamicRangeCommand(WORD cmdId) {
    if (cmdId >= IDM_AUDIO_DEVICE_BASE && cmdId < IDM_AUDIO_DEVICE_BASE + 100) {
        SelectAudioDevice(cmdId - IDM_AUDIO_DEVICE_BASE);
        return true;
    }
    if (cmdId >= IDM_FILE_RECENT_BASE && cmdId < IDM_FILE_RECENT_BASE + MAX_RECENT_FILES) {
        size_t idx = cmdId - IDM_FILE_RECENT_BASE;
        if (idx < g_recentFiles.size()) {
            g_playlist.clear();
            g_playlist.push_back(g_recentFiles[idx]);
            g_currentTrack = -1;
            PlayTrack(0);
        }
        return true;
    }
    if (cmdId >= IDM_PRESET_BASE && cmdId < IDM_PRESET_BASE + 100) {
        auto names = GetEffectPresetNames();
        size_t idx = cmdId - IDM_PRESET_BASE;
        if (idx < names.size() && LoadEffectPreset(names[idx])) {
            std::wstring msg = std::wstring(T("Loaded preset ")) + names[idx];
            SpeakW(msg);
        }
        return true;
    }
    if (cmdId >= IDM_PRESET_DELETE_BASE && cmdId < IDM_PRESET_DELETE_BASE + 100) {
        auto names = GetEffectPresetNames();
        size_t idx = cmdId - IDM_PRESET_DELETE_BASE;
        if (idx < names.size()) {
            std::wstring n = names[idx];
            if (DeleteEffectPreset(n)) {
                std::wstring msg = std::wstring(T("Deleted preset ")) + n;
                SpeakW(msg);
            }
        }
        return true;
    }
    if (cmdId == IDM_PRESET_SAVE_NEW) {
        ShowSaveEffectPresetDialog();
        return true;
    }
    return false;
}

// Parse command line arguments
void ParseCommandLine() {
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                std::wstring path = argv[i];
                if (IsPlaylistFile(path)) {
                    // Parse playlist and add its contents
                    auto entries = ParsePlaylist(path);
                    g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                } else {
                    g_playlist.push_back(path);
                }
            }
        }
        LocalFree(argv);
    }
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            HINSTANCE hInstance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

            INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
            InitCommonControlsEx(&icc);

            CreateStatusBar(hwnd, hInstance);

            // Create video child window (hidden by default, shown when video plays)
            g_videoHwnd = CreateWindowExW(0, L"STATIC", nullptr,
                WS_CHILD | WS_CLIPSIBLINGS | SS_BLACKRECT,
                0, 0, 100, 100, hwnd, nullptr, hInstance, nullptr);

            if (!InitBass(hwnd)) {
                return -1;
            }

            InitDatabase();
            InitEffects();
            LoadDSPSettings();
            InitSpeech(hwnd);
            RegisterGlobalHotkeys();

            // Set initial menu check states
            CheckMenuItem(GetMenu(hwnd), IDM_PLAY_SHUFFLE, g_shuffle ? MF_CHECKED : MF_UNCHECKED);

            // Localize main menu based on active language.
            LocalizeMenu(GetMenu(hwnd));
            DrawMenuBar(hwnd);

            SetTimer(hwnd, IDT_UPDATE_TITLE, UPDATE_INTERVAL, nullptr);
            SetTimer(hwnd, IDT_SCHEDULER, 60000, nullptr);  // Check schedules every minute
            g_startupTime = GetTickCount();

            if (!g_playlist.empty()) {
                int startIndex = 0;
                if (g_loadFolder && g_playlist.size() == 1) {
                    std::wstring singleFile = g_playlist[0];
                    startIndex = ExpandFileToFolder(singleFile, g_playlist);
                }
                PlayTrack(startIndex);
            }

            UpdateStatusBar();

            // Check for updates on startup (runs in background thread)
            CheckForUpdatesOnStartup();

            // Silently keep yt-dlp.exe up to date (background thread).
            LaunchYtdlpUpdateCheck();

            // v1.63 — drain any CLI switches that came in with our own
            // command line (the launcher stashed them in g_pendingCliCommands).
            // Posted so it runs after WM_CREATE returns and the message pump
            // gets to deliver — gives PlayTrack a chance to settle first.
            if (!g_pendingCliCommands.empty()) {
                PostMessageW(hwnd, WM_APP_CLI, 0, 0);
            }

            return 0;
        }

        case WM_SIZE:
            // Forward to the status bar so its built-in autosize layout runs,
            // then resize the video child to fill what's left above it.
            // When minimized AND the user opted in to tray-on-minimize, hide
            // the window now (returns 0 to suppress default behavior).
            if (g_statusBar) {
                SendMessageW(g_statusBar, WM_SIZE, 0, 0);
            }
            if (wParam != SIZE_MINIMIZED && g_videoHwnd) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                int sbH = 0;
                if (g_statusBar && IsWindowVisible(g_statusBar)) {
                    RECT sbRect;
                    GetWindowRect(g_statusBar, &sbRect);
                    sbH = sbRect.bottom - sbRect.top;
                }
                MoveWindow(g_videoHwnd, 0, 0, rc.right, rc.bottom - sbH, TRUE);
            }
            if (wParam == SIZE_MINIMIZED && g_minimizeToTray) {
                HideToTray(hwnd);
                return 0;
            }
            return 0;

        case WM_KEYDOWN:
            // -----------------------------------------------------------
            // Keyboard help (F12-toggled "describe key" mode)
            //
            // When g_keyboardHelpMode is on, every keypress in the main
            // window is announced instead of executed. F12 itself is
            // excluded so the user can always toggle the mode off.
            // -----------------------------------------------------------
            if (g_keyboardHelpMode && wParam != VK_F12) {
                std::string desc = DescribeKey(wParam, lParam);
                if (!desc.empty()) Speak(desc);
                return 0;
            }
            if (wParam == VK_F11 && g_isVideoPlaying) {
                PostMessageW(hwnd, WM_COMMAND, IDM_VIDEO_FULLSCREEN, 0);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_isFullscreen) {
                PostMessageW(hwnd, WM_COMMAND, IDM_VIDEO_FULLSCREEN, 0);
                return 0;
            }
            // -----------------------------------------------------------
            // Keymap dispatcher (REAPER-style)
            //
            // Build a Shortcut struct from the current VK + modifier state
            // and look it up in the active keymap. The keymap maps to an
            // action's IDM command, which we post via WM_COMMAND.
            //
            // Users customize bindings via Tools → Actions (F4). Regional
            // defaults live in <install>\KeyMaps\USA|FR-CA|FR-FR.MediaAccessKeyMap
            // and are auto-detected at first launch.
            //
            // Pure modifier presses (Ctrl/Shift/Alt/Win alone) are skipped
            // so they don't generate spurious lookups.
            // -----------------------------------------------------------
            switch ((UINT)wParam) {
                case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
                case VK_MENU:    case VK_LMENU:    case VK_RMENU:
                case VK_LWIN:    case VK_RWIN:
                case VK_CAPITAL: case VK_NUMLOCK:  case VK_SCROLL:
                    break;  // fall through to default break below
                default: {
                    mediaaccess::Shortcut sc;
                    sc.vk    = (UINT)wParam;
                    sc.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    sc.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
                    sc.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
                    sc.win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
                               ((GetKeyState(VK_RWIN) & 0x8000) != 0);  // v1.66
                    // When a DAISY book is loaded, Books-category bindings
                    // take precedence over Main-category bindings so the
                    // navigation keys (Shift+arrows, G, Shift+L, Shift+B)
                    // do the right thing for book reading.
                    int cmd = 0;
                    if (mediaaccess::DaisyIsActive()) {
                        cmd = mediaaccess::GetActiveKeyMap()
                                .FindCommandFor(sc, mediaaccess::ActionCategory::Books);
                    }
                    if (cmd == 0) {
                        cmd = mediaaccess::GetActiveKeyMap()
                                .FindCommandFor(sc, mediaaccess::ActionCategory::Main);
                    }
                    if (cmd != 0) {
                        PostMessageW(hwnd, WM_COMMAND, cmd, 0);
                        return 0;
                    }
                    break;
                }
            }
            break;

        case WM_LBUTTONDBLCLK:
            if (g_isVideoPlaying) {
                PostMessageW(hwnd, WM_COMMAND, IDM_VIDEO_FULLSCREEN, 0);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_isFullscreen) {
                while (ShowCursor(TRUE) < 0) {}
                SetTimer(hwnd, IDT_FULLSCREEN_CURSOR_HIDE, FULLSCREEN_CURSOR_HIDE_MS, nullptr);
            }
            break;

        case WM_RBUTTONUP:
            if (g_isVideoPlaying) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_FULLSCREEN,  T("Fullscreen\tF11"));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_SUB_CYCLE,   T("Cycle Subtitles"));
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_SUB_LOAD,    T("Load Subtitle File..."));
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_AUDIO_CYCLE, T("Cycle Audio Track"));
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_ASPECT,      T("Cycle Aspect Ratio"));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_VIDEO_SCREENSHOT,  T("Take Screenshot"));
                POINT pt;
                GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                return 0;
            }
            break;

        case WM_TIMER:
            // Multiplexed timer handler — wParam is the timer ID:
            //   IDT_UPDATE_TITLE         — periodic status-bar refresh.
            //   IDT_BATCH_FILES          — coalesce a burst of WM_COPYDATA file
            //                              drops into one PlayTrack() call so
            //                              "Open with…" of 30 files doesn't
            //                              start/stop the engine 30 times.
            //   IDT_SCHEDULER            — once-per-minute scheduled-event check.
            //   IDT_SCHED_DURATION       — scheduled fade/stop fire.
            //   IDT_FULLSCREEN_CURSOR_HIDE — auto-hide cursor during fullscreen
            //                              video after FULLSCREEN_CURSOR_HIDE_MS
            //                              of mouse inactivity.
            //   IDT_SLEEP_TIMER          — sleep-timer 1s tick.
            if (wParam == IDT_UPDATE_TITLE) {
                UpdateStatusBar();
            } else if (wParam == IDT_BATCH_FILES) {
                KillTimer(hwnd, IDT_BATCH_FILES);
                if (!g_pendingFiles.empty()) {
                    int startIndex = 0;
                    if (g_loadFolder && g_pendingFiles.size() == 1) {
                        startIndex = ExpandFileToFolder(g_pendingFiles[0], g_playlist);
                        g_pendingFiles.clear();
                    } else {
                        g_playlist = std::move(g_pendingFiles);
                        g_pendingFiles.clear();
                    }
                    PlayTrack(startIndex);
                    if (g_bringToFront) {
                        if (!IsWindowVisible(hwnd)) {
                            RestoreFromTray(hwnd);
                        } else {
                            SetForegroundWindow(hwnd);
                            if (IsIconic(hwnd)) {
                                ShowWindow(hwnd, SW_RESTORE);
                            }
                        }
                    }
                }
            } else if (wParam == IDT_SCHEDULER) {
                CheckScheduledEvents();
            } else if (wParam == IDT_SCHED_DURATION) {
                KillTimer(hwnd, IDT_SCHED_DURATION);
                HandleScheduledDurationEnd();
            } else if (wParam == IDT_FULLSCREEN_CURSOR_HIDE) {
                KillTimer(hwnd, IDT_FULLSCREEN_CURSOR_HIDE);
                if (g_isFullscreen) {
                    POINT pt;
                    GetCursorPos(&pt);
                    RECT rc;
                    GetWindowRect(hwnd, &rc);
                    if (PtInRect(&rc, pt)) while (ShowCursor(FALSE) >= 0) {}
                }
            } else if (wParam == IDT_SLEEP_TIMER) {
                mediaaccess::SleepOnTick();
            }
            return 0;

        case WM_SPEAK:
            DoSpeak();
            return 0;

        case WM_META_CHANGED:
            AnnounceStreamMetadata();
            UpdateWindowTitle();
            return 0;

        case WM_ACTIVATEAPP:
            // v1.59 — when MediaAccess gains app-level focus (Alt+Tab from
            // another app, taskbar click, tray restore), speak the current
            // track / station / book title via SpeakTagTitle so the screen
            // reader user immediately knows what's playing without having
            // to press a shortcut. Gated by Options > Playback > "Announce
            // track when MediaAccess gets focus" (default ON).
            //
            // wParam is TRUE on activation, FALSE on deactivation. We only
            // act on activation.
            if (wParam && g_announceTrackOnFocus) {
                // Only announce when something is actually loaded.
                // SpeakTagTitle returns silently if nothing to say.
                bool somethingPlaying =
                    (g_currentTrack >= 0 &&
                     g_currentTrack < (int)g_playlist.size()) ||
                    g_activeEngine == PlaybackEngine::MPV ||
                    mediaaccess::DaisyIsActive();
                if (somethingPlaying) {
                    SpeakTagTitle();
                }
            }
            return 0;

        case WM_YT_HYBRID_READY: {
            // YouTube hybrid background download finished — swap mpv -> BASS.
            std::wstring* idPtr = reinterpret_cast<std::wstring*>(lParam);
            if (idPtr) {
                YouTubeOnHybridDownloadReady(*idPtr);
                delete idPtr;
            }
            return 0;
        }

        case WM_USER + 200: {
            // Update check result
            auto* data = reinterpret_cast<std::pair<UpdateInfo, bool>*>(lParam);
            if (data) {
                HandleUpdateCheckResult(hwnd, &data->first, data->second);
                delete data;
            }
            return 0;
        }

        case WM_USER + 201:
            // Apply downloaded update
            ApplyUpdate();
            return 0;

        case WM_DAISY_NEXT_CLIP_LOCAL:
            // BASS worker thread told us a clip finished — advance on the
            // main thread where we can safely free streams.
            DaisyOnClipEnded((int)wParam);
            return 0;

        case mediaaccess::WM_TTS_END_OF_STREAM:
            // SAPI told us the current paragraph finished speaking — advance.
            DaisyOnTtsEndOfStream();
            return 0;

        case WM_HOTKEY: {
            // System-wide hotkey fired (RegisterHotKey from hotkeys.cpp).
            // IDs 0x7F00..0x7F03 are reserved for the always-on media keys
            // (Play/Pause / Stop / Prev / Next); everything else matches a
            // user-defined entry in g_hotkeys produced by
            // SyncGlobalHotkeysFromKeymap() out of the keymap's Global category.
            int hotkeyId = static_cast<int>(wParam);

            switch (hotkeyId) {
                case 0x7F00: // HOTKEY_ID_MEDIA_PLAYPAUSE
                    PostMessage(hwnd, WM_COMMAND, IDM_PLAY_PLAYPAUSE, 0);
                    return 0;
                case 0x7F01: // HOTKEY_ID_MEDIA_STOP
                    PostMessage(hwnd, WM_COMMAND, IDM_PLAY_STOP, 0);
                    return 0;
                case 0x7F02: // HOTKEY_ID_MEDIA_PREV
                    PostMessage(hwnd, WM_COMMAND, IDM_PLAY_PREV, 0);
                    return 0;
                case 0x7F03: // HOTKEY_ID_MEDIA_NEXT
                    PostMessage(hwnd, WM_COMMAND, IDM_PLAY_NEXT, 0);
                    return 0;
            }

            // Handle user-defined hotkeys. Prefer commandId (set by the
            // keymap-driven global sync); fall back to the legacy actionIdx
            // path for hotkeys that came in from the old [Hotkeys] INI.
            for (const auto& hk : g_hotkeys) {
                if (hk.id == hotkeyId) {
                    int cmd = (hk.commandId != 0)
                                ? hk.commandId
                                : g_hotkeyActions[hk.actionIdx].commandId;
                    PostMessage(hwnd, WM_COMMAND, cmd, 0);
                    break;
                }
            }
            return 0;
        }

        case WM_TRAYICON:
            // Notification icon callback. lParam is the mouse event the shell
            // forwarded; we only care about double-click (restore) and right-up
            // (context menu).
            if (lParam == WM_LBUTTONDBLCLK) {
                RestoreFromTray(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COPYDATA: {
            // Cross-process inbox from a second launch of MediaAccess.exe.
            // The launcher in wWinMain encodes intent via cds->dwData:
            //   1 — first file in a "Open with..." batch (lpData = wide path)
            //   2 — subsequent file in that same batch
            //   3 — bare relaunch (no payload) → restore window, foreground it
            //   4 — CLI switch payload ("verb\0param\0" UTF-16) — see cli_switches
            // Sentinels 1/2 funnel through a brief BATCH_DELAY coalescing
            // window (IDT_BATCH_FILES) so Explorer's per-file SendMessage
            // storm doesn't restart playback once per file.
            COPYDATASTRUCT* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            // v1.63 — if we're already tearing down (WM_DESTROY in flight),
            // drop late deliveries silently rather than touch a half-freed
            // BASS state.
            if (g_isShuttingDown) return TRUE;
            // v1.63 — dwData == 4: CLI switch ("verb\0param\0" UTF-16).
            if (cds && cds->dwData == 4 && cds->lpData && cds->cbData > 0) {
                CliCommand cmd;
                if (DecodeCliPayload(cds->lpData, cds->cbData, cmd)) {
                    ApplyCliCommand(hwnd, cmd, /*fromRemote=*/true);
                }
                return TRUE;
            }
            // dwData == 3: second launch with no file args — just bring the
            // existing window back. Fixes "desktop icon does nothing when
            // MediaAccess is hidden in the system tray".
            if (cds && cds->dwData == 3) {
                RestoreFromTray(hwnd);
                if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
                ShowWindow(hwnd, SW_SHOW);
                SetForegroundWindow(hwnd);
                return TRUE;
            }
            if (cds && (cds->dwData == 1 || cds->dwData == 2) && cds->lpData) {
                const wchar_t* filePath = static_cast<const wchar_t*>(cds->lpData);
                if (GetFileAttributesW(filePath) != INVALID_FILE_ATTRIBUTES) {
                    DWORD elapsed = GetTickCount() - g_startupTime;
                    std::wstring path = filePath;
                    if (!g_disableBatchDelay && elapsed < BATCH_DELAY && !g_playlist.empty()) {
                        if (IsPlaylistFile(path)) {
                            auto entries = ParsePlaylist(path);
                            g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                        } else {
                            g_playlist.push_back(path);
                        }
                    } else {
                        if (IsPlaylistFile(path)) {
                            auto entries = ParsePlaylist(path);
                            g_pendingFiles.insert(g_pendingFiles.end(), entries.begin(), entries.end());
                        } else {
                            g_pendingFiles.push_back(path);
                        }
                        SetTimer(hwnd, IDT_BATCH_FILES, g_disableBatchDelay ? 0 : BATCH_DELAY, nullptr);
                    }
                }
            }
            return TRUE;
        }

        case WM_INITMENUPOPUP:
            // Lazy-populate the "Recent files" submenu the instant a top-level
            // menu is about to open. Avoids dynamically tracking recent-files
            // changes during normal playback — we just rebuild on demand.
            // HIWORD(lParam) is TRUE for the window menu (Alt+Space); skip it.
            if (HIWORD(lParam) == FALSE) {
                HMENU hMenu = GetMenu(hwnd);
                if (hMenu) {
                    UpdateRecentFilesMenu(hMenu);
                }
            }
            break;

        case WM_COMMAND:
            // Giant menu/action dispatch. Most IDM_* ids come from either the
            // menu bar, the accelerator table, the keymap dispatcher above, or
            // global hotkeys (WM_HOTKEY). Dynamic-range IDs (recent files,
            // audio devices, effect presets) fall through to the default and
            // are routed by DispatchDynamicRangeCommand.
            switch (LOWORD(wParam)) {
                case IDM_FILE_OPEN:
                    ShowOpenDialog();
                    break;
                case IDM_FILE_ADD_FOLDER:
                    ShowAddFolderDialog();
                    break;
                case IDM_FILE_PLAYLIST:
                    ShowPlaylistDialog();
                    break;
                case IDM_FILE_OPEN_URL:
                    ShowOpenURLDialog();
                    break;
                case IDM_FILE_YOUTUBE:
                    ShowYouTubeDialog(hwnd);
                    break;
                case IDM_FILE_RADIO:
                    ShowRadioDialog();
                    break;
                case IDM_FILE_ADD_TO_FAVORITES:
                    AddCurrentStreamToFavorites();
                    break;
                case IDM_FILE_SCHEDULE:
                    ShowSchedulerDialog();
                    break;
                case IDM_FILE_PODCAST:
                    ShowPodcastDialog();
                    break;
                case IDM_FILE_EXIT:
                    PostQuitMessage(0);
                    break;
                case IDM_FILE_HIDE_TRAY:
                    HideToTray(hwnd);
                    break;
                case IDM_FILE_PASTE:
                    PasteMediaFromClipboard();
                    break;
                case IDM_TOOLS_OPTIONS:
                    ShowOptionsDialog();
                    break;
                case IDM_HELP_PLUGINS:
                    MessageBoxW(hwnd, GetLoadedPluginsInfo().c_str(), T("Loaded Plugins"), MB_OK | MB_ICONINFORMATION);
                    break;
                case IDM_HELP_TEST_YOUTUBE:
                    ShowTestYouTubePlayback();
                    break;
                case IDM_HELP_CLEAR_YT_CACHE:
                    HelpClearYouTubeCache(hwnd);
                    break;
                case IDM_HELP_UPDATES:
                    ShowCheckForUpdatesDialog(hwnd, false);
                    break;
                case IDM_HELP_SET_DEFAULT:
                    HelpSetAsDefault(hwnd);
                    break;
                case IDM_HELP_AUDIT_LAYOUT:
                    AuditOptionsLayout();
                    break;
                case IDM_KEYBOARD_HELP_TOGGLE:
                    g_keyboardHelpMode = !g_keyboardHelpMode;
                    Speak(Ts(g_keyboardHelpMode ? "Keyboard help on"
                                                : "Keyboard help off"));
                    break;
                case IDM_TOOLS_ACTIONS:
                    mediaaccess::ShowActionsWindow(hwnd);
                    break;
                // -----------------------------------------------------------
                // DAISY / EPUB book reader (v1.49 Phase 1)
                // -----------------------------------------------------------
                case IDM_FILE_OPEN_BOOK:
                    mediaaccess::OpenBookFromDialog(hwnd);
                    break;
                case IDM_TOOLS_BOOK_LIBRARY:
                    mediaaccess::ShowBookLibrary(hwnd);
                    break;
                case IDM_TOOLS_AUDIO_SLOTS:
                    ShowAudioSlotsDialog(hwnd);
                    break;
                case IDM_AUDIO_DEVICE_CYCLE:
                    CycleAudioDevice();
                    break;
                case IDM_AUDIO_DEVICE_SPEAK:
                    SpeakCurrentAudioDevice();
                    break;
                case IDM_BOOK_NAV_LEVEL_UP:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyCycleNavLevel(+1);
                    break;
                case IDM_BOOK_NAV_LEVEL_DOWN:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyCycleNavLevel(-1);
                    break;
                case IDM_BOOK_NAV_FORWARD:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyNavigateForward();
                    break;
                case IDM_BOOK_NAV_BACKWARD:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyNavigateBackward();
                    break;
                case IDM_BOOK_ANNOUNCE_LOCATION:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyAnnounceCurrentLocation();
                    break;
                case IDM_BOOK_ADD_BOOKMARK_WITH_NOTE:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::PromptAddBookmark(hwnd);
                    break;
                case IDM_BOOK_GO_TO_PAGE:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::PromptGoToPage(hwnd);
                    break;
                case IDM_BOOK_TOGGLE_TEXT_WINDOW:
                    mediaaccess::BookTextWindowToggle(hwnd);
                    break;
                case IDM_BOOK_SEARCH:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::PromptSearchInBook(hwnd);
                    break;
                case IDM_BOOK_BOOKMARK_LIST:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::ShowBookmarkList(hwnd);
                    break;
                case IDM_BOOK_ANNOUNCE_PROGRESS:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::DaisyAnnounceProgress();
                    break;
                case IDM_BOOK_TOGGLE_SKIP:
                    if (mediaaccess::DaisyIsActive()) {
                        g_bookSkipBypass = !g_bookSkipBypass;
                        Speak(g_bookSkipBypass
                              ? Ts("Skip filter off")
                              : Ts("Skip filter on"));
                    }
                    break;
                case IDM_SLEEP_TIMER_OPEN: {
                    int minutes = 0;
                    if (mediaaccess::SleepPromptCustomMinutes(hwnd, minutes)) {
                        mediaaccess::SleepStart(minutes);
                    }
                    break;
                }
                case IDM_SLEEP_TIMER_CANCEL:
                    mediaaccess::SleepCancel();
                    break;
                case IDM_SLEEP_TIMER_SPEAK:
                    mediaaccess::SleepSpeakRemaining();
                    break;
                // Sleep-timer quick presets (IDM_SLEEP_PRESET_BASE + 0..5).
                case IDM_SLEEP_PRESET_BASE + 0: mediaaccess::SleepStart(kSleepPresetMinutes[0]); break;
                case IDM_SLEEP_PRESET_BASE + 1: mediaaccess::SleepStart(kSleepPresetMinutes[1]); break;
                case IDM_SLEEP_PRESET_BASE + 2: mediaaccess::SleepStart(kSleepPresetMinutes[2]); break;
                case IDM_SLEEP_PRESET_BASE + 3: mediaaccess::SleepStart(kSleepPresetMinutes[3]); break;
                case IDM_SLEEP_PRESET_BASE + 4: mediaaccess::SleepStart(kSleepPresetMinutes[4]); break;
                case IDM_SLEEP_PRESET_BASE + 5: mediaaccess::SleepStart(kSleepPresetMinutes[5]); break;
                // v1.63 — audio slot activations (IDM_AUDIO_SLOT_BASE + 0..9)
                case IDM_AUDIO_SLOT_BASE + 0: ActivateAudioSlot(0); break;
                case IDM_AUDIO_SLOT_BASE + 1: ActivateAudioSlot(1); break;
                case IDM_AUDIO_SLOT_BASE + 2: ActivateAudioSlot(2); break;
                case IDM_AUDIO_SLOT_BASE + 3: ActivateAudioSlot(3); break;
                case IDM_AUDIO_SLOT_BASE + 4: ActivateAudioSlot(4); break;
                case IDM_AUDIO_SLOT_BASE + 5: ActivateAudioSlot(5); break;
                case IDM_AUDIO_SLOT_BASE + 6: ActivateAudioSlot(6); break;
                case IDM_AUDIO_SLOT_BASE + 7: ActivateAudioSlot(7); break;
                case IDM_AUDIO_SLOT_BASE + 8: ActivateAudioSlot(8); break;
                case IDM_AUDIO_SLOT_BASE + 9: ActivateAudioSlot(9); break;
                case IDM_BOOK_SEARCH_NEXT:
                    if (mediaaccess::DaisyIsActive()) mediaaccess::FindNextInBook();
                    break;
                case IDM_HELP_MANUAL:
                    OpenHelpManual(hwnd);
                    break;
                case IDM_HELP_README:
                    OpenHelpReadme(hwnd);
                    break;
                case IDM_HELP_CONTACT:
                    OpenHelpContact(hwnd);
                    break;
                case IDM_HELP_DONATE:
                    OpenHelpDonate(hwnd);
                    break;
                case IDM_BOOKMARK_ADD:
                    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                        double pos = GetCurrentPosition();
                        int id = AddBookmark(g_playlist[g_currentTrack], pos);
                        if (id >= 0) {
                            Speak(Ts("Bookmark added"));
                        }
                    }
                    break;
                case IDM_BOOKMARK_LIST:
                    ShowBookmarksDialog();
                    break;
                case IDM_VIEW_SONG_HISTORY:
                    ShowSongHistoryDialog();
                    break;
                case IDM_PLAY_PLAYPAUSE:
                    // When a DAISY book is loaded, route transport to the
                    // book player instead of the audio file engine.
                    if (mediaaccess::DaisyIsActive()) { mediaaccess::DaisyPlayPause(); break; }
                    PlayPause();
                    break;
                case IDM_PLAY_PLAY:
                    if (mediaaccess::DaisyIsActive()) { mediaaccess::DaisyPlay(); break; }
                    Play();
                    break;
                case IDM_PLAY_PAUSE:
                    if (mediaaccess::DaisyIsActive()) { mediaaccess::DaisyPause(); break; }
                    Pause();
                    break;
                case IDM_PLAY_STOP:
                    if (mediaaccess::DaisyIsActive()) { mediaaccess::DaisyStop(); break; }
                    Stop();
                    break;
                case IDM_PLAY_PREV:
                    PrevTrack();
                    break;
                case IDM_PLAY_NEXT:
                    // lParam==1 means don't auto-play (e.g., when auto-advance is disabled)
                    NextTrack(lParam == 0);
                    break;
                case IDM_PLAY_SHUFFLE:
                    g_shuffle = !g_shuffle;
                    Speak(g_shuffle ? Ts("Shuffle on") : Ts("Shuffle off"));
                    CheckMenuItem(GetMenu(hwnd), IDM_PLAY_SHUFFLE, g_shuffle ? MF_CHECKED : MF_UNCHECKED);
                    SaveSettings();
                    break;
                case IDM_PLAY_REPEAT_TOGGLE:
                    ToggleRepeatMode();
                    break;
                case IDM_EFFECT_PRESETS:
                    ShowEffectPresetsMenu(hwnd);
                    break;
                case IDM_PLAY_BEGINNING:
                    SeekToPosition(0);
                    break;
                case IDM_PLAY_NEAR_END: {
                    // Jump to 30 seconds before the end of the track (or
                    // to the very beginning for tracks shorter than 30 s).
                    // Useful for previewing endings, skipping bonus tracks,
                    // or finding where an audiobook chapter wraps up.
                    double len = GetTrackLength();
                    if (len <= 0.0) break;  // length unknown (e.g. live stream)
                    double target = len - 30.0;
                    if (target < 0.0) target = 0.0;
                    SeekToPosition(target);
                    break;
                }
                case IDM_PLAY_JUMPTOTIME:
                    ShowJumpToTimeDialog();
                    break;
                case IDM_PLAY_SEEKBACK:
                    if (mediaaccess::DaisyIsActive()) {
                        mediaaccess::DaisySeekRelative(-GetCurrentSeekAmount());
                        break;
                    }
                    if (g_currentSeekIndex == 12) {
                        // Chapter seeking
                        if (!g_chapters.empty()) {
                            SeekToPrevChapter();
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                                Seek(-g_seekAmounts[i].value);
                                break;
                            }
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
                        SeekTracks(-static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
                    } else {
                        Seek(-GetCurrentSeekAmount());
                    }
                    break;
                case IDM_PLAY_SEEKFWD:
                    if (mediaaccess::DaisyIsActive()) {
                        mediaaccess::DaisySeekRelative(GetCurrentSeekAmount());
                        break;
                    }
                    if (g_currentSeekIndex == 12) {
                        // Chapter seeking
                        if (!g_chapters.empty()) {
                            SeekToNextChapter();
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack && g_playlist.size() <= 1) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                                Seek(g_seekAmounts[i].value);
                                break;
                            }
                        }
                    } else if (g_seekAmounts[g_currentSeekIndex].isTrack) {
                        SeekTracks(static_cast<int>(g_seekAmounts[g_currentSeekIndex].value));
                    } else {
                        Seek(GetCurrentSeekAmount());
                    }
                    break;
                case IDM_SEEK_DECREASE:
                    CycleSeekAmount(-1);
                    break;
                case IDM_SEEK_INCREASE:
                    CycleSeekAmount(1);
                    break;
                case IDM_PLAY_VOLUP:
                    SetVolume(g_volume + g_volumeStep);
                    break;
                case IDM_PLAY_VOLDOWN:
                    SetVolume(g_volume - g_volumeStep);
                    break;
                case IDM_PLAY_MUTE:
                    ToggleMute();
                    break;
                case IDM_PLAY_ELAPSED:
                    SpeakElapsed();
                    break;
                case IDM_PLAY_REMAINING:
                    SpeakRemaining();
                    break;
                case IDM_PLAY_TOTAL:
                    SpeakTotal();
                    break;
                case IDM_PLAY_NOWPLAYING:
                    SpeakTagTitle();
                    break;
                case IDM_TOGGLE_WINDOW:
                    ToggleWindow(hwnd);
                    break;
                case IDM_TRAY_RESTORE:
                    RestoreFromTray(hwnd);
                    break;
                case IDM_TRAY_EXIT:
                    DestroyWindow(hwnd);
                    break;
                // Effect controls
                case IDM_EFFECT_PREV:
                    CycleEffect(-1);
                    break;
                case IDM_EFFECT_NEXT:
                    CycleEffect(1);
                    break;
                case IDM_EFFECT_UP:
                    AdjustCurrentEffect(1);
                    break;
                case IDM_EFFECT_DOWN:
                    AdjustCurrentEffect(-1);
                    break;
                case IDM_EFFECT_RESET:
                    ResetCurrentParam();
                    break;
                case IDM_EFFECT_MIN:
                    SetCurrentParamToMin();
                    break;
                case IDM_EFFECT_MAX:
                    SetCurrentParamToMax();
                    break;
                // Effect toggles
                case IDM_TOGGLE_VOLUME:
                    ToggleStreamEffect(0);
                    break;
                case IDM_TOGGLE_PITCH:
                    ToggleStreamEffect(1);
                    break;
                case IDM_TOGGLE_TEMPO:
                    ToggleStreamEffect(2);
                    break;
                case IDM_TOGGLE_RATE:
                    ToggleStreamEffect(3);
                    break;
                case IDM_TOGGLE_REVERB:
                    ToggleDSPEffect(DSPEffectType::Reverb);
                    break;
                case IDM_TOGGLE_ECHO:
                    ToggleDSPEffect(DSPEffectType::Echo);
                    break;
                case IDM_TOGGLE_EQ:
                    ToggleDSPEffect(DSPEffectType::EQ);
                    break;
                case IDM_TOGGLE_COMPRESSOR:
                    ToggleDSPEffect(DSPEffectType::Compressor);
                    break;
                case IDM_TOGGLE_STEREOWIDTH:
                    ToggleDSPEffect(DSPEffectType::StereoWidth);
                    break;
                case IDM_TOGGLE_CENTERCANCEL:
                    ToggleDSPEffect(DSPEffectType::CenterCancel);
                    break;
                case IDM_TOGGLE_CONVOLUTION:
                    ToggleDSPEffect(DSPEffectType::Convolution);
                    break;
                case IDM_TOGGLE_SPATIAL:
                    ToggleDSPEffect(DSPEffectType::SpatialAudio);
                    break;
                // Speak seek amount
                case IDM_SPEAK_SEEK:
                    SpeakSeekAmount();
                    break;
                // Tag reading (1-0 keys)
                case IDM_READ_TAG_TITLE:
                    SpeakTagTitle();
                    break;
                case IDM_READ_TAG_ARTIST:
                    SpeakTagArtist();
                    break;
                case IDM_READ_TAG_ALBUM:
                    SpeakTagAlbum();
                    break;
                case IDM_READ_TAG_YEAR:
                    SpeakTagYear();
                    break;
                case IDM_READ_TAG_TRACK:
                    SpeakTagTrack();
                    break;
                case IDM_READ_TAG_GENRE:
                    SpeakTagGenre();
                    break;
                case IDM_READ_TAG_COMMENT:
                    SpeakTagComment();
                    break;
                case IDM_READ_TAG_BITRATE:
                    SpeakTagBitrate();
                    break;
                case IDM_READ_TAG_DURATION:
                    SpeakTagDuration();
                    break;
                case IDM_READ_TAG_FILENAME:
                    SpeakTagFilename();
                    break;
                // View tags in dialog (Shift+1-0)
                case IDM_VIEW_TAG_TITLE:
                    ShowTagDialog(L"Title", GetTagTitle());
                    break;
                case IDM_VIEW_TAG_ARTIST:
                    ShowTagDialog(L"Artist", GetTagArtist());
                    break;
                case IDM_VIEW_TAG_ALBUM:
                    ShowTagDialog(L"Album", GetTagAlbum());
                    break;
                case IDM_VIEW_TAG_YEAR:
                    ShowTagDialog(L"Year", GetTagYear());
                    break;
                case IDM_VIEW_TAG_TRACK:
                    ShowTagDialog(L"Track", GetTagTrack());
                    break;
                case IDM_VIEW_TAG_GENRE:
                    ShowTagDialog(L"Genre", GetTagGenre());
                    break;
                case IDM_VIEW_TAG_COMMENT:
                    ShowTagDialog(L"Comment", GetTagComment());
                    break;
                case IDM_VIEW_TAG_BITRATE:
                    ShowTagDialog(L"Bitrate", GetTagBitrate());
                    break;
                case IDM_VIEW_TAG_DURATION:
                    ShowTagDialog(L"Duration", GetTagDuration());
                    break;
                case IDM_VIEW_TAG_FILENAME:
                    ShowTagDialog(L"Filename", GetTagFilename());
                    break;
                case IDM_RECORD_TOGGLE:
                    ToggleRecording();
                    break;
                case IDM_SHOW_AUDIO_DEVICES:
                    ShowAudioDeviceMenu(hwnd);
                    break;
                // ===== Video commands =====
                case IDM_VIDEO_FULLSCREEN:
                    if (g_isVideoPlaying) MPVToggleFullscreen(hwnd);
                    break;
                case IDM_VIDEO_SUB_CYCLE:
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleSubtitles();
                    break;
                case IDM_VIDEO_SUB_LOAD:
                    OpenSubtitleFile(hwnd);
                    break;
                case IDM_VIDEO_SUB_OFF:
                    if (g_activeEngine == PlaybackEngine::MPV) MPVSetSubtitleTrack(0);
                    break;
                case IDM_VIDEO_AUDIO_CYCLE:
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleAudioTracks();
                    break;
                case IDM_VIDEO_ASPECT:
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleAspectRatio();
                    break;
                case IDM_VIDEO_SCREENSHOT:
                    if (g_activeEngine == PlaybackEngine::MPV) MPVTakeScreenshot();
                    break;
                default:
                    // Dynamic-range command IDs (audio devices, recent files,
                    // effect presets). Helper returns true if handled.
                    DispatchDynamicRangeCommand(LOWORD(wParam));
                    break;
            }
            return 0;

        case WM_DESTROY:
            // v1.63 — mark shutdown so WM_COPYDATA dwData=4 (CLI deliveries)
            // arriving after this point are dropped before they touch BASS.
            g_isShuttingDown = true;
            // -----------------------------------------------------------
            // Kill audio IMMEDIATELY before doing anything else.
            // Otherwise the BASS device keeps producing sound for the
            // multi-second duration of the save/cleanup chain below
            // (settings flush, YouTube temp wipe, FreeMPV's 3-second
            // thread-join wait, etc.) — the user hears playback continue
            // after the window has already disappeared.
            //
            // BASS_Pause() suspends the output device synchronously; the
            // individual streams get freed properly later inside
            // FreeBass(). This pair guarantees silence on the first line
            // of shutdown no matter how slow the rest of the chain is.
            // -----------------------------------------------------------
            mediaaccess::DaisyClose();  // Save book position + free book stream first
            mediaaccess::BookTextWindowDestroy();
            mediaaccess::TtsShutdown();
            if (g_fxStream) BASS_ChannelStop(g_fxStream);
            if (g_stream)   BASS_ChannelStop(g_stream);
            BASS_Pause();
            KillTimer(hwnd, IDT_UPDATE_TITLE);
            KillTimer(hwnd, IDT_SCHEDULER);
            KillTimer(hwnd, IDT_SCHED_DURATION);
            mediaaccess::SleepShutdown();
            RemoveTrayIcon();
            UnregisterGlobalHotkeys();
            StopRecording();  // Stop recording on exit
            if (g_fxStream && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                SaveFilePosition(g_playlist[g_currentTrack]);
            }
            SavePlaybackState();
            SaveSettings();
            YouTubeCleanup();  // Clean up temp files
            CloseDatabase();
            FreeMPV();  // Free video engine before BASS
            FreeLogger();
            FreeBass();
            // After BASS is released, any cache file that was open as the
            // playing stream is now closed and safe to delete.
            if (g_clearYtCacheOnExit) ClearYouTubeCache();
            FreeSpeech();
            PostQuitMessage(0);
            return 0;
    }

    // v1.63 — drain the CLI command queue accumulated when this is the
    // first instance and the launcher saw control switches on argv.
    if (msg == WM_APP_CLI) {
        DrainPendingCliCommands(hwnd);
        return 0;
    }

    // Download completion from DownloadManager (posted to main window)
    if (msg == WM_DOWNLOAD_COMPLETE) {
        int id = static_cast<int>(wParam);
        bool success = (lParam != 0);
        DownloadManager::Instance().ProcessCompletion(id, success);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Entry point
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    // Restrict DLL search order to system32 + app dir + user-added dirs (prevents DLL hijacking
    // while still allowing SetDllDirectoryW below to add the lib subfolder)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS);

    InitLogger();

    // Set DLL search path to lib subfolder (must be before any DLL loads)
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(exePath, MAX_PATH, L"lib");
        SetDllDirectoryW(exePath);
    }

    // Resolve the real config path (AppData in installed mode, exe-dir in
    // portable mode). Without this, the multi-instance flag read below
    // would always come from <exe-dir>\MediaAccess.ini even when Options
    // saved it to %APPDATA%\MediaAccess\MediaAccess.ini — i.e. the
    // checkbox would never appear to take effect after install.
    InitConfigPath();

    // Check if multiple instances are allowed
    bool allowMultiple = GetPrivateProfileIntW(L"Playback", L"AllowMultipleInstances", 0, g_configPath.c_str()) != 0;

    // Walk argv once: classify each token as file vs CLI switch (v1.63) vs ignored.
    // A token is a switch iff it starts with "/" AND IsKnownCliSwitch returns true
    // AND it doesn't also resolve to a real file (paranoid edge case).
    bool hasFileArgs = false;
    std::vector<CliCommand> cliCmds;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            bool isFile = (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES);
            if (isFile) { hasFileArgs = true; continue; }
            if (IsKnownCliSwitch(argv[i])) {
                CliCommand cmd;
                if (ParseCliSwitch(argv[i], cmd)) {
                    cliCmds.push_back(std::move(cmd));
                }
            }
            // Otherwise silently ignored — bogus arg or path that doesn't exist.
        }
    }
    bool hasCliSwitches = !cliCmds.empty();

    // Single instance logic:
    // - If multiple instances NOT allowed: always use single instance
    // - If multiple instances allowed AND has file/cli args: send to existing instance
    // - If multiple instances allowed AND no args: start new instance
    bool useSingleInstance = !allowMultiple || hasFileArgs || hasCliSwitches;

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS && useSingleInstance) {
        HWND existingWnd = FindWindowW(WINDOW_CLASS, nullptr);
        if (existingWnd) {
            if (argv && hasFileArgs) {
                bool firstFile = true;
                for (int i = 1; i < argc; i++) {
                    if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                        COPYDATASTRUCT cds;
                        cds.dwData = firstFile ? 1 : 2;
                        cds.cbData = static_cast<DWORD>((wcslen(argv[i]) + 1) * sizeof(wchar_t));
                        cds.lpData = argv[i];
                        SendMessageW(existingWnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
                        firstFile = false;
                    }
                }
            } else if (!hasCliSwitches) {
                // No file args AND no CLI switches: user launched MediaAccess
                // again (desktop icon, Start menu, Win+R, etc.) while it's
                // already running — possibly hidden in the system tray. Ask
                // the existing instance to restore and come to the foreground.
                COPYDATASTRUCT cds;
                cds.dwData = 3;       // "activate" sentinel
                cds.cbData = 0;
                cds.lpData = nullptr;
                SendMessageW(existingWnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
                // Also try to allow SetForegroundWindow on the other process
                DWORD existingPid = 0;
                GetWindowThreadProcessId(existingWnd, &existingPid);
                if (existingPid) AllowSetForegroundWindow(existingPid);
            }

            // v1.63 — relay CLI switches via WM_COPYDATA dwData=4. Use
            // SendMessageTimeoutW with SMTO_ABORTIFHUNG so a modal dialog in
            // the existing instance doesn't freeze every CLI invocation.
            // Also allow the existing instance to take foreground when /show
            // is among the switches.
            if (hasCliSwitches) {
                DWORD existingPid = 0;
                GetWindowThreadProcessId(existingWnd, &existingPid);
                if (existingPid) AllowSetForegroundWindow(existingPid);
                for (const auto& cmd : cliCmds) {
                    std::wstring payload;
                    EncodeCliPayload(cmd, payload);
                    if (payload.empty()) continue;
                    COPYDATASTRUCT cds;
                    cds.dwData = 4;
                    cds.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
                    cds.lpData = const_cast<wchar_t*>(payload.c_str());
                    DWORD_PTR result = 0;
                    SendMessageTimeoutW(existingWnd, WM_COPYDATA, 0,
                                        reinterpret_cast<LPARAM>(&cds),
                                        SMTO_ABORTIFHUNG, 5000, &result);
                }
            }
            if (argv) LocalFree(argv);
            CloseHandle(hMutex);
            return 0;
        }
    }
    if (argv) LocalFree(argv);

    // v1.63 — no other instance, but CLI commands are present. Stash them
    // for DrainPendingCliCommands which fires after WM_CREATE finishes
    // initial setup. Special case: if the ONLY commands are quit/hide,
    // there's nothing to act on yet — exit 0 silently rather than spawn
    // an instance that immediately dies.
    if (hasCliSwitches && !hasFileArgs) {
        bool anyMeaningful = false;
        for (const auto& c : cliCmds) {
            if (c.verb != CliVerb::Quit && c.verb != CliVerb::Hide) {
                anyMeaningful = true; break;
            }
        }
        if (!anyMeaningful) {
            CloseHandle(hMutex);
            return 0;
        }
    }
    if (hasCliSwitches) {
        g_pendingCliCommands = std::move(cliCmds);
    }

    // Register translations before LoadSettings: LoadSettings reads the saved
    // language and immediately calls SetLanguage() so any UI built afterwards
    // uses the correct language.
    InitTranslations();
    LoadSettings();
    // Initialize COM for SAPI (TTS) and any other COM-using helpers. Tolerate
    // RPC_E_CHANGED_MODE if some other component already initialized it with
    // a different threading model.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mediaaccess::TtsInit();
    // Load the active keymap (and ship USA/FR-CA/FR-FR defaults if missing).
    // Must happen AFTER LoadSettings (which reads the language preference) so
    // the keymap dispatcher sees the right active keymap from the very first
    // WM_KEYDOWN, and the bilingual action names display in the right tongue.
    mediaaccess::LoadActiveKeyMapAtStartup();
    if (g_registerFileTypes) {
        RegisterAllFileTypes();
    }
    LoadHotkeys();  // Legacy [Hotkeys] INI. Migrated to the keymap below.
    // One-shot migration: pre-v1.41 global hotkeys are injected into the
    // active keymap's Global category, the [Hotkeys] section is erased,
    // and the keymap file is rewritten. Subsequent launches find g_hotkeys
    // empty after LoadHotkeys() and this is a no-op.
    mediaaccess::MigrateLegacyHotkeysIfPresent();
    mediaaccess::SyncGlobalHotkeysFromKeymap();
    YouTubeCleanup();  // Clean up any leftover temp files from previous sessions
    // Enforce user-configured cache size cap (0 = unlimited, no-op).
    // Runs at startup so the user notices the cache shrinking when they
    // launch, not at unpredictable mid-session moments.
    if (g_ytCacheLimitMB > 0) EnforceYouTubeCacheLimit(g_ytCacheLimitMB);
    ParseCommandLine();

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDM_MAIN_MENU);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, T("Failed to register window class."), APP_NAME, MB_ICONERROR);
        return 1;
    }

    HACCEL hAccel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDA_ACCEL));

    HWND hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS,
        APP_NAME,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        DEFAULT_WINDOW_WIDTH, DEFAULT_WINDOW_HEIGHT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBoxW(nullptr, T("Failed to create window."), APP_NAME, MB_ICONERROR);
        return 1;
    }

    // Localize the main menu now that it has been attached to the window
    LocalizeMenu(GetMenu(hwnd));
    // Rewrite each menu item's accelerator hint to match the active keymap.
    // Must come after LocalizeMenu (which may rewrite the text portion).
    mediaaccess::RefreshMenuAcceleratorHints();
    DrawMenuBar(hwnd);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    if (g_playlist.empty()) {
        LoadPlaybackState();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Handle modeless YouTube dialog
        HWND ytDlg = GetYouTubeDialog();
        if (ytDlg && IsDialogMessageW(ytDlg, &msg)) {
            continue;  // Message was handled by the dialog
        }

        // Only process accelerators if YouTube dialog doesn't have focus
        bool ytHasFocus = ytDlg && (GetForegroundWindow() == ytDlg || IsChild(ytDlg, GetFocus()));

        // Skip accelerators when keyboard help is on, so the WM_KEYDOWN
        // describe-the-key path receives Space, arrows, and every other
        // accelerator-bound key. F12 still needs to pass through so the
        // user can toggle the mode back off; we let it reach the
        // accelerator translator unconditionally.
        bool kbHelpBlock = g_keyboardHelpMode &&
                           msg.hwnd == hwnd &&
                           (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) &&
                           msg.wParam != VK_F12;

        if (ytHasFocus || kbHelpBlock || !TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}
