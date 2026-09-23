#include "ui_internal.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/youtube.h"   // v2.69 — YouTubeGetDownloadRoot for the playlists folder

// Forward declaration for GetFilesFromClipboard (defined in ui.cpp)
extern std::vector<std::wstring> GetFilesFromClipboard();

// =============================================================================
// v2.69 (Nicolas, studio radio) — the manager shows a list that is not always
// the one being played.
//
// Picking a saved playlist in the combo loads it HERE ONLY: playback must never
// be interrupted by browsing, because on air that would cut the track. The shown
// list only becomes the playing list when the user actually starts a track
// (Enter / double-click). ShownList() is what every edit in this window works on.
// =============================================================================
static std::vector<std::wstring> g_shownList;   // used only while !g_shownIsLive
// v2.69 — file the shown list came from (picker or last Save As). Empty when
// the window shows the playing list and no file backs it: Ctrl+S then asks
// for a name instead of overwriting something the user never chose.
static std::wstring g_shownSourcePath;
static bool g_shownIsLive = true;               // true: the window mirrors g_playlist
static std::vector<std::wstring>& ShownList() { return g_shownIsLive ? g_playlist : g_shownList; }

// Full paths behind the combo entries, same order (the combo shows file names).
static std::vector<std::wstring> g_comboPaths;
static WNDPROC g_comboOrigProc = nullptr;

// Registry of saved playlists offered by the combo. Most recent first, no
// duplicates (case-insensitive), capped like the INI side.
static void RegistryAdd(const std::wstring& path) {
    for (auto it = g_playlistRegistry.begin(); it != g_playlistRegistry.end(); ++it) {
        if (_wcsicmp(it->c_str(), path.c_str()) == 0) { g_playlistRegistry.erase(it); break; }
    }
    g_playlistRegistry.insert(g_playlistRegistry.begin(), path);
    if (g_playlistRegistry.size() > 50) g_playlistRegistry.resize(50);
}

static void RegistryRemove(const std::wstring& path) {
    for (auto it = g_playlistRegistry.begin(); it != g_playlistRegistry.end(); ++it) {
        if (_wcsicmp(it->c_str(), path.c_str()) == 0) { g_playlistRegistry.erase(it); return; }
    }
}

// Fill the combo from the registry, skipping entries whose file is gone — a
// playlist may live anywhere, and a missing one is simply not offered. No
// announcement: the user owns their files (explicit decision).
static void FillPlaylistCombo(HWND hCombo) {
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    g_comboPaths.clear();
    for (const auto& path : g_playlistRegistry) {
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name = path;
        size_t pos = name.find_last_of(L"\\/");
        if (pos != std::wstring::npos) name = name.substr(pos + 1);
        // v2.69 — the picker reads as a NAME, not as a file: drop the extension.
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
        SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        g_comboPaths.push_back(path);
    }
}

// Helper to rebuild playlist listbox
static void RebuildPlaylistList(HWND hList, int selectIndex = -1) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    const std::vector<std::wstring>& shown = ShownList();
    for (size_t i = 0; i < shown.size(); i++) {
        std::wstring filename = shown[i];
        size_t pos = filename.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            filename = filename.substr(pos + 1);
        }
        wchar_t buf[512];
        swprintf(buf, 512, L"%d. %s", (int)(i + 1), filename.c_str());
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (selectIndex >= 0 && selectIndex < (int)shown.size()) {
        // v2.69 — LBS_EXTENDEDSEL: LB_SETCURSEL is not supported here and was
        // failing silently, so the caret never came back to selectIndex after a
        // delete / paste / Alt+Up-Down. Enter and the screen reader both follow
        // the caret, so it has to move.
        SendMessageW(hList, LB_SETSEL, TRUE, selectIndex);
        SendMessageW(hList, LB_SETCARETINDEX, selectIndex, FALSE);
    }
}

// Subclassed listbox data for playlist manager
static WNDPROC g_playlistOrigProc = nullptr;
HWND g_playlistDlg = nullptr;   // v2.69 — read by main.cpp (Explorer open must
                                // not steal focus from an open playlist)

// Helper to get selected indices from multi-select listbox
static std::vector<int> GetSelectedIndices(HWND hwnd) {
    std::vector<int> indices;
    int count = (int)SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
    if (count > 0) {
        indices.resize(count);
        SendMessageW(hwnd, LB_GETSELITEMS, count, reinterpret_cast<LPARAM>(indices.data()));
    }
    return indices;
}

// v2.69 (Nicolas, studio radio) — Space starts/stops playback from ANYWHERE in
// this window: on air the hand is on this list, not on the main window. It is a
// setting because it costs two standard behaviours: Space no longer toggles the
// selection in the track list, nor presses the focused button (Enter and the
// Alt+letter access keys still do).
// Defined further down, used by the control subclasses just below.
static void SavePlaylistQuick(HWND hDlg, HWND hCombo);

// v2.69 — Ctrl+S saves from anywhere in the window (list, picker, buttons).
static bool SaveShortcut(UINT msg, WPARAM wParam) {
    return msg == WM_KEYDOWN && wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000) &&
           !(GetKeyState(VK_MENU) & 0x8000);
}

static bool SpaceIsPlayPause(UINT msg, WPARAM wParam) {
    if (!g_playlistSpacePlayPause) return false;
    if (msg != WM_KEYDOWN || wParam != VK_SPACE) return false;
    if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
        (GetKeyState(VK_SHIFT) & 0x8000)) {
        return false;   // leave modified Space alone
    }
    PlayPause();
    return true;
}

// Buttons need the same treatment, so they get a small subclass of their own.
static WNDPROC g_btnOrigProc = nullptr;
static LRESULT CALLBACK PlaylistButtonProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (SaveShortcut(msg, wParam)) {   // v2.69
        HWND dlg = GetParent(hwnd);
        SavePlaylistQuick(dlg, GetDlgItem(dlg, IDC_PLAYLIST_COMBO));
        return 0;
    }
    if (SpaceIsPlayPause(msg, wParam)) return 0;
    // Swallow the key-up too, otherwise the button still fires on release.
    if (g_playlistSpacePlayPause && msg == WM_KEYUP && wParam == VK_SPACE) return 0;
    return CallWindowProcW(g_btnOrigProc, hwnd, msg, wParam, lParam);
}

// v2.69 — house rule: a navigable list must never hit a silent edge. Up on the
// first line announces "First," + the line, Down on the last announces "Last," +
// the line — also when the list holds a single item. Without this the screen
// reader says nothing at all, because the selection did not move.
static bool AnnounceListEdge(HWND hList, WPARAM wParam) {
    if (wParam != VK_UP && wParam != VK_DOWN) return false;
    int count = (int)SendMessageW(hList, LB_GETCOUNT, 0, 0);
    if (count <= 0) return false;
    int caret = (int)SendMessageW(hList, LB_GETCARETINDEX, 0, 0);
    bool first = (wParam == VK_UP   && caret == 0);
    bool last  = (wParam == VK_DOWN && caret == count - 1);
    if (!first && !last) return false;
    int len = (int)SendMessageW(hList, LB_GETTEXTLEN, caret, 0);
    std::wstring text(len > 0 ? len + 1 : 1, L'\0');
    if (len > 0) SendMessageW(hList, LB_GETTEXT, caret, reinterpret_cast<LPARAM>(&text[0]));
    text.resize(wcslen(text.c_str()));
    SpeakW(std::wstring(first ? T("First,") : T("Last,")) + L" " + text);
    return true;
}

// Same rule for the saved-playlist picker (a combo box, so different messages).
static bool AnnounceComboEdge(HWND hCombo, WPARAM wParam) {
    if (wParam != VK_UP && wParam != VK_DOWN) return false;
    int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
    if (count <= 0) return false;
    int cur = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    bool first = (wParam == VK_UP   && cur == 0);
    bool last  = (wParam == VK_DOWN && cur == count - 1);
    if (!first && !last) return false;
    int len = (int)SendMessageW(hCombo, CB_GETLBTEXTLEN, cur, 0);
    std::wstring text(len > 0 ? len + 1 : 1, L'\0');
    if (len > 0) SendMessageW(hCombo, CB_GETLBTEXT, cur, reinterpret_cast<LPARAM>(&text[0]));
    text.resize(wcslen(text.c_str()));
    SpeakW(std::wstring(first ? T("First,") : T("Last,")) + L" " + text);
    return true;
}

// v2.69 — write the shown list to `path`. Single place that knows the format.
static bool WritePlaylistFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"w, ccs=UTF-8");
    if (!f) return false;
    fwprintf(f, L"#EXTM3U\n");
    for (const auto& track : ShownList()) fwprintf(f, L"%s\n", track.c_str());
    fclose(f);
    return true;
}

// "Save..." — always asks for a name, opening in the playlists folder.
static void SavePlaylistAs(HWND hDlg, HWND hCombo) {
    if (ShownList().empty()) {
        MessageBoxW(hDlg, T("Playlist is empty."), T("Save Playlist"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t filePath[MAX_PATH] = L"playlist.m3u";
    std::wstring initialDir = GetPlaylistsDir();
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hDlg;
    ofn.lpstrFilter = L"M3U Playlist (*.m3u)\0*.m3u\0M3U8 Playlist (*.m3u8)\0*.m3u8\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"m3u";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!WritePlaylistFile(filePath)) {
        MessageBoxW(hDlg, T("Failed to save playlist."), T("Error"), MB_OK | MB_ICONERROR);
        return;
    }
    g_shownSourcePath = filePath;      // Ctrl+S now overwrites this file
    RegistryAdd(filePath);
    if (hCombo) FillPlaylistCombo(hCombo);
    Speak(Ts("Playlist saved"));
}

// Ctrl+S — save without asking when we know the file the list came from, which
// is the case after picking one or after a first Save As. Otherwise fall back to
// asking for a name (v2.69, reported by Lee: edits were lost with no feedback).
static void SavePlaylistQuick(HWND hDlg, HWND hCombo) {
    if (g_shownSourcePath.empty()) { SavePlaylistAs(hDlg, hCombo); return; }
    if (ShownList().empty()) {
        MessageBoxW(hDlg, T("Playlist is empty."), T("Save Playlist"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (WritePlaylistFile(g_shownSourcePath)) Speak(Ts("Playlist saved"));
    else MessageBoxW(hDlg, T("Failed to save playlist."), T("Error"), MB_OK | MB_ICONERROR);
}

// Start a track of the SHOWN list. If the user was browsing a saved playlist,
// that list becomes the playing one at this exact moment — not before, so that
// browsing never cuts what is on air (v2.69).
static void PlayShownTrack(int index) {
    if (!g_shownIsLive) {
        g_playlist = g_shownList;
        g_currentTrack = -1;
        g_shownIsLive = true;
        g_shownList.clear();
        // The window now shows the playing list; it is no longer tied to a file.
        g_shownSourcePath.clear();
    }
    if (index >= 0 && index < (int)g_playlist.size()) PlayTrack(index);
}

// Remove the selected tracks from the shown list. Shared by the Delete key and
// the "Remove track" button, which must not do two different things.
static void RemoveSelectedTracks(HWND hList) {
    std::vector<int> selected = GetSelectedIndices(hList);
    if (selected.empty()) return;
    std::vector<std::wstring>& shown = ShownList();
    for (int i = (int)selected.size() - 1; i >= 0; i--) {
        int idx = selected[i];
        if (idx < 0 || idx >= (int)shown.size()) continue;
        shown.erase(shown.begin() + idx);
        if (!g_shownIsLive) continue;
        if (g_currentTrack > idx) g_currentTrack--;
        else if (g_currentTrack == idx) g_currentTrack = -1;
    }
    int newSel = selected[0];
    if (newSel >= (int)shown.size()) newSel = (int)shown.size() - 1;
    RebuildPlaylistList(hList, newSel);
    Speak(std::to_string(selected.size()) + " " + Ts("removed"));
}

// Load the playlist currently picked in the combo into the shown list. Playback
// is deliberately left alone.
static void LoadShownFromCombo(HWND hList, HWND hCombo) {
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_comboPaths.size()) return;
    g_shownList = ParsePlaylist(g_comboPaths[sel]);
    g_shownIsLive = false;
    g_shownSourcePath = g_comboPaths[sel];   // v2.69 — Ctrl+S overwrites this one
    RebuildPlaylistList(hList, g_shownList.empty() ? -1 : 0);
    Speak(std::to_string(g_shownList.size()) + " " + Ts("tracks"));
}

// Ctrl+V on the combo: register the playlist files held in the clipboard and
// show the first one. Media files are ignored here — they belong in the track
// list, which has its own Ctrl+V.
static void PastePlaylistsIntoCombo(HWND hList, HWND hCombo) {
    std::vector<std::wstring> items;
    try { items = GetFilesFromClipboard(); } catch (...) {}
    int added = 0;
    for (const auto& item : items) {
        if (!IsURL(item.c_str()) && IsPlaylistFile(item)) { RegistryAdd(item); added++; }
    }
    if (added == 0) { Speak(Ts("No playlist in clipboard")); return; }
    FillPlaylistCombo(hCombo);
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    LoadShownFromCombo(hList, hCombo);
    Speak(std::to_string(added) + " " + Ts("playlists added"));
}

// Subclassed listbox procedure for the playlist manager.
//
// Adds keyboard shortcuts that a vanilla listbox doesn't offer and that
// screen-reader users expect:
//   Enter         play the highlighted track and close the dialog
//   Delete        remove the selected track(s)
//   Escape        close the dialog
//   Ctrl+A        select all
//   Ctrl+V        paste files/URLs from clipboard, inserted after the
//                 current selection
//   Alt+Up/Down   reorder the selected track(s); g_currentTrack is
//                 adjusted so playback continues pointing at the same
//                 file even after a move
static LRESULT CALLBACK PlaylistListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (SaveShortcut(msg, wParam)) {   // v2.69
        HWND dlg = GetParent(hwnd);
        SavePlaylistQuick(dlg, GetDlgItem(dlg, IDC_PLAYLIST_COMBO));
        return 0;
    }
    if (SpaceIsPlayPause(msg, wParam)) return 0;   // v2.69
    if (msg == WM_KEYDOWN && AnnounceListEdge(hwnd, wParam)) return 0;   // v2.69
    if (msg == WM_KEYDOWN) {
        // v2.69 — LBS_EXTENDEDSEL list: LB_GETCURSEL is not defined for a
        // multi-selection list box. LB_GETCARETINDEX is, and it is the item the
        // screen reader is on, which is what the user means by "this one".
        int sel = (int)SendMessageW(hwnd, LB_GETCARETINDEX, 0, 0);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (wParam == VK_ESCAPE) {
            EndDialog(g_playlistDlg, IDCANCEL);
            return 0;
        }

        if (wParam == VK_RETURN && sel >= 0 && sel < (int)ShownList().size()) {
            // v2.69 (Nicolas, studio radio) — start the track and STAY in the
            // list: this window is the cart wall, closing it after every launch
            // forced a reopen before each next item. PlayTrack already speaks
            // the track and posts WM_PLAYLIST_TRACK_CHANGED, so the selection
            // follows playback on its own. Escape still closes.
            PlayShownTrack(sel);
            return 0;
        }

        if (wParam == VK_DELETE) {
            RemoveSelectedTracks(hwnd);
            return 0;
        }

        // Ctrl+A: Select all
        if (ctrl && wParam == 'A') {
            SendMessageW(hwnd, LB_SETSEL, TRUE, -1);
            return 0;
        }

        // Ctrl+V: Paste
        if (ctrl && wParam == 'V') {
            try {
                // v2.69 — a pasted .m3u/.m3u8/.pls is inserted as the tracks
                // it names, like pasting those files by hand.
                std::vector<std::wstring> newFiles = ExpandPastedPlaylists(GetFilesFromClipboard());
                if (!newFiles.empty()) {
                    std::vector<std::wstring>& shown = ShownList();
                    int insertPos = (sel >= 0 && sel < (int)shown.size()) ? sel + 1 : (int)shown.size();
                    for (size_t i = 0; i < newFiles.size(); i++) {
                        shown.insert(shown.begin() + insertPos + i, newFiles[i]);
                    }
                    if (g_shownIsLive && g_currentTrack >= insertPos) {
                        g_currentTrack += (int)newFiles.size();
                    }
                    RebuildPlaylistList(hwnd, insertPos);
                    Speak(std::to_string(newFiles.size()) + " " + Ts("files pasted"));
                }
            } catch (...) {
                // Silently ignore clipboard errors
            }
            return 0;
        }
    }

    if (msg == WM_SYSKEYDOWN) {
        std::vector<int> selected = GetSelectedIndices(hwnd);
        if (selected.empty()) return CallWindowProcW(g_playlistOrigProc, hwnd, msg, wParam, lParam);

        // Alt+Up: Move selected items up
        if (wParam == VK_UP && selected[0] > 0) {
            // Move items up one by one from the top
            for (int idx : selected) {
                std::swap(ShownList()[idx], ShownList()[idx - 1]);
                if (!g_shownIsLive) continue;   // v2.69 — no playing index to keep in sync
                if (g_currentTrack == idx) g_currentTrack--;
                else if (g_currentTrack == idx - 1) g_currentTrack++;
            }
            // Rebuild and reselect
            RebuildPlaylistList(hwnd, selected[0] - 1);
            // Reselect all moved items
            for (int idx : selected) {
                SendMessageW(hwnd, LB_SETSEL, TRUE, idx - 1);
            }
            return 0;
        }

        // Alt+Down: Move selected items down
        int lastIdx = selected[selected.size() - 1];
        if (wParam == VK_DOWN && lastIdx < (int)ShownList().size() - 1) {
            // Move items down one by one from the bottom
            for (int i = (int)selected.size() - 1; i >= 0; i--) {
                int idx = selected[i];
                std::swap(ShownList()[idx], ShownList()[idx + 1]);
                if (!g_shownIsLive) continue;   // v2.69
                if (g_currentTrack == idx) g_currentTrack++;
                else if (g_currentTrack == idx + 1) g_currentTrack--;
            }
            // Rebuild and reselect
            RebuildPlaylistList(hwnd, selected[0] + 1);
            // Reselect all moved items
            for (int idx : selected) {
                SendMessageW(hwnd, LB_SETSEL, TRUE, idx + 1);
            }
            return 0;
        }
    }

    // Tell dialog we want Enter and Escape keys
    if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
    }

    return CallWindowProcW(g_playlistOrigProc, hwnd, msg, wParam, lParam);
}

// v2.69 (Lee) — the picker reads like a proper record, not a bare file name:
//   name (default)  ->  Right: number of tracks  ->  Right: where the file is.
// Left walks back. Reset to the name whenever the selection changes, so the user
// always lands on the name first. Right/Left would otherwise move the combo
// selection, which Up/Down already do.
static int g_comboFacet = 0;

static void AnnounceComboFacet(HWND hCombo) {
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_comboPaths.size()) { Speak(Ts("No playlist selected")); return; }
    if (g_comboFacet == 0) {
        int len = (int)SendMessageW(hCombo, CB_GETLBTEXTLEN, sel, 0);
        std::wstring name(len > 0 ? len + 1 : 1, L'\0');
        if (len > 0) SendMessageW(hCombo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(&name[0]));
        name.resize(wcslen(name.c_str()));
        SpeakW(name);
    } else if (g_comboFacet == 1) {
        // v2.69 (Lee) — the count must always be current. When this entry is the
        // one shown in the window, count what the window holds: the file on disk
        // is stale as soon as a track is added or removed and not yet saved.
        size_t n;
        if (!g_shownSourcePath.empty() &&
            _wcsicmp(g_shownSourcePath.c_str(), g_comboPaths[sel].c_str()) == 0) {
            n = ShownList().size();
        } else {
            n = ParsePlaylist(g_comboPaths[sel]).size();
        }
        Speak(std::to_string(n) + " " + Ts("tracks"));
    } else {
        SpeakW(g_comboPaths[sel]);
    }
}

// F2 — rename the playlist itself: the file is renamed on disk and the picker
// entry follows it. The extension is kept, so a .m3u stays a .m3u.
static std::wstring g_renameResult;

static INT_PTR CALLBACK PlaylistRenameDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            SetDlgItemTextW(hwnd, IDC_PLAYLIST_RENAME_EDIT, g_renameResult.c_str());
            SendDlgItemMessageW(hwnd, IDC_PLAYLIST_RENAME_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_PLAYLIST_RENAME_EDIT));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t buf[MAX_PATH] = {0};
                GetDlgItemTextW(hwnd, IDC_PLAYLIST_RENAME_EDIT, buf, MAX_PATH);
                std::wstring name = buf;
                while (!name.empty() && (name.front() == L' ' || name.front() == L'	')) name.erase(name.begin());
                while (!name.empty() && (name.back() == L' ' || name.back() == L'	')) name.pop_back();
                // Anything Windows refuses in a file name becomes an underscore.
                for (auto& c : name) {
                    if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
                }
                g_renameResult = name;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) { EndDialog(hwnd, IDCANCEL); return TRUE; }
            break;
    }
    return FALSE;
}

static void RenameSelectedPlaylist(HWND hDlg, HWND hCombo) {
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_comboPaths.size()) { Speak(Ts("No playlist selected")); return; }
    std::wstring path = g_comboPaths[sel];
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir  = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    std::wstring file = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = file.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? file : file.substr(0, dot);
    std::wstring ext  = (dot == std::wstring::npos) ? L"" : file.substr(dot);

    g_renameResult = stem;
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PLAYLIST_RENAME),
                   hDlg, PlaylistRenameDlgProc) != IDOK) return;
    if (g_renameResult.empty() || g_renameResult == stem) return;

    std::wstring target = dir + g_renameResult + ext;
    if (!MoveFileW(path.c_str(), target.c_str())) {
        MessageBoxW(hDlg, T("Could not rename the playlist"), T("Rename playlist"),
                    MB_OK | MB_ICONWARNING);
        return;
    }
    RegistryRemove(path);
    RegistryAdd(target);
    FillPlaylistCombo(hCombo);
    for (size_t i = 0; i < g_comboPaths.size(); i++) {
        if (_wcsicmp(g_comboPaths[i].c_str(), target.c_str()) == 0) {
            SendMessageW(hCombo, CB_SETCURSEL, i, 0);
            break;
        }
    }
    g_comboFacet = 0;
    SpeakW(g_renameResult);
}

// Combo subclass: the picker must accept Ctrl+V (a pasted playlist file is
// added to the list of saved playlists). A CBS_DROPDOWNLIST combo has no edit
// field, so nothing would handle it otherwise.
static LRESULT CALLBACK PlaylistComboProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (SaveShortcut(msg, wParam)) {   // v2.69
        HWND dlg = GetParent(hwnd);
        SavePlaylistQuick(dlg, GetDlgItem(dlg, IDC_PLAYLIST_COMBO));
        return 0;
    }
    if (SpaceIsPlayPause(msg, wParam)) return 0;   // v2.69
    if (msg == WM_KEYDOWN && AnnounceComboEdge(hwnd, wParam)) return 0;   // v2.69
    if (msg == WM_KEYDOWN && (wParam == VK_RIGHT || wParam == VK_LEFT)) {   // v2.69
        if (wParam == VK_RIGHT && g_comboFacet < 2) g_comboFacet++;
        else if (wParam == VK_LEFT && g_comboFacet > 0) g_comboFacet--;
        AnnounceComboFacet(hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_F2) {   // v2.69
        RenameSelectedPlaylist(GetParent(hwnd), hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) g_comboFacet = 0;   // v2.69
    if (msg == WM_KEYDOWN && wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        HWND dlg = GetParent(hwnd);
        PastePlaylistsIntoCombo(GetDlgItem(dlg, IDC_PLAYLIST_LIST), hwnd);
        return 0;
    }
    return CallWindowProcW(g_comboOrigProc, hwnd, msg, wParam, lParam);
}

// Playlist dialog procedure
static INT_PTR CALLBACK PlaylistDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList = nullptr;
    static HWND hCombo = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            g_playlistDlg = hwnd;
            hList = GetDlgItem(hwnd, IDC_PLAYLIST_LIST);

            // Subclass the listbox
            g_playlistOrigProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PlaylistListProc)));

            // v2.69 — the window always opens on the list being played.
            g_shownIsLive = true;
            g_shownList.clear();
            g_shownSourcePath.clear();

            hCombo = GetDlgItem(hwnd, IDC_PLAYLIST_COMBO);
            g_comboOrigProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hCombo, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PlaylistComboProc)));
            FillPlaylistCombo(hCombo);

            // v2.69 — Space must work on the buttons too.
            const int btns[] = { IDC_PLAYLIST_SAVE, IDC_PLAYLIST_DEL_TRACK, IDC_PLAYLIST_DEL_LIST };
            for (int id : btns) {
                HWND b = GetDlgItem(hwnd, id);
                if (!b) continue;
                WNDPROC prev = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PlaylistButtonProc)));
                if (!g_btnOrigProc) g_btnOrigProc = prev;   // same class, same original proc
            }

            RebuildPlaylistList(hList, g_currentTrack);
            // v2.69 (Nicolas) — open on the saved-playlist picker: the first move
            // in a show is choosing the list, then Tab down to its tracks.
            SetFocus(hCombo);
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            // v2.69 (Nicolas) — Enter must NEVER close this window. When the
            // focus is not on the list (a button, say), the dialog manager turns
            // Enter into IDOK, and DefDlgProc would end the dialog. Swallow it:
            // play the item under the caret if the list has the focus, else do
            // nothing. Escape (IDCANCEL, above) stays the way out.
            if (LOWORD(wParam) == IDOK) {
                if (hList && GetFocus() == hList) {
                    int sel = (int)SendMessageW(hList, LB_GETCARETINDEX, 0, 0);
                    if (sel >= 0 && sel < (int)ShownList().size()) PlayShownTrack(sel);
                }
                return TRUE;
            }
            // Handle Save button
            if (LOWORD(wParam) == IDC_PLAYLIST_SAVE) {
                SavePlaylistAs(hwnd, hCombo);
                return TRUE;
            }
            // v2.69 — picking a saved playlist loads it in this window only.
            // Playback keeps going: on air, browsing must never cut the track.
            if (LOWORD(wParam) == IDC_PLAYLIST_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
                g_comboFacet = 0;   // v2.69 — a new entry always reads its name first
                LoadShownFromCombo(hList, hCombo);
                return TRUE;
            }
            // v2.69 — two separate delete buttons, because "delete" means two
            // very different things here.
            if (LOWORD(wParam) == IDC_PLAYLIST_DEL_TRACK) {
                RemoveSelectedTracks(hList);
                SetFocus(hList);
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_PLAYLIST_DEL_LIST) {
                int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                if (sel < 0 || sel >= (int)g_comboPaths.size()) {
                    Speak(Ts("No playlist selected"));
                    return TRUE;
                }
                std::wstring path = g_comboPaths[sel];
                int answer = MessageBoxW(hwnd,
                    T("Delete this playlist file from the disk as well?"),
                    T("Delete playlist"), MB_YESNOCANCEL | MB_ICONQUESTION);
                if (answer == IDCANCEL) return TRUE;
                RegistryRemove(path);
                if (answer == IDYES && !DeleteFileW(path.c_str())) {
                    MessageBoxW(hwnd, T("Could not delete the file"),
                                T("Delete playlist"), MB_OK | MB_ICONWARNING);
                }
                FillPlaylistCombo(hCombo);
                Speak(Ts("Playlist removed from the list"));
                return TRUE;
            }
            // Handle double-click on listbox
            if (LOWORD(wParam) == IDC_PLAYLIST_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(hList, LB_GETCARETINDEX, 0, 0);
                if (sel >= 0 && sel < (int)ShownList().size()) {
                    PlayShownTrack(sel);   // v2.69 — stay in the list, as for Enter
                }
                return TRUE;
            }
            break;

        case WM_PLAYLIST_TRACK_CHANGED:
            // Update selection to follow current track
            if (g_playlistFollowPlayback && hList && g_currentTrack >= 0 && g_shownIsLive &&
                SendMessageW(hList, LB_GETSELCOUNT, 0, 0) <= 1) {
                // v2.69 — only follow when the user is NOT holding a multi-item
                // selection: clearing it mid-show (they were about to move or
                // delete those tracks) would be worse than not following.
                // v2.69 — this list is LBS_EXTENDEDSEL: LB_SETCURSEL is not
                // supported on a multi-selection list box and silently failed,
                // so the selection never actually followed playback. Clear the
                // selection, select the playing track and move the caret to it
                // (the caret is what a screen reader reads and what Enter acts
                // on). Only noticeable now that the window stays open while
                // playing, which is exactly when following matters.
                SendMessageW(hList, LB_SETSEL, FALSE, (LPARAM)-1);
                SendMessageW(hList, LB_SETSEL, TRUE, g_currentTrack);
                SendMessageW(hList, LB_SETCARETINDEX, g_currentTrack, FALSE);
            }
            return TRUE;

        case WM_PLAYLIST_CONTENT_CHANGED:
            // v2.69 — the playlist itself was replaced while this window stayed
            // open (files opened from Explorer). Without this the window showed
            // the OLD titles: the screen reader announced one track and Enter
            // played another.
            // Only when the window shows the playing list: if the user is
            // browsing a saved playlist, leave their view alone.
            if (hList && g_shownIsLive) RebuildPlaylistList(hList, g_currentTrack);
            return TRUE;

        case WM_DESTROY:
            // Restore original listbox procedure
            if (g_playlistOrigProc && hList) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_playlistOrigProc));
                g_playlistOrigProc = nullptr;
            }
            if (g_btnOrigProc) {   // v2.69
                const int btns[] = { IDC_PLAYLIST_SAVE, IDC_PLAYLIST_DEL_TRACK, IDC_PLAYLIST_DEL_LIST };
                for (int id : btns) {
                    HWND b = GetDlgItem(hwnd, id);
                    if (b) SetWindowLongPtrW(b, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_btnOrigProc));
                }
                g_btnOrigProc = nullptr;
            }
            if (g_comboOrigProc && hCombo) {   // v2.69
                SetWindowLongPtrW(hCombo, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_comboOrigProc));
                g_comboOrigProc = nullptr;
            }
            // Browsing state is per-session: next open starts on the live list.
            g_shownIsLive = true;
            g_shownList.clear();
            g_playlistDlg = nullptr;
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Resize listbox to fill space, leaving room for buttons/label at bottom
            // v2.69 — row 1: picker + "delete playlist"; middle: the tracks;
            // bottom: Save + "remove track" + the shortcut hint.
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_COMBO), nullptr,
                95, 7, (w > 330) ? w - 230 : 100, 200, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_DEL_LIST), nullptr,
                w - 130, 6, 123, 18, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_LIST), nullptr,
                7, 32, w - 14, h - 65, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_SAVE), nullptr,
                7, h - 27, 65, 18, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_DEL_TRACK), nullptr,
                78, h - 27, 92, 18, SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 350;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// v2.69 (Nicolas) — folder where playlists are saved. Two tiers, mirroring the
// download-folder logic so there is always a usable answer:
//   1. Options > Playback > "Playlists folder", when set, reachable and writable.
//   2. Otherwise <download root>\\Playlist, i.e. it follows wherever the user
//      sends downloads (Music\\MediaAccess\\Playlist for a configured folder,
//      Downloads\\MediaAccess\\Playlist out of the box). Created if missing.
// A configured-but-unusable path falls back silently rather than blocking a save.
std::wstring GetPlaylistsDir() {
    std::wstring pref = g_playlistFolder;
    if (!pref.empty()) {
        wchar_t abs[MAX_PATH] = {0};
        DWORD n = GetFullPathNameW(pref.c_str(), MAX_PATH, abs, nullptr);
        if (n > 0 && n < MAX_PATH) {
            CreateDirectoryW(abs, nullptr);
            DWORD attrs = GetFileAttributesW(abs);
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                // v2.69 — actually prove it is writable, like the download root
                // does: a read-only folder or an offline share would otherwise be
                // accepted and only fail later, at save time.
                std::wstring probe = std::wstring(abs) + L"\\.ma_write_test";
                HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                       nullptr);
                if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return abs; }
            }
        }
    }
    std::wstring dir = YouTubeGetDownloadRoot() + L"\\Playlist";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Show playlist manager dialog
void ShowPlaylistDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PLAYLIST), g_hwnd, PlaylistDlgProc);
}

// Notify playlist dialog about track change
void NotifyPlaylistTrackChanged() {
    if (g_playlistDlg) {
        PostMessageW(g_playlistDlg, WM_PLAYLIST_TRACK_CHANGED, 0, 0);
    }
}

// v2.69 — the playlist CONTENT was replaced (not just the current index).
// Tells an open manager window to rebuild its list instead of only moving the
// caret inside stale entries.
void NotifyPlaylistContentChanged() {
    if (g_playlistDlg) {
        PostMessageW(g_playlistDlg, WM_PLAYLIST_CONTENT_CHANGED, 0, 0);
    }
}
