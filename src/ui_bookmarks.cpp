#include "ui_internal.h"
#include "mediaaccess/translations.h"

// Bookmarks dialog

static std::vector<Bookmark> g_allBookmarks;       // All bookmarks from database
static std::vector<Bookmark> g_dialogBookmarks;    // Filtered bookmarks shown in list
static std::wstring g_currentFilePath;             // Current file path for filtering
static HWND g_bookmarksDlg = nullptr;              // Dialog handle for subclass access

// Jump to a bookmark (load file if needed and seek to position)
static void JumpToBookmark(const Bookmark& bm) {
    // Check if the file is in the current playlist
    int trackIndex = -1;
    for (size_t i = 0; i < g_playlist.size(); i++) {
        if (_wcsicmp(g_playlist[i].c_str(), bm.filePath.c_str()) == 0) {
            trackIndex = static_cast<int>(i);
            break;
        }
    }

    if (trackIndex >= 0) {
        // File is in playlist, switch to it if not current
        if (trackIndex != g_currentTrack) {
            PlayTrack(trackIndex);
        }
    } else {
        // Load the file
        g_playlist.clear();
        g_playlist.push_back(bm.filePath);
        g_currentTrack = -1;
        PlayTrack(0);
    }

    // Seek to position (give a moment for file to load if needed)
    SeekToPosition(bm.position);
}

// Refresh the bookmark listbox based on current filter
static void RefreshBookmarkList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
    HWND hFilter = GetDlgItem(hwnd, IDC_BOOKMARK_FILTER);

    // Get filter selection (0 = current file, 1 = all)
    int filterSel = static_cast<int>(SendMessageW(hFilter, CB_GETCURSEL, 0, 0));
    bool showAll = (filterSel == 1);

    // Clear and repopulate
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_dialogBookmarks.clear();

    for (const auto& bm : g_allBookmarks) {
        if (showAll || _wcsicmp(bm.filePath.c_str(), g_currentFilePath.c_str()) == 0) {
            g_dialogBookmarks.push_back(bm);
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(bm.displayName.c_str()));
        }
    }

    // Select first item if any
    if (!g_dialogBookmarks.empty()) {
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    }
}

// Subclass procedure for the bookmark listbox to handle Enter, Delete, and Escape keys
static WNDPROC g_origListProc = nullptr;

static LRESULT CALLBACK BookmarkListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Enter - jump to bookmark
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                JumpToBookmark(g_dialogBookmarks[sel]);
                EndDialog(GetParent(hwnd), IDOK);
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Delete - remove bookmark
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                int bookmarkId = g_dialogBookmarks[sel].id;
                RemoveBookmark(bookmarkId);

                // Remove from both lists
                g_dialogBookmarks.erase(g_dialogBookmarks.begin() + sel);
                for (auto it = g_allBookmarks.begin(); it != g_allBookmarks.end(); ++it) {
                    if (it->id == bookmarkId) {
                        g_allBookmarks.erase(it);
                        break;
                    }
                }

                SendMessageW(hwnd, LB_DELETESTRING, sel, 0);

                // Select next item
                int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                if (count > 0) {
                    if (sel >= count) sel = count - 1;
                    SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                }

                Speak(Ts("Bookmark removed"));
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Escape - close dialog
            EndDialog(GetParent(hwnd), IDCANCEL);
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        // Only capture Enter/Escape, let Tab pass through for dialog navigation
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origListProc, hwnd, msg, wParam, lParam);
}

// Bookmarks dialog procedure
static INT_PTR CALLBACK BookmarksDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            g_bookmarksDlg = hwnd;

            // Get current file path
            if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                g_currentFilePath = g_playlist[g_currentTrack];
            } else {
                g_currentFilePath.clear();
            }

            // Load all bookmarks
            g_allBookmarks = GetAllBookmarks();

            // Setup filter combobox
            HWND hFilter = GetDlgItem(hwnd, IDC_BOOKMARK_FILTER);
            SendMessageW(hFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Current file")));
            SendMessageW(hFilter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("All bookmarks")));

            // Default to current file if playing, otherwise all bookmarks
            SendMessageW(hFilter, CB_SETCURSEL, g_currentFilePath.empty() ? 1 : 0, 0);

            // Subclass the listbox to handle Enter, Delete, and Escape
            HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
            g_origListProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hList, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(BookmarkListSubclassProc)));

            // Populate listbox
            RefreshBookmarkList(hwnd);

            SetFocus(hList);
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BOOKMARK_FILTER:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        RefreshBookmarkList(hwnd);
                        SetFocus(GetDlgItem(hwnd, IDC_BOOKMARK_LIST));
                        return TRUE;
                    }
                    break;

                case IDC_BOOKMARK_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to jump to bookmark
                        HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_dialogBookmarks.size())) {
                            JumpToBookmark(g_dialogBookmarks[sel]);
                            EndDialog(hwnd, IDOK);
                        }
                        return TRUE;
                    }
                    break;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Reposition filter combo at top (keep y position, stretch width)
            SetWindowPos(GetDlgItem(hwnd, IDC_BOOKMARK_FILTER), nullptr,
                35, 8, 120, 60, SWP_NOZORDER);
            // Resize listbox to fill space, leaving room for filter at top and button at bottom
            SetWindowPos(GetDlgItem(hwnd, IDC_BOOKMARK_LIST), nullptr,
                7, 26, w - 14, h - 52, SWP_NOZORDER);
            // Reposition Close button at bottom-right
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr,
                w - 57, h - 20, 50, 14, SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }

        case WM_DESTROY: {
            // Restore original listbox proc
            HWND hList = GetDlgItem(hwnd, IDC_BOOKMARK_LIST);
            if (g_origListProc) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origListProc));
                g_origListProc = nullptr;
            }
            g_bookmarksDlg = nullptr;
            break;
        }
    }
    return FALSE;
}

// Show bookmarks dialog
void ShowBookmarksDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_BOOKMARKS), g_hwnd, BookmarksDlgProc);
}
