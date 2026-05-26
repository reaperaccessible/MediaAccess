#include "ui_internal.h"
#include "mediaaccess/translations.h"

// Forward declaration for GetFilesFromClipboard (defined in ui.cpp)
extern std::vector<std::wstring> GetFilesFromClipboard();

// Helper to rebuild playlist listbox
static void RebuildPlaylistList(HWND hList, int selectIndex = -1) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_playlist.size(); i++) {
        std::wstring filename = g_playlist[i];
        size_t pos = filename.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            filename = filename.substr(pos + 1);
        }
        wchar_t buf[512];
        swprintf(buf, 512, L"%d. %s", (int)(i + 1), filename.c_str());
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
    }
    if (selectIndex >= 0 && selectIndex < (int)g_playlist.size()) {
        SendMessageW(hList, LB_SETCURSEL, selectIndex, 0);
    }
}

// Subclassed listbox data for playlist manager
static WNDPROC g_playlistOrigProc = nullptr;
static HWND g_playlistDlg = nullptr;

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

// Subclassed listbox procedure for playlist manager
static LRESULT CALLBACK PlaylistListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (wParam == VK_ESCAPE) {
            EndDialog(g_playlistDlg, IDCANCEL);
            return 0;
        }

        if (wParam == VK_RETURN && sel >= 0 && sel < (int)g_playlist.size()) {
            PlayTrack(sel);
            EndDialog(g_playlistDlg, IDOK);
            return 0;
        }

        if (wParam == VK_DELETE) {
            std::vector<int> selected = GetSelectedIndices(hwnd);
            if (!selected.empty()) {
                // Remove from end to preserve indices
                for (int i = (int)selected.size() - 1; i >= 0; i--) {
                    int idx = selected[i];
                    if (idx >= 0 && idx < (int)g_playlist.size()) {
                        g_playlist.erase(g_playlist.begin() + idx);
                        if (g_currentTrack > idx) {
                            g_currentTrack--;
                        } else if (g_currentTrack == idx) {
                            g_currentTrack = -1;
                        }
                    }
                }
                int newSel = selected[0];
                if (newSel >= (int)g_playlist.size()) newSel = (int)g_playlist.size() - 1;
                RebuildPlaylistList(hwnd, newSel);
                Speak(std::to_string(selected.size()) + " " + Ts("removed"));
                return 0;
            }
        }

        // Ctrl+A: Select all
        if (ctrl && wParam == 'A') {
            SendMessageW(hwnd, LB_SETSEL, TRUE, -1);
            return 0;
        }

        // Ctrl+V: Paste
        if (ctrl && wParam == 'V') {
            try {
                std::vector<std::wstring> newFiles = GetFilesFromClipboard();
                if (!newFiles.empty()) {
                    int insertPos = (sel >= 0 && sel < (int)g_playlist.size()) ? sel + 1 : (int)g_playlist.size();
                    for (size_t i = 0; i < newFiles.size(); i++) {
                        g_playlist.insert(g_playlist.begin() + insertPos + i, newFiles[i]);
                    }
                    if (g_currentTrack >= insertPos) {
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
                std::swap(g_playlist[idx], g_playlist[idx - 1]);
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
        if (wParam == VK_DOWN && lastIdx < (int)g_playlist.size() - 1) {
            // Move items down one by one from the bottom
            for (int i = (int)selected.size() - 1; i >= 0; i--) {
                int idx = selected[i];
                std::swap(g_playlist[idx], g_playlist[idx + 1]);
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

// Playlist dialog procedure
static INT_PTR CALLBACK PlaylistDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList = nullptr;

    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            g_playlistDlg = hwnd;
            hList = GetDlgItem(hwnd, IDC_PLAYLIST_LIST);

            // Subclass the listbox
            g_playlistOrigProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PlaylistListProc)));

            RebuildPlaylistList(hList, g_currentTrack);
            SetFocus(hList);
            return FALSE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            // Handle Save button
            if (LOWORD(wParam) == IDC_PLAYLIST_SAVE) {
                if (g_playlist.empty()) {
                    MessageBoxW(hwnd, T("Playlist is empty."), T("Save Playlist"), MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }

                wchar_t filePath[MAX_PATH] = L"playlist.m3u";
                OPENFILENAMEW ofn = {0};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"M3U Playlist (*.m3u)\0*.m3u\0M3U8 Playlist (*.m3u8)\0*.m3u8\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = filePath;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrDefExt = L"m3u";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

                if (GetSaveFileNameW(&ofn)) {
                    FILE* f = _wfopen(filePath, L"w, ccs=UTF-8");
                    if (f) {
                        fwprintf(f, L"#EXTM3U\n");
                        for (const auto& path : g_playlist) {
                            fwprintf(f, L"%s\n", path.c_str());
                        }
                        fclose(f);
                        Speak(Ts("Playlist saved"));
                    } else {
                        MessageBoxW(hwnd, T("Failed to save playlist."), T("Error"), MB_OK | MB_ICONERROR);
                    }
                }
                return TRUE;
            }
            // Handle double-click on listbox
            if (LOWORD(wParam) == IDC_PLAYLIST_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)g_playlist.size()) {
                    PlayTrack(sel);
                    EndDialog(hwnd, IDOK);
                }
                return TRUE;
            }
            break;

        case WM_PLAYLIST_TRACK_CHANGED:
            // Update selection to follow current track
            if (g_playlistFollowPlayback && hList && g_currentTrack >= 0) {
                SendMessageW(hList, LB_SETCURSEL, g_currentTrack, 0);
            }
            return TRUE;

        case WM_DESTROY:
            // Restore original listbox procedure
            if (g_playlistOrigProc && hList) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_playlistOrigProc));
                g_playlistOrigProc = nullptr;
            }
            g_playlistDlg = nullptr;
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Resize listbox to fill space, leaving room for buttons/label at bottom
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_LIST), nullptr,
                7, 7, w - 14, h - 40, SWP_NOZORDER);
            // Reposition Save button at bottom-left
            SetWindowPos(GetDlgItem(hwnd, IDC_PLAYLIST_SAVE), nullptr,
                7, h - 27, 50, 14, SWP_NOZORDER);
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
