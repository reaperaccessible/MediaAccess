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
#include <utility>  // for std::pair

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

            return 0;
        }

        case WM_SIZE:
            if (g_statusBar) {
                SendMessageW(g_statusBar, WM_SIZE, 0, 0);
            }
            // Resize video child window to fill client area above status bar
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
                Speak(DescribeKey(wParam, lParam));
                return 0;
            }
            if (wParam == VK_F11 && g_isVideoPlaying) {
                PostMessageW(hwnd, WM_COMMAND, 1200, 0);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_isFullscreen) {
                PostMessageW(hwnd, WM_COMMAND, 1200, 0);
                return 0;
            }
            // -----------------------------------------------------------
            // Physical scan-code shortcuts (layout-independent)
            //
            // These keys are dispatched by their PHYSICAL POSITION on the
            // keyboard rather than the character they happen to produce.
            // That way the Winamp-style "ZXCVB" transport row, the
            // movement-unit slider (`,` `.`) and the effect slider (`[`
            // `]`) work identically on QWERTY, AZERTY, QWERTZ, Bépo,
            // Dvorak, Cyrillic, Greek… without any user-visible
            // configuration. See user manual for the explanation.
            //
            // Scan codes are the Set 1 IBM PC values (bits 16-23 of the
            // WM_KEYDOWN LPARAM). Reference:
            //   Q W E R T Y U I O P [ ]   = 0x10..0x1B
            //   A S D F G H J K L ; '     = 0x1E..0x28
            //   Z X C V B N M , . /       = 0x2C..0x35
            //
            // Modifier-bearing shortcuts (Ctrl+letter, Ctrl+Shift+letter)
            // remain VK-based via the accelerator table because users
            // think of them as letter mnemonics, not key positions.
            // -----------------------------------------------------------
            {
                BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                BOOL alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
                // Shift is allowed only for the IDM_VIEW_TAG_* entries
                // which are number-row based and remain VK-driven; the
                // letter / OEM shortcuts below all require no modifier.
                BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
                if (!ctrl && !alt && !shift) {
                    UINT scan = (UINT)((lParam >> 16) & 0xFF);
                    int cmd = 0;
                    switch (scan) {
                        // Winamp transport row (bottom row of letters)
                        case 0x2C: cmd = IDM_PLAY_PREV;          break; // physical Z
                        case 0x2D: cmd = IDM_PLAY_PLAY;          break; // physical X
                        case 0x2E: cmd = IDM_PLAY_PAUSE;         break; // physical C
                        case 0x2F: cmd = IDM_PLAY_STOP;          break; // physical V
                        case 0x30: cmd = IDM_PLAY_NEXT;          break; // physical B
                        case 0x32: cmd = IDM_BOOKMARK_ADD;       break; // physical M
                        case 0x33: cmd = IDM_SEEK_DECREASE;      break; // physical ,
                        case 0x34: cmd = IDM_SEEK_INCREASE;      break; // physical .
                        // Other modifier-free letters
                        case 0x12: cmd = IDM_PLAY_REPEAT_TOGGLE; break; // physical E
                        case 0x13: cmd = IDM_RECORD_TOGGLE;      break; // physical R
                        case 0x16: cmd = IDM_PLAY_MUTE;          break; // physical U
                        case 0x19: cmd = IDM_EFFECT_PRESETS;     break; // physical P
                        case 0x1A: cmd = IDM_EFFECT_PREV;        break; // physical [
                        case 0x1B: cmd = IDM_EFFECT_NEXT;        break; // physical ]
                        case 0x1E: cmd = IDM_SHOW_AUDIO_DEVICES; break; // physical A
                        case 0x23: cmd = IDM_PLAY_SHUFFLE;       break; // physical H
                        case 0x24: cmd = IDM_PLAY_JUMPTOTIME;    break; // physical J
                    }
                    if (cmd != 0) {
                        PostMessageW(hwnd, WM_COMMAND, cmd, 0);
                        return 0;
                    }
                }
            }
            break;

        case WM_LBUTTONDBLCLK:
            if (g_isVideoPlaying) {
                PostMessageW(hwnd, WM_COMMAND, 1200, 0);
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (g_isFullscreen) {
                while (ShowCursor(TRUE) < 0) {}
                SetTimer(hwnd, 410, 3000, nullptr);
            }
            break;

        case WM_RBUTTONUP:
            if (g_isVideoPlaying) {
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, 1200, T("Fullscreen\tF11"));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, 1201, T("Cycle Subtitles"));
                AppendMenuW(hMenu, MF_STRING, 1202, T("Load Subtitle File..."));
                AppendMenuW(hMenu, MF_STRING, 1240, T("Cycle Audio Track"));
                AppendMenuW(hMenu, MF_STRING, 1270, T("Cycle Aspect Ratio"));
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, 1280, T("Take Screenshot"));
                POINT pt;
                GetCursorPos(&pt);
                TrackPopupMenu(hMenu, TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                return 0;
            }
            break;

        case WM_TIMER:
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
            } else if (wParam == 410) { // Cursor auto-hide timer for fullscreen
                KillTimer(hwnd, 410);
                if (g_isFullscreen) {
                    POINT pt;
                    GetCursorPos(&pt);
                    RECT rc;
                    GetWindowRect(hwnd, &rc);
                    if (PtInRect(&rc, pt)) while (ShowCursor(FALSE) >= 0) {}
                }
            }
            return 0;

        case WM_SPEAK:
            DoSpeak();
            return 0;

        case WM_META_CHANGED:
            AnnounceStreamMetadata();
            UpdateWindowTitle();
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

        case WM_HOTKEY: {
            int hotkeyId = static_cast<int>(wParam);

            // Handle media keys (registered with special IDs)
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

            // Handle user-defined hotkeys
            for (const auto& hk : g_hotkeys) {
                if (hk.id == hotkeyId) {
                    PostMessage(hwnd, WM_COMMAND, g_hotkeyActions[hk.actionIdx].commandId, 0);
                    break;
                }
            }
            return 0;
        }

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONDBLCLK) {
                RestoreFromTray(hwnd);
            } else if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hwnd);
            }
            return 0;

        case WM_COPYDATA: {
            COPYDATASTRUCT* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
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
            // Update recent files menu when File menu is opened
            if (HIWORD(lParam) == FALSE) {  // Not a system menu
                HMENU hMenu = GetMenu(hwnd);
                if (hMenu) {
                    UpdateRecentFilesMenu(hMenu);
                }
            }
            break;

        case WM_COMMAND:
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
                case IDM_FILE_PASTE: { extern std::vector<std::wstring> GetFilesFromClipboard();
                    // Ctrl+V on the main window: paste media files/URLs from the
                    // clipboard. Replaces the current playlist (matching the
                    // "Open file" semantics — paste behaves like a fresh open).
                    std::vector<std::wstring> files;
                    try {
                        files = GetFilesFromClipboard();
                    } catch (...) {}
                    if (files.empty()) {
                        Speak(Ts("No media in clipboard"));
                    } else {
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
                    break;
                }
                case IDM_TOOLS_OPTIONS:
                    ShowOptionsDialog();
                    break;
                case IDM_HELP_PLUGINS:
                    MessageBoxW(hwnd, GetLoadedPluginsInfo().c_str(), T("Loaded Plugins"), MB_OK | MB_ICONINFORMATION);
                    break;
                case IDM_HELP_TEST_YOUTUBE:
                    ShowTestYouTubePlayback();
                    break;
                case IDM_HELP_CLEAR_YT_CACHE: {
                    unsigned long long bytes = GetYouTubeCacheSize();
                    double mb = bytes / (1024.0 * 1024.0);
                    wchar_t prompt[256];
                    swprintf(prompt, 256, L"%s\n\n%.1f MB", T("Clear all cached YouTube audio?"), mb);
                    if (MessageBoxW(hwnd, prompt, T("Clear YouTube cache"),
                                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        int n = ClearYouTubeCache();
                        wchar_t done[128];
                        swprintf(done, 128, T("Removed %d cached files."), n);
                        MessageBoxW(hwnd, done, T("Clear YouTube cache"), MB_OK | MB_ICONINFORMATION);
                    }
                    break;
                }
                case IDM_HELP_UPDATES:
                    ShowCheckForUpdatesDialog(hwnd, false);
                    break;
                case IDM_HELP_AUDIT_LAYOUT:
                    AuditOptionsLayout();
                    break;
                case IDM_KEYBOARD_HELP_TOGGLE:
                    g_keyboardHelpMode = !g_keyboardHelpMode;
                    Speak(Ts(g_keyboardHelpMode ? "Keyboard help on"
                                                : "Keyboard help off"));
                    break;
                case IDM_HELP_MANUAL: {
                    // Open the bilingual HTML manual in the user's default browser.
                    // We pick the file matching the current app language; user can
                    // switch languages from the link inside the manual.
                    wchar_t manualPath[MAX_PATH];
                    GetModuleFileNameW(nullptr, manualPath, MAX_PATH);
                    wchar_t* slash = wcsrchr(manualPath, L'\\');
                    if (slash) {
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
                    break;
                }
                case IDM_HELP_README: {
                    wchar_t readmePath[MAX_PATH];
                    GetModuleFileNameW(nullptr, readmePath, MAX_PATH);
                    wchar_t* slash = wcsrchr(readmePath, L'\\');
                    if (slash) {
                        *(slash + 1) = L'\0';
                        wcscat_s(readmePath, MAX_PATH, L"docs\\readme.txt");
                        HINSTANCE r = ShellExecuteW(hwnd, L"open", readmePath, nullptr, nullptr, SW_SHOWNORMAL);
                        if ((INT_PTR)r <= 32) {
                            MessageBoxW(hwnd, T("Could not open readme.txt. Make sure the docs folder is present alongside MediaAccess.exe."), T("Readme"), MB_OK | MB_ICONWARNING);
                        }
                    }
                    break;
                }
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
                    PlayPause();
                    break;
                case IDM_PLAY_PLAY:
                    Play();
                    break;
                case IDM_PLAY_PAUSE:
                    Pause();
                    break;
                case IDM_PLAY_STOP:
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
                case IDM_PLAY_JUMPTOTIME:
                    ShowJumpToTimeDialog();
                    break;
                case IDM_PLAY_SEEKBACK:
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
                case 1200: // IDM_VIDEO_FULLSCREEN
                    if (g_isVideoPlaying) MPVToggleFullscreen(hwnd);
                    break;
                case 1201: // IDM_VIDEO_SUB_CYCLE
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleSubtitles();
                    break;
                case 1202: { // IDM_VIDEO_SUB_LOAD
                    if (g_activeEngine != PlaybackEngine::MPV) break;
                    wchar_t fname[MAX_PATH] = L"";
                    OPENFILENAMEW ofn = { sizeof(ofn) };
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"Subtitle Files\0*.srt;*.ass;*.ssa;*.sub;*.vtt\0All Files\0*.*\0";
                    ofn.lpstrFile = fname;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) MPVLoadExternalSubtitle(fname);
                    break;
                }
                case 1203: // IDM_VIDEO_SUB_OFF
                    if (g_activeEngine == PlaybackEngine::MPV) MPVSetSubtitleTrack(0);
                    break;
                case 1240: // IDM_VIDEO_AUDIO_CYCLE
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleAudioTracks();
                    break;
                case 1270: // IDM_VIDEO_ASPECT
                    if (g_activeEngine == PlaybackEngine::MPV) MPVCycleAspectRatio();
                    break;
                case 1280: // IDM_VIDEO_SCREENSHOT
                    if (g_activeEngine == PlaybackEngine::MPV) MPVTakeScreenshot();
                    break;
                default:
                    // Handle audio device selection (dynamic menu IDs)
                    {
                        WORD cmdId = LOWORD(wParam);
                        if (cmdId >= IDM_AUDIO_DEVICE_BASE && cmdId < IDM_AUDIO_DEVICE_BASE + 100) {
                            int deviceIndex = cmdId - IDM_AUDIO_DEVICE_BASE;
                            SelectAudioDevice(deviceIndex);
                        }
                        // Handle recent file selection
                        else if (cmdId >= IDM_FILE_RECENT_BASE && cmdId < IDM_FILE_RECENT_BASE + MAX_RECENT_FILES) {
                            size_t idx = cmdId - IDM_FILE_RECENT_BASE;
                            if (idx < g_recentFiles.size()) {
                                g_playlist.clear();
                                g_playlist.push_back(g_recentFiles[idx]);
                                g_currentTrack = -1;
                                PlayTrack(0);
                            }
                        }
                        // Handle effect preset apply
                        else if (cmdId >= IDM_PRESET_BASE && cmdId < IDM_PRESET_BASE + 100) {
                            auto names = GetEffectPresetNames();
                            size_t idx = cmdId - IDM_PRESET_BASE;
                            if (idx < names.size() && LoadEffectPreset(names[idx])) {
                                std::wstring msg = L"Loaded preset " + names[idx];
                                SpeakW(msg);
                            }
                        }
                        // Handle effect preset delete
                        else if (cmdId >= IDM_PRESET_DELETE_BASE && cmdId < IDM_PRESET_DELETE_BASE + 100) {
                            auto names = GetEffectPresetNames();
                            size_t idx = cmdId - IDM_PRESET_DELETE_BASE;
                            if (idx < names.size()) {
                                std::wstring n = names[idx];
                                if (DeleteEffectPreset(n)) {
                                    std::wstring msg = L"Deleted preset " + n;
                                    SpeakW(msg);
                                }
                            }
                        }
                        // Handle save new preset
                        else if (cmdId == IDM_PRESET_SAVE_NEW) {
                            ShowSaveEffectPresetDialog();
                        }
                    }
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, IDT_UPDATE_TITLE);
            KillTimer(hwnd, IDT_SCHEDULER);
            KillTimer(hwnd, IDT_SCHED_DURATION);
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

    // Build config path early to read multi-instance setting
    wchar_t configPath[MAX_PATH];
    GetModuleFileNameW(nullptr, configPath, MAX_PATH);
    wchar_t* configSlash = wcsrchr(configPath, L'\\');
    if (configSlash) {
        *(configSlash + 1) = L'\0';
        wcscat_s(configPath, MAX_PATH, L"MediaAccess.ini");
    }

    // Check if multiple instances are allowed
    bool allowMultiple = GetPrivateProfileIntW(L"Playback", L"AllowMultipleInstances", 0, configPath) != 0;

    // Check if we have file arguments
    bool hasFileArgs = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            if (GetFileAttributesW(argv[i]) != INVALID_FILE_ATTRIBUTES) {
                hasFileArgs = true;
                break;
            }
        }
    }

    // Single instance logic:
    // - If multiple instances NOT allowed: always use single instance
    // - If multiple instances allowed AND has file args: send to existing instance
    // - If multiple instances allowed AND no file args: start new instance
    bool useSingleInstance = !allowMultiple || hasFileArgs;

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
            } else {
                // No file args: user launched MediaAccess again (desktop
                // icon, Start menu, Win+R, etc.) while it's already running
                // — possibly hidden in the system tray. Ask the existing
                // instance to restore and come to the foreground.
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
            if (argv) LocalFree(argv);
            CloseHandle(hMutex);
            return 0;
        }
    }
    if (argv) LocalFree(argv);

    // Register translations before LoadSettings: LoadSettings reads the saved
    // language and immediately calls SetLanguage() so any UI built afterwards
    // uses the correct language.
    InitTranslations();
    LoadSettings();
    if (g_registerFileTypes) {
        RegisterAllFileTypes();
    }
    LoadHotkeys();
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
        500, 150,
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
