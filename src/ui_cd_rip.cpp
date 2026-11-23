// ui_cd_rip.cpp — "Extract audio CD" window (docs/PLAN_EXTRACTION_CD.md).
// Phase 1: choose the drive, list the tracks, eject. Phase 2: extract the
// ticked tracks to WAV or FLAC (cd_ripper), progress on Ctrl+I.
//
// Modeless like the YouTube window: closing only hides it, and the main
// message loop routes its keys through IsDialogMessageW (main.cpp).
//
// Speech: the window title carries what the screen reader cannot guess
// ("Extract audio CD: drive D, 12 tracks, 47 minutes"), so opening the window
// is said in one go by NVDA and JAWS (title, list, line). The program itself
// speaks only for events the reader cannot see: a disc inserted or removed,
// the tray opened or closed, the edges of a list ("First,", "Last,"), each
// track started, and the end of an extraction (with the download sound).
// Buttons are never renamed (Extract and Stop are two buttons): renaming the
// focused button would make the reader speak a second time.

#include "ui_internal.h"
#include "mediaaccess/ui_cd_rip.h"
#include "mediaaccess/disc_drives.h"
#include "mediaaccess/basscd_loader.h"
#include "mediaaccess/cd_ripper.h"
#include "mediaaccess/notify_sounds.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/keymap.h"
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
    if (mediaaccess::IsRipping()) {
        SpeakW(T("An extraction is running. Stop it first."));
        return;
    }
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

// ---- Settings ([CdRip] in MediaAccess.ini) ----

std::wstring DefaultOutputRoot() {
    wchar_t music[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, music))) return music;
    return L"C:\\";
}

void LoadRipSettings(HWND dlg) {
    int format = GetPrivateProfileIntW(L"CdRip", L"Format", (int)mediaaccess::RipFormat::FLAC, g_configPath.c_str());
    SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_SETCURSEL, (format == 0) ? 0 : 1, 0);
    wchar_t root[MAX_PATH * 4] = {};
    GetPrivateProfileStringW(L"CdRip", L"OutputRoot", L"", root, _countof(root), g_configPath.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, root[0] ? root : DefaultOutputRoot().c_str());
}

void SaveRipSettings(HWND dlg) {
    int format = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_GETCURSEL, 0, 0);
    WritePrivateProfileStringW(L"CdRip", L"Format", format == 0 ? L"0" : L"1", g_configPath.c_str());
    wchar_t root[MAX_PATH * 4] = {};
    GetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, root, _countof(root));
    WritePrivateProfileStringW(L"CdRip", L"OutputRoot", root, g_configPath.c_str());
}

void InitRipControls(HWND dlg) {
    HWND combo = GetDlgItem(dlg, IDC_CDRIP_FORMAT);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)T("WAV (uncompressed)"));
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)T("FLAC (lossless)"));
    SetWindowSubclass(combo, DriveComboProc, 2, 0);   // same "First," / "Last," rule
    LoadRipSettings(dlg);
}

void OnBrowse(HWND dlg) {
    wchar_t cur[MAX_PATH * 4] = {};
    GetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, cur, _countof(cur));
    std::wstring chosen = PickFolderModern(cur, T("Choose the output folder"));
    if (!chosen.empty()) {
        SetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, chosen.c_str());
        SaveRipSettings(dlg);
    }
}

// ---- Extraction ----

void SetStatus(HWND dlg, const std::wstring& text) {
    SetDlgItemTextW(dlg, IDC_CDRIP_STATUS, text.c_str());
}

void OnStart(HWND dlg) {
    if (mediaaccess::IsRipping()) {
        SpeakW(T("An extraction is already running"));
        return;
    }
    const DiscDrive* d = SelectedDrive(dlg);
    if (!d || g_contents.type != DriveMediaType::AudioCD) {
        SpeakW(T("No audio CD in the drive"));
        return;
    }
    HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
    mediaaccess::RipJob job;
    job.drive = *d;
    for (int row = 0; row < (int)g_rowTrack.size(); ++row) {
        if (!ListView_GetCheckState(list, row)) continue;
        const DiscTrack& t = g_listContents.tracks[g_rowTrack[row]];
        job.tracks.push_back({ t.index, t.number, t.seconds, RowText(row) });
    }
    if (job.tracks.empty()) {
        SpeakW(T("No track is ticked"));
        return;
    }
    int format = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_GETCURSEL, 0, 0);
    job.format = (format == 0) ? mediaaccess::RipFormat::WAV : mediaaccess::RipFormat::FLAC;
    wchar_t root[MAX_PATH * 4] = {};
    GetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, root, _countof(root));
    std::wstring rootDir = root[0] ? root : DefaultOutputRoot();
    while (!rootDir.empty() && (rootDir.back() == L'\\' || rootDir.back() == L'/')) rootDir.pop_back();
    // Phase 2 has no album name yet: one dated folder per extraction.
    SYSTEMTIME st;
    GetLocalTime(&st);
    job.folder = rootDir + L"\\" + Format(T("Audio CD %04d-%02d-%02d %02dh%02d"),
                                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    SaveRipSettings(dlg);

    // BASSCD keeps one stream per drive: a CD track playing from this drive
    // would be cut by the extraction (and would cut it). Stop it first.
    if (g_currentTrack >= 0 && g_currentTrack < (int)g_playlist.size()) {
        const std::wstring& cur = g_playlist[g_currentTrack];
        if (mediaaccess::IsCdaFile(cur.c_str()) && towupper(cur[0]) == d->letter) Stop();
    }
    if (!mediaaccess::StartRip(job, dlg)) {
        SpeakW(T("An extraction is already running"));
        return;
    }
    SetStatus(dlg, Format(T("Extracting to %ls"), job.folder.c_str()));
}

void OnStop() {
    if (!mediaaccess::IsRipping()) {
        SpeakW(T("No extraction is running"));
        return;
    }
    mediaaccess::CancelRip();   // WM_CDRIP_CANCELLED says how many were written
}

void OnProgress() {
    if (!mediaaccess::IsRipping()) {
        SpeakW(T("No extraction is running"));
        return;
    }
    mediaaccess::RipProgress p = mediaaccess::GetRipProgress();
    std::wstring s = Format(T("Track %d of %d, %d %%, total %d %%"), p.trackPos, p.trackCount,
                            (int)(p.trackFraction * 100.0), (int)(p.totalFraction * 100.0));
    if (p.secondsLeft >= 0) {
        int minutes = (int)(p.secondsLeft / 60.0 + 0.5);
        s += L", ";
        s += (minutes < 1) ? std::wstring(T("less than a minute left"))
                           : Format(T("about %d minutes left"), minutes);
    }
    SpeakW(s);
}

void OnTrackStart(HWND dlg, int pos, int count, int number) {
    std::wstring text = Format(T("Track %d of %d: %ls"), pos, count, Format(T("Track %d"), number).c_str());
    SetStatus(dlg, text);
    SpeakW(text);
}

void OnRipEnded(HWND dlg, const std::wstring& text, bool success) {
    SetStatus(dlg, text);
    SpeakW(text);
    PlayDownloadSound(success);
}

// Applications key / Shift+F10 on the track list: the window's actions with
// their shortcuts, named like the actions (house rule).
std::wstring ShortcutHint(const char* actionId) {
    const std::vector<mediaaccess::Shortcut>* keys = mediaaccess::GetActiveKeyMap().GetShortcuts(actionId);
    if (!keys || keys->empty()) return std::wstring();
    std::string text = mediaaccess::ShortcutToDisplay(keys->front());
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n);
    return w.empty() ? w : L"\t" + w;
}

void ShowTrackMenu(HWND dlg, LPARAM lParam) {
    HWND list = GetDlgItem(dlg, IDC_CDRIP_TRACKS);
    POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
    if (lParam == (LPARAM)-1) {   // from the keyboard: at the focused line
        RECT rc = {};
        int cur = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
        if (cur < 0 || !ListView_GetItemRect(list, cur, &rc, LVIR_LABEL)) GetClientRect(list, &rc);
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(list, &pt);
    }
    HMENU menu = CreatePopupMenu();
    std::wstring start = std::wstring(T("Extract / Stop")) + ShortcutHint("CDRIP_START");
    std::wstring progress = std::wstring(T("Announce the progress")) + ShortcutHint("CDRIP_PROGRESS");
    AppendMenuW(menu, MF_STRING, IDM_CDRIP_START, start.c_str());
    AppendMenuW(menu, MF_STRING, IDM_CDRIP_PROGRESS, progress.c_str());
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, dlg, nullptr);
    DestroyMenu(menu);
    if (cmd) SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

void HideDialog(HWND dlg) {
    ShowWindow(dlg, SW_HIDE);
    SaveRipSettings(dlg);
    // An extraction keeps running hidden (it releases its drive at the end).
    if (!mediaaccess::IsRipping()) {
        for (const DiscDrive& d : g_drives) mediaaccess::ReleaseDrive(d);
    }
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
            InitRipControls(dlg);
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
                case IDC_CDRIP_START:
                    OnStart(dlg);
                    return TRUE;
                case IDC_CDRIP_STOP:
                    OnStop();
                    return TRUE;
                case IDC_CDRIP_BROWSE:
                    OnBrowse(dlg);
                    return TRUE;
                case IDC_CDRIP_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) SaveRipSettings(dlg);
                    return TRUE;
                case IDM_CDRIP_START:   // Ctrl+R: extract, or stop when running
                    if (mediaaccess::IsRipping()) OnStop(); else OnStart(dlg);
                    return TRUE;
                case IDM_CDRIP_PROGRESS:
                    OnProgress();
                    return TRUE;
                case IDCANCEL:
                    HideDialog(dlg);
                    return TRUE;
            }
            break;

        case WM_DEVICECHANGE:
            OnMediaChange(dlg, wParam, lParam);
            return TRUE;

        case WM_CONTEXTMENU:
            if ((HWND)wParam == GetDlgItem(dlg, IDC_CDRIP_TRACKS)) {
                ShowTrackMenu(dlg, lParam);
                return TRUE;
            }
            break;

        case WM_CDRIP_TRACK_START:
            OnTrackStart(dlg, (int)wParam, (int)LOWORD(lParam), (int)HIWORD(lParam));
            return TRUE;
        case WM_CDRIP_DONE:
            OnRipEnded(dlg, ((int)wParam == 1) ? std::wstring(T("Extraction done: 1 track"))
                                               : Format(T("Extraction done: %d tracks"), (int)wParam), true);
            return TRUE;
        case WM_CDRIP_CANCELLED:
            OnRipEnded(dlg, ((int)wParam == 0) ? std::wstring(T("Extraction stopped, no track written"))
                                               : Format(T("Extraction stopped, %d tracks written"), (int)wParam), false);
            return TRUE;
        case WM_CDRIP_ERROR: {
            std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
            if (text) {
                OnRipEnded(dlg, *text, false);
                delete text;
            }
            return TRUE;
        }

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

bool CdRipTryShortcut(const MSG& msg) {
    if (!g_cdDialog || (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN)) return false;
    if (msg.hwnd != g_cdDialog && !IsChild(g_cdDialog, msg.hwnd)) return false;
    mediaaccess::Shortcut sc;
    sc.vk    = (UINT)msg.wParam;
    sc.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    sc.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    sc.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    sc.win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) || ((GetKeyState(VK_RWIN) & 0x8000) != 0);
    if (!mediaaccess::IsPlaylistShortcutAllowed(sc)) return false;
    int cmd = mediaaccess::GetActiveKeyMap().FindCommandFor(sc, mediaaccess::ActionCategory::CdRip);
    if (cmd == 0) return false;
    SendMessageW(g_cdDialog, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    return true;   // not translated: no stray Ctrl+letter character in a field
}
