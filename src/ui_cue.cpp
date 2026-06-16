// =============================================================================
// ui_cue.cpp — accessible track-list dialog for .cue sheets (v2.34).
//
// Cloned from the playlist manager's listbox pattern (ui_playlist.cpp):
//   Enter   jump to the selected track and close
//   Escape  close
// Single-FILE cue: the tracks are chapters of one stream, so Enter seeks to the
// chapter's start position. Multi-FILE cue (or no chapters at all): the entries
// are playlist tracks, so Enter calls PlayTrack(sel).
// =============================================================================

#include "ui_internal.h"
#include "mediaaccess/cue_sheet.h"
#include "mediaaccess/translations.h"

// True when the current track list is chapter-based (single-FILE cue).
static bool TrackListIsChapters() {
    return !g_chapters.empty();
}

// Build "N. name" rows: chapters when in cue/chapter mode, else playlist files.
static void RebuildTrackList(HWND hList, int selectIndex = -1) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    if (TrackListIsChapters()) {
        for (size_t i = 0; i < g_chapters.size(); i++) {
            wchar_t buf[600];
            swprintf(buf, 600, L"%d. %s", (int)(i + 1), g_chapters[i].name.c_str());
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
        }
        if (selectIndex >= 0 && selectIndex < (int)g_chapters.size()) {
            SendMessageW(hList, LB_SETCURSEL, selectIndex, 0);
        }
    } else {
        for (size_t i = 0; i < g_playlist.size(); i++) {
            std::wstring filename = g_playlist[i];
            size_t pos = filename.find_last_of(L"\\/");
            if (pos != std::wstring::npos) filename = filename.substr(pos + 1);
            wchar_t buf[600];
            swprintf(buf, 600, L"%d. %s", (int)(i + 1), filename.c_str());
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
        }
        if (selectIndex >= 0 && selectIndex < (int)g_playlist.size()) {
            SendMessageW(hList, LB_SETCURSEL, selectIndex, 0);
        }
    }
}

static WNDPROC g_trackListOrigProc = nullptr;
static HWND g_trackListDlg = nullptr;

static void JumpToTrack(int sel) {
    if (sel < 0) return;
    if (TrackListIsChapters()) {
        if (sel >= (int)g_chapters.size()) return;
        SeekToPosition(g_chapters[sel].position);
        Speak(WideToUtf8(g_chapters[sel].name));
    } else {
        if (sel >= (int)g_playlist.size()) return;
        PlayTrack(sel);
    }
}

static LRESULT CALLBACK TrackListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        int sel = (int)SendMessageW(hwnd, LB_GETCURSEL, 0, 0);
        if (wParam == VK_ESCAPE) {
            EndDialog(g_trackListDlg, IDCANCEL);
            return 0;
        }
        if (wParam == VK_RETURN && sel >= 0) {
            JumpToTrack(sel);
            EndDialog(g_trackListDlg, IDOK);
            return 0;
        }
    }

    if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
    }

    return CallWindowProcW(g_trackListOrigProc, hwnd, msg, wParam, lParam);
}

static INT_PTR CALLBACK TrackListDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hList = nullptr;
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            g_trackListDlg = hwnd;
            hList = GetDlgItem(hwnd, IDC_TRACK_LIST);
            g_trackListOrigProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(TrackListProc)));

            int sel = TrackListIsChapters() ? GetCurrentChapterIndex() : g_currentTrack;
            if (sel < 0) sel = 0;
            RebuildTrackList(hList, sel);
            SetFocus(hList);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (LOWORD(wParam) == IDC_TRACK_LIST && HIWORD(wParam) == LBN_DBLCLK) {
                int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    JumpToTrack(sel);
                    EndDialog(hwnd, IDOK);
                }
                return TRUE;
            }
            break;
        case WM_DESTROY:
            if (g_trackListOrigProc && hList) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_trackListOrigProc));
                g_trackListOrigProc = nullptr;
            }
            g_trackListDlg = nullptr;
            break;
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

void ShowTrackListDialog() {
    // Nothing to show if neither chapters nor a playlist exist.
    if (g_chapters.empty() && g_playlist.empty()) {
        Speak(Ts("No track"));
        return;
    }
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_TRACK_LIST), g_hwnd, TrackListDlgProc);
}
