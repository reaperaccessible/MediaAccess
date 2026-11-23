// ui_cd_rip.cpp — "Extract audio CD" window, phase 1 of
// docs/PLAN_EXTRACTION_CD.md: choose the drive, list the tracks, eject.
//
// Modeless like the YouTube window: closing only hides it, and the main
// message loop routes its keys through IsDialogMessageW (main.cpp).
//
// Speech: the window title carries what the screen reader cannot guess
// ("Extract audio CD: drive D, 12 tracks, 47 minutes"), so opening the window
// is said in one go by NVDA and JAWS (title, list, line). The program itself
// speaks only for events the reader cannot see: a disc inserted or removed,
// the tray opened or closed, and the edges of a list ("First,", "Last,").

#include "ui_internal.h"
#include "mediaaccess/ui_cd_rip.h"
#include "mediaaccess/disc_drives.h"
#include "mediaaccess/basscd_loader.h"
#include <dbt.h>
#include <vector>
#include <cmath>

using mediaaccess::DiscContents;
using mediaaccess::DiscDrive;
using mediaaccess::DiscTrack;
using mediaaccess::DriveMediaType;

namespace {

HWND                   g_cdDialog = nullptr;
// Set when the program itself opened the tray: Windows then reports the disc
// as removed, which must not be said a second time ("Tray open" already was).
bool                   g_trayOpenedByUs = false;
std::vector<DiscDrive> g_drives;
DiscContents           g_contents;       // disc in the selected drive
DiscContents           g_listContents;   // disc the track list shows
std::vector<int>       g_rowTrack;       // list row -> index in g_listContents.tracks

const DiscDrive* SelectedDrive(HWND dlg) {
    int sel = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_DRIVE, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_drives.size()) return nullptr;
    return &g_drives[sel];
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring Duration(double seconds) {
    int s = (int)(seconds + 0.5);
    return Format(L"%d:%02d", s / 60, s % 60);
}

std::wstring DriveName(const DiscDrive& d) {
    return Format(T("drive %lc"), d.letter);
}

// "drive D, 12 tracks, 47 minutes" / "drive D, no disc" / "drive D, no audio CD"
std::wstring DiscSummary(const DiscDrive& d, const DiscContents& c) {
    std::wstring s = DriveName(d) + L", ";
    if (c.type == DriveMediaType::AudioCD) {
        int n = c.AudioTrackCount();
        int minutes = (int)(c.AudioSeconds() / 60.0 + 0.5);
        s += (n == 1) ? std::wstring(T("1 track")) : Format(T("%d tracks"), n);
        s += L", ";
        s += (minutes <= 1) ? Format(T("%d minute"), minutes) : Format(T("%d minutes"), minutes);
    } else if (c.type == DriveMediaType::None) {
        s += T("no disc");
    } else {
        s += T("no audio CD");
    }
    return s;
}

void UpdateTitleAndStatus(HWND dlg) {
    const DiscDrive* d = SelectedDrive(dlg);
    std::wstring summary = d ? DiscSummary(*d, g_contents) : std::wstring();
    std::wstring title = summary.empty() ? std::wstring(T("Extract audio CD"))
                                         : Format(T("Extract audio CD: %ls"), summary.c_str());
    SetWindowTextW(dlg, title.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_STATUS, summary.c_str());
}

std::wstring RowText(int row) {
    if (row < 0 || row >= (int)g_rowTrack.size()) return std::wstring();
    const DiscTrack& t = g_listContents.tracks[g_rowTrack[row]];
    return Format(L"%02d - ", t.number) + Format(T("Track %d"), t.number);
}

void FillTrackList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    g_rowTrack.clear();
    for (size_t i = 0; i < g_listContents.tracks.size(); ++i) {
        if (g_listContents.tracks[i].isData) continue;   // data tracks are never extracted
        g_rowTrack.push_back((int)i);
        int row = (int)g_rowTrack.size() - 1;
        std::wstring title = RowText(row);
        std::wstring dur = Duration(g_listContents.tracks[i].seconds);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = &title[0];
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, row, 1, &dur[0]);
        ListView_SetCheckState(list, row, TRUE);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
}

bool SameDisc(const DiscContents& a, const DiscContents& b) {
    if (a.tracks.size() != b.tracks.size()) return false;
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        if (a.tracks[i].isData != b.tracks[i].isData) return false;
        if (a.tracks[i].number != b.tracks[i].number) return false;
        if (fabs(a.tracks[i].seconds - b.tracks[i].seconds) > 0.02) return false;
    }
    return true;
}

// Reads the disc in the selected drive; the list always shows what is really
// in it (empty when there is no audio CD). It is left alone when the very same
// disc is read again (the window reopened), so the ticked tracks are kept.
void RefreshDisc(HWND dlg) {
    const DiscDrive* d = SelectedDrive(dlg);
    g_contents = d ? mediaaccess::ReadDiscContents(*d) : DiscContents();
    DiscContents shown = (g_contents.type == DriveMediaType::AudioCD) ? g_contents : DiscContents();
    if (!SameDisc(shown, g_listContents)) {
        g_listContents = shown;
        FillTrackList(dlg);
    }
    UpdateTitleAndStatus(dlg);
    EnableWindow(GetDlgItem(dlg, IDC_CDRIP_EJECT), d && d->canOpen);
}

void FillDriveCombo(HWND dlg) {
    HWND combo = GetDlgItem(dlg, IDC_CDRIP_DRIVE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const DiscDrive& d : g_drives) {
        std::wstring text = Format(L"%lc: ", d.letter) + d.vendorModel;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    }
    // First drive holding an audio CD, else the first drive.
    int pick = 0;
    for (size_t i = 0; i < g_drives.size(); ++i) {
        if (mediaaccess::ReadDiscContents(g_drives[i]).type == DriveMediaType::AudioCD) {
            pick = (int)i;
            break;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, pick, 0);
}

// Lands the focus on the first track, or on the drive list when there is none.
void FocusFirstControl(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
    if (ListView_GetItemCount(list) > 0) {
        ListView_SetItemState(list, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)list, TRUE);
    } else {
        SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dlg, IDC_CDRIP_DRIVE), TRUE);
    }
}

// House rule: a list never hits a silent edge.
LRESULT CALLBACK TrackListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                               UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) {
        int count = ListView_GetItemCount(hwnd);
        int cur = ListView_GetNextItem(hwnd, -1, LVNI_FOCUSED);
        if (count > 0 && cur >= 0) {
            bool first = (wParam == VK_UP && cur == 0);
            bool last = (wParam == VK_DOWN && cur == count - 1);
            if (first || last) {
                SpeakW(std::wstring(first ? T("First,") : T("Last,")) + L" " + RowText(cur));
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DriveComboProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN) &&
        !SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0)) {
        int count = (int)SendMessageW(hwnd, CB_GETCOUNT, 0, 0);
        int cur = (int)SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
        bool first = (wParam == VK_UP && cur == 0);
        bool last = (wParam == VK_DOWN && cur == count - 1);
        if (count > 0 && (first || last)) {
            int len = (int)SendMessageW(hwnd, CB_GETLBTEXTLEN, cur, 0);
            std::wstring text(len > 0 ? len + 1 : 1, L'\0');
            if (len > 0) SendMessageW(hwnd, CB_GETLBTEXT, cur, (LPARAM)&text[0]);
            text.resize(wcslen(text.c_str()));
            SpeakW(std::wstring(first ? T("First,") : T("Last,")) + L" " + text);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void InitTrackList(HWND dlg) {
    HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 380;
    col.pszText = const_cast<wchar_t*>(T("Title"));
    ListView_InsertColumn(list, 0, &col);
    col.cx = 90;
    col.pszText = const_cast<wchar_t*>(T("Duration"));
    ListView_InsertColumn(list, 1, &col);
    SetWindowSubclass(list, TrackListProc, 1, 0);
    SetWindowSubclass(GetDlgItem(dlg, IDC_CDRIP_DRIVE), DriveComboProc, 1, 0);
}

void OnEject(HWND dlg) {
    const DiscDrive* d = SelectedDrive(dlg);
    bool nowOpen = false;
    if (!d) return;
    bool wasOpen = mediaaccess::IsDriveTrayOpen(*d);
    if (!mediaaccess::ToggleDriveTray(*d, &nowOpen)) {
        // Many slim and USB drives can open their tray but not pull it back in.
        SpeakW(wasOpen ? T("Close the tray by hand") : T("The tray cannot be opened"));
        return;
    }
    if (nowOpen) {
        g_trayOpenedByUs = true;
        g_contents = DiscContents();
        g_listContents = DiscContents();
        FillTrackList(dlg);
        UpdateTitleAndStatus(dlg);
        SpeakW(T("Tray open"));
    } else {
        // Closing the tray: the disc is read when Windows reports it
        // (WM_DEVICECHANGE), which then announces it.
        SpeakW(T("Tray closed"));
    }
}

// A disc arrived in or left one of the drives (Windows broadcast).
void OnMediaChange(HWND dlg, WPARAM event, LPARAM lParam) {
    if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE) return;
    const DEV_BROADCAST_HDR* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
    if (!hdr || hdr->dbch_devicetype != DBT_DEVTYP_VOLUME) return;
    const DEV_BROADCAST_VOLUME* vol = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(hdr);
    if (!(vol->dbcv_flags & DBTF_MEDIA)) return;
    const DiscDrive* d = SelectedDrive(dlg);
    if (!d || d->letter < L'A' || d->letter > L'Z') return;
    if (!(vol->dbcv_unitmask & (1u << (d->letter - L'A')))) return;

    RefreshDisc(dlg);
    if (event == DBT_DEVICEREMOVECOMPLETE && g_trayOpenedByUs) {
        g_trayOpenedByUs = false;   // "Tray open" was already said for this event
        return;
    }
    g_trayOpenedByUs = false;
    if (event == DBT_DEVICEARRIVAL && g_contents.type == DriveMediaType::AudioCD) {
        int n = g_contents.AudioTrackCount();
        SpeakW(Format(T("CD inserted in %ls"), DriveName(*d).c_str()) + L", " +
               ((n == 1) ? std::wstring(T("1 track")) : Format(T("%d tracks"), n)));
        // The list was empty and may hold the keyboard focus: give the focus to
        // a real line (the first track), said after the announcement, instead of
        // letting the reader describe an unfocused line ("not selected").
        HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
        if (GetFocus() == list && ListView_GetItemCount(list) > 0) {
            ListView_SetItemState(list, 0, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(list, 0, FALSE);
        }
    } else if (event == DBT_DEVICEARRIVAL) {
        SpeakW(Format(T("No audio CD in %ls"), DriveName(*d).c_str()));
    } else {
        SpeakW(T("Disc removed"));
    }
}

void HideDialog(HWND dlg) {
    ShowWindow(dlg, SW_HIDE);
    for (const DiscDrive& d : g_drives) mediaaccess::ReleaseDrive(d);
    if (g_hwnd) SetForegroundWindow(g_hwnd);
}

INT_PTR CALLBACK CdRipDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Windows asks the foreground window before offering AutoPlay for a disc
    // just inserted. While this window is in front, the CD is ours: refusing
    // it keeps Windows' own AutoPlay notification from speaking over
    // "CD inserted in drive E".
    static const UINT kQueryCancelAutoPlay = RegisterWindowMessageW(L"QueryCancelAutoPlay");
    if (msg == kQueryCancelAutoPlay && kQueryCancelAutoPlay != 0) {
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            InitTrackList(dlg);
            FillDriveCombo(dlg);
            RefreshDisc(dlg);
            return FALSE;   // focus is placed by ShowCdRipDialog

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CDRIP_DRIVE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) RefreshDisc(dlg);
                    return TRUE;
                case IDC_CDRIP_EJECT:
                    OnEject(dlg);
                    return TRUE;
                case IDCANCEL:
                    HideDialog(dlg);
                    return TRUE;
            }
            break;

        case WM_DEVICECHANGE:
            OnMediaChange(dlg, wParam, lParam);
            return TRUE;

        case WM_CLOSE:
            HideDialog(dlg);
            return TRUE;

        case WM_DESTROY:
            g_cdDialog = nullptr;
            break;
    }
    return FALSE;
}

} // namespace

void ShowCdRipDialog(HWND parent) {
    if (!mediaaccess::basscd::Get()) {
        MessageBoxW(parent, T("Audio CD support is missing (basscd.dll). Please reinstall MediaAccess."),
                    T("Extract audio CD"), MB_ICONWARNING);
        return;
    }
    g_drives = mediaaccess::EnumerateOpticalDrives();
    if (g_drives.empty()) {
        MessageBoxW(parent, T("No CD or DVD drive was found on this computer."),
                    T("Extract audio CD"), MB_ICONINFORMATION);
        return;
    }

    if (g_cdDialog) {
        // Reopened: drives may have changed (USB drive plugged in or out).
        FillDriveCombo(g_cdDialog);
        RefreshDisc(g_cdDialog);
    } else {
        g_cdDialog = CreateDialogW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CD_RIP),
                                   parent, CdRipDlgProc);
        if (!g_cdDialog) return;
    }
    // The title is set before the window shows, so it is said with the focus.
    ShowWindow(g_cdDialog, SW_SHOW);
    SetForegroundWindow(g_cdDialog);
    FocusFirstControl(g_cdDialog);
}

HWND GetCdRipDialog() {
    return g_cdDialog;
}
