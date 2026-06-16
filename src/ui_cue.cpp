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

// ---- Track-list search (v2.36) ------------------------------------------
// Ctrl+F opens a small "Find track" prompt; F3 / Shift+F3 jump to the next /
// previous title match (case-insensitive substring, wrap-around). Keys are
// LOCAL to this modal dialog — not registered actions, so no keymap impact.
static std::wstring s_cueSearchQuery;

// Title text of row i (without the "N. " prefix RebuildTrackList adds), so a
// digit in the query never false-matches the numbering of every row.
static std::wstring TrackRowTitle(int i) {
    if (TrackListIsChapters()) {
        if (i < 0 || i >= (int)g_chapters.size()) return L"";
        return g_chapters[i].name;
    }
    if (i < 0 || i >= (int)g_playlist.size()) return L"";
    std::wstring f = g_playlist[i];
    size_t pos = f.find_last_of(L"\\/");
    if (pos != std::wstring::npos) f = f.substr(pos + 1);
    return f;
}

// Case-insensitive substring test using CompareStringW with NORM_IGNORECASE so
// accented characters fold correctly (é matches É) — important for French track
// titles. Mirrors FindCI in daisy_book.cpp; towlower in the "C" locale does NOT
// fold accents, which would silently miss accented titles for the FR user.
static bool TitleContainsCI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        if (CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                           hay.c_str() + i, (int)needle.size(),
                           needle.c_str(), (int)needle.size()) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// Next row whose title contains s_cueSearchQuery, scanning from startIdx in
// direction dir (+1/-1) with wrap-around. includeStart checks startIdx itself
// first (used for a fresh Ctrl+F so a matching current row counts). -1 if none.
static int FindCueMatch(int startIdx, int dir, bool includeStart) {
    int rowCount = TrackListIsChapters() ? (int)g_chapters.size() : (int)g_playlist.size();
    if (rowCount <= 0 || s_cueSearchQuery.empty()) return -1;
    if (startIdx < 0) startIdx = 0;
    for (int step = includeStart ? 0 : 1; step <= rowCount; step++) {
        int idx = ((startIdx + dir * step) % rowCount + rowCount) % rowCount;
        if (TitleContainsCI(TrackRowTitle(idx), s_cueSearchQuery)) return idx;
    }
    return -1;
}

static INT_PTR CALLBACK CueSearchDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM /*lp*/) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            SetDlgItemTextW(dlg, IDC_CUE_SEARCH_EDIT, s_cueSearchQuery.c_str());
            SendDlgItemMessageW(dlg, IDC_CUE_SEARCH_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, IDC_CUE_SEARCH_EDIT));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[1024] = {0};
                GetDlgItemTextW(dlg, IDC_CUE_SEARCH_EDIT, buf, 1024);
                s_cueSearchQuery = buf;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
            break;
        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// Show the "Find track" prompt; true when the user confirmed a non-empty query.
static bool PromptCueSearch(HWND owner) {
    return DialogBoxW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CUE_SEARCH),
                      owner, CueSearchDlgProc) == IDOK && !s_cueSearchQuery.empty();
}

// Move the selection to the next/previous matching track and announce it.
// A programmatic LB_SETCURSEL is not reliably spoken by NVDA/JAWS without a
// user keystroke, so we announce the matched title explicitly.
static void DoCueSearch(HWND hList, int dir, bool includeCurrent) {
    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    int m = FindCueMatch(sel, dir, includeCurrent);
    if (m >= 0) {
        SendMessageW(hList, LB_SETCURSEL, m, 0);
        Speak(WideToUtf8(TrackRowTitle(m)));
    } else {
        Speak(Ts("No matching track"));
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
        // Ctrl+F: open the find prompt, then jump to the first match.
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'F' || wParam == 'f')) {
            if (PromptCueSearch(g_trackListDlg)) DoCueSearch(hwnd, +1, true);
            SetFocus(hwnd);
            return 0;
        }
        // F3 / Shift+F3: next / previous match (open the prompt if no query yet).
        if (wParam == VK_F3) {
            bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (s_cueSearchQuery.empty()) {
                if (PromptCueSearch(g_trackListDlg)) DoCueSearch(hwnd, back ? -1 : +1, true);
                SetFocus(hwnd);
            } else {
                DoCueSearch(hwnd, back ? -1 : +1, false);
            }
            return 0;
        }
    }

    if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE ||
                     pmsg->wParam == VK_F3)) {
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
    s_cueSearchQuery.clear();  // fresh search per opening (v2.36)
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_TRACK_LIST), g_hwnd, TrackListDlgProc);
}
