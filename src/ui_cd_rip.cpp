// ui_cd_rip.cpp — "Extract audio CD" window (docs/PLAN_EXTRACTION_CD.md).
// Phase 1: choose the drive, list the tracks, eject. Phase 2: extract the
// ticked tracks to WAV or FLAC (cd_ripper), progress on Ctrl+I. Phase 3: album
// and track names (cd_metadata: cache, MusicBrainz, CD-Text, GnuDB), editable
// fields, F2 on a track, choice of the edition (Ctrl+M), new search (F5), the
// cover saved as folder.jpg, files named after the titles. Phase 4: MP3, Ogg
// Vorbis and Opus with a quality choice, tags and cover in the files, name
// templates with a preview, CUE and M3U8 (Options... dialog).
//
// Modeless like the YouTube window: closing only hides it, and the main
// message loop routes its keys through IsDialogMessageW (main.cpp).
//
// Speech: the window title carries what the screen reader cannot guess
// ("Extract audio CD: drive D, 12 tracks, 47 minutes"), so opening the window
// is said in one go by NVDA and JAWS (title, list, line). The program itself
// speaks only for events the reader cannot see: a disc inserted or removed,
// the tray opened or closed, the names found for the disc, the edges of a
// list ("First,", "Last,"), each track started, and the end of an extraction
// (with the download sound). Buttons are never renamed (Extract and Stop are
// two buttons): renaming the focused button would make the reader speak twice.

#include "ui_internal.h"
#include "mediaaccess/ui_cd_rip.h"
#include "mediaaccess/disc_drives.h"
#include "mediaaccess/basscd_loader.h"
#include "mediaaccess/cd_ripper.h"
#include "mediaaccess/cd_metadata.h"
#include "mediaaccess/notify_sounds.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/keymap.h"
#include <dbt.h>
#include <vector>
#include <cmath>
#include <memory>
#include <thread>

using mediaaccess::CdLookupResult;
using mediaaccess::CdReleaseMeta;
using mediaaccess::DiscContents;
using mediaaccess::DiscDrive;
using mediaaccess::DiscTrack;
using mediaaccess::DriveMediaType;

#define WM_CDMETA_RESULT (WM_APP + 0x310)   // wParam: lookup token, lParam: CdLookupResult*

namespace {

HWND                   g_cdDialog = nullptr;
// Set when the program itself opened the tray: Windows then reports the disc
// as removed, which must not be said a second time ("Tray open" already was).
bool                   g_trayOpenedByUs = false;
std::vector<DiscDrive> g_drives;
DiscContents           g_contents;       // disc in the selected drive
DiscContents           g_listContents;   // disc the track list shows
std::vector<int>       g_rowTrack;       // list row -> index in g_listContents.tracks

CdLookupResult g_lookup;                 // last lookup for the listed disc
CdReleaseMeta  g_meta;                   // names shown and edited (one entry per row)
int            g_lookupToken = 0;        // a newer lookup makes older results stale
bool           g_filling = false;        // fields being filled by the program, not typed
bool           g_metaEdited = false;     // the user changed a name: save it to the cache

// [CdRip] options (Options... dialog). Plan 4.6 default templates.
const wchar_t* kDefaultFolderTpl = L"%albumartist%\\%album% (%year%)";
const wchar_t* kDefaultFileTpl = L"%track% - %title%";
struct RipOptions {
    std::wstring folderTpl = kDefaultFolderTpl, fileTpl = kDefaultFileTpl;
    bool embedCover = true, saveFolderJpg = true, writeCue = true, writeM3u = true, ejectWhenDone = false;
};
RipOptions g_opts;

const DiscDrive* SelectedDrive(HWND dlg) {
    int sel = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_DRIVE, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)g_drives.size()) return nullptr;
    return &g_drives[sel];
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring GetText(HWND dlg, int id) {
    HWND h = GetDlgItem(dlg, id);
    int len = GetWindowTextLengthW(h);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, &s[0], len + 1);
    s.resize(len);
    return s;
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

void SetStatus(HWND dlg, const std::wstring& text) {
    SetDlgItemTextW(dlg, IDC_CDRIP_STATUS, text.c_str());
}

void UpdateTitleAndStatus(HWND dlg) {
    const DiscDrive* d = SelectedDrive(dlg);
    std::wstring summary = d ? DiscSummary(*d, g_contents) : std::wstring();
    std::wstring title = summary.empty() ? std::wstring(T("Extract audio CD"))
                                         : Format(T("Extract audio CD: %ls"), summary.c_str());
    SetWindowTextW(dlg, title.c_str());
    SetStatus(dlg, summary);
}

// Title of a row: the name found or typed, else "Track n".
std::wstring TrackTitle(int row) {
    if (row >= 0 && row < (int)g_meta.tracks.size() && !g_meta.tracks[row].title.empty()) {
        return g_meta.tracks[row].title;
    }
    if (row < 0 || row >= (int)g_rowTrack.size()) return std::wstring();
    return Format(T("Track %d"), g_listContents.tracks[g_rowTrack[row]].number);
}

// "01 - Title": the number stays in the name, so it is heard with the title.
std::wstring RowText(int row) {
    if (row < 0 || row >= (int)g_rowTrack.size()) return std::wstring();
    return Format(L"%02d - ", g_listContents.tracks[g_rowTrack[row]].number) + TrackTitle(row);
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

void RenameRow(HWND dlg, int row) {
    std::wstring text = RowText(row);
    ListView_SetItemText(GetDlgItem(dlg, IDC_CDRIP_TRACKS), row, 0, &text[0]);
}

int SelectedRow(HWND dlg) {
    return ListView_GetNextItem(GetDlgItem(dlg, IDC_CDRIP_TRACKS), -1, LVNI_SELECTED);
}

void FillTrackFields(HWND dlg) {
    int row = SelectedRow(dlg);
    bool ok = row >= 0 && row < (int)g_meta.tracks.size();
    g_filling = true;
    SetDlgItemTextW(dlg, IDC_CDRIP_TTITLE, ok ? TrackTitle(row).c_str() : L"");
    SetDlgItemTextW(dlg, IDC_CDRIP_TARTIST, ok ? g_meta.tracks[row].artist.c_str() : L"");
    g_filling = false;
}

void FillMetaFields(HWND dlg) {
    g_filling = true;
    SetDlgItemTextW(dlg, IDC_CDRIP_ALBUM, g_meta.album.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_ARTIST, g_meta.albumArtist.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_YEAR, g_meta.year.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_GENRE, g_meta.genre.c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_DISC, g_meta.tracks.empty() ? L"" : std::to_wstring(g_meta.discNumber).c_str());
    SetDlgItemTextW(dlg, IDC_CDRIP_DISCTOTAL, g_meta.tracks.empty() ? L"" : std::to_wstring(g_meta.discTotal).c_str());
    CheckDlgButton(dlg, IDC_CDRIP_COMPILATION, g_meta.compilation ? BST_CHECKED : BST_UNCHECKED);
    g_filling = false;
    FillTrackFields(dlg);
}

// Shows `meta` for the listed disc: fields, then the track names.
void ShowMeta(HWND dlg, const CdReleaseMeta& meta) {
    g_meta = meta;
    g_metaEdited = false;
    FillMetaFields(dlg);
    for (int row = 0; row < (int)g_rowTrack.size(); ++row) RenameRow(dlg, row);
}

void SaveMetaIfEdited() {
    if (g_metaEdited && !g_lookup.discId.empty()) {
        mediaaccess::SaveToCache(g_lookup.discId, g_meta);
        g_metaEdited = false;
    }
}

// Starts a lookup on a worker thread; WM_CDMETA_RESULT brings the answer.
void StartLookup(HWND dlg, bool useCache) {
    const DiscDrive* d = SelectedDrive(dlg);
    if (!d || g_listContents.type != DriveMediaType::AudioCD) return;
    if (mediaaccess::IsRipping()) {
        SpeakW(T("An extraction is running. Stop it first."));
        return;
    }
    int token = ++g_lookupToken;
    DiscDrive drive = *d;
    DiscContents contents = g_listContents;
    SetStatus(dlg, T("Looking up the disc..."));
    std::thread([dlg, token, drive, contents, useCache]() {
        CdLookupResult* r = new CdLookupResult(mediaaccess::LookupDisc(drive, contents, useCache));
        if (!IsWindow(dlg) || !PostMessageW(dlg, WM_CDMETA_RESULT, token, (LPARAM)r)) delete r;
    }).detach();
}

// True when the editions differ in what the user would see (track count or
// titles): then the choice is offered at once.
bool EditionsDiffer(const std::vector<CdReleaseMeta>& c) {
    for (size_t i = 1; i < c.size(); ++i) {
        if (c[i].tracks.size() != c[0].tracks.size()) return true;
        for (size_t t = 0; t < c[0].tracks.size(); ++t) {
            if (c[i].tracks[t].title != c[0].tracks[t].title) return true;
        }
    }
    return false;
}

std::wstring Headline(const CdReleaseMeta& m) {
    std::wstring s = m.album;
    if (!m.albumArtist.empty()) s += L", " + m.albumArtist;
    if (!m.year.empty()) s += L", " + m.year;
    return s;
}

bool ShowEditionDialog(HWND dlg);
std::wstring ShortcutHint(const char* actionId);
mediaaccess::RipFormat SelectedFormat(HWND dlg);
void SaveRipSettings(HWND dlg);
std::wstring ExpandTemplate(const std::wstring& tpl, const CdReleaseMeta& m, int row);
std::wstring FileTemplateFor(const CdReleaseMeta& m, const std::wstring& tpl);
std::wstring AlbumFolderName(const CdReleaseMeta& m, const std::wstring& tpl);

void OnLookupResult(HWND dlg, int token, CdLookupResult* r) {
    std::unique_ptr<CdLookupResult> owner(r);
    if (token != g_lookupToken) return;   // the disc changed meanwhile
    g_lookup = *r;
    UpdateTitleAndStatus(dlg);
    if (r->candidates.empty()) {
        ShowMeta(dlg, mediaaccess::DefaultRelease(g_listContents));
        SpeakW(T("Unknown disc. The tracks are named Track 1, Track 2. You can change them."));
        return;
    }
    const CdReleaseMeta& first = r->candidates[0];
    if (g_metaEdited) {
        // The user named tracks while the answer was on its way: keep their
        // names; the editions found stay one Ctrl+M away.
        std::wstring key = ShortcutHint("CDRIP_RELEASE");
        if (!key.empty()) key.erase(0, 1);
        SpeakW(Format(T("Names found, your changes are kept. %ls to use them"), key.c_str()));
        return;
    }
    if (r->candidates.size() > 1 && EditionsDiffer(r->candidates)) {
        ShowMeta(dlg, first);
        ShowEditionDialog(dlg);   // its title says what it is
        return;
    }
    const wchar_t* where = r->fromCache                    ? T("Found in MediaAccess's memory: %ls")
                         : first.source == L"CD-Text"      ? T("Found in the CD-Text: %ls")
                         : first.source == L"GnuDB"        ? T("Found on GnuDB: %ls")
                                                           : T("Found on MusicBrainz: %ls");
    std::wstring text = Format(where, Headline(first).c_str());
    if (r->candidates.size() > 1) {
        std::wstring key = ShortcutHint("CDRIP_RELEASE");
        if (!key.empty()) key.erase(0, 1);   // the hint starts with a tab
        text += L". " + Format(T("%d editions, the first one is chosen, %ls to change"),
                               (int)r->candidates.size(), key.c_str());
    }
    // Said before the names change: the line that has the focus is then
    // renamed, and the reader says its new name after this.
    SpeakW(text);
    ShowMeta(dlg, first);
}

// ---- Edition dialog (modal) ----

// A list view, like the track list: a LISTBOX here was read twice by NVDA
// when the dialog opened (focus, then selection).
LRESULT CALLBACK EditionListProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wParam == VK_UP || wParam == VK_DOWN)) {   // never a silent edge
        int count = ListView_GetItemCount(hwnd);
        int cur = ListView_GetNextItem(hwnd, -1, LVNI_FOCUSED);
        bool first = (wParam == VK_UP && cur == 0);
        bool last = (wParam == VK_DOWN && cur == count - 1);
        if (count > 0 && cur >= 0 && (first || last)) {
            wchar_t text[1024] = {};
            ListView_GetItemText(hwnd, cur, 0, text, _countof(text));
            SpeakW(std::wstring(first ? T("First,") : T("Last,")) + L" " + text);
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

int EditionSelected(HWND dlg) {
    return ListView_GetNextItem(GetDlgItem(dlg, IDC_CDREL_LIST), -1, LVNI_SELECTED);
}

INT_PTR CALLBACK EditionDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(dlg);
            HWND list = GetDlgItem(dlg, IDC_CDREL_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
            RECT rc;
            GetClientRect(list, &rc);
            LVCOLUMNW col = {};
            col.mask = LVCF_WIDTH;
            col.cx = rc.right - rc.left - GetSystemMetrics(SM_CXVSCROLL);
            ListView_InsertColumn(list, 0, &col);
            std::vector<std::wstring> lines;
            int pick = 0;
            for (size_t i = 0; i < g_lookup.candidates.size(); ++i) {
                lines.push_back(mediaaccess::DescribeRelease(g_lookup.candidates[i]));
                if (g_lookup.candidates[i].releaseMbid == g_meta.releaseMbid) pick = (int)i;
            }
            if (lines.empty()) lines.push_back(T("No edition found for this disc"));
            for (size_t i = 0; i < lines.size(); ++i) {
                LVITEMW item = {};
                item.mask = LVIF_TEXT;
                item.iItem = (int)i;
                item.pszText = &lines[i][0];
                ListView_InsertItem(list, &item);
            }
            ListView_SetItemState(list, pick, LVIS_FOCUSED | LVIS_SELECTED, LVIS_FOCUSED | LVIS_SELECTED);
            ListView_EnsureVisible(list, pick, FALSE);
            SetWindowSubclass(list, EditionListProc, 1, 0);
            EnableWindow(GetDlgItem(dlg, IDC_CDREL_SUBMIT), !g_lookup.discId.empty());
            EnableWindow(GetDlgItem(dlg, IDC_CDREL_OPEN_MB), !g_lookup.candidates.empty());
            // TRUE: Windows gives the focus to the list (first control) once,
            // with the dialog; an extra SetFocus made the line speak twice.
            return TRUE;
        }
        case WM_NOTIFY: {
            const NMHDR* nm = reinterpret_cast<const NMHDR*>(lParam);
            if (nm->idFrom == IDC_CDREL_LIST && nm->code == LVN_ITEMACTIVATE && !g_lookup.candidates.empty()) {
                EndDialog(dlg, IDOK + 100 + EditionSelected(dlg));   // Enter or double click
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            int sel = EditionSelected(dlg);
            switch (LOWORD(wParam)) {
                case IDOK:
                    EndDialog(dlg, g_lookup.candidates.empty() ? IDCANCEL : IDOK + 100 + sel);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
                case IDC_CDREL_SEARCH:
                    EndDialog(dlg, IDRETRY);
                    return TRUE;
                case IDC_CDREL_OPEN_MB:
                    if (sel >= 0 && sel < (int)g_lookup.candidates.size()) {
                        std::wstring url = L"https://musicbrainz.org/release/" + g_lookup.candidates[sel].releaseMbid;
                        ShellExecuteW(dlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                    return TRUE;
                case IDC_CDREL_SUBMIT: {
                    std::wstring url = mediaaccess::MusicBrainzSubmitUrl(g_lookup, (int)g_rowTrack.size());
                    ShellExecuteW(dlg, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                }
            }
            break;
        }
    }
    return FALSE;
}

// Returns true when the shown names changed.
bool ShowEditionDialog(HWND dlg) {
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CD_RELEASE), dlg, EditionDlgProc);
    if (r == IDRETRY) {
        StartLookup(dlg, false);
        return false;
    }
    int idx = (int)r - (IDOK + 100);
    if (idx >= 0 && idx < (int)g_lookup.candidates.size()) {
        ShowMeta(dlg, g_lookup.candidates[idx]);
        g_metaEdited = true;   // the choice is remembered for this disc
        SaveMetaIfEdited();
        return true;
    }
    return false;
}

void OnChooseEdition(HWND dlg) {
    if (g_listContents.type != DriveMediaType::AudioCD) {
        SpeakW(T("No audio CD in the drive"));
        return;
    }
    ShowEditionDialog(dlg);
}

// ---- Options dialog (modal) ----

void UpdatePreview(HWND opt) {
    CdReleaseMeta m = g_meta;
    if (m.album.empty()) { m.album = T("Album"); }
    if (m.albumArtist.empty()) { m.albumArtist = T("Artist"); }
    std::wstring folder = AlbumFolderName(m, GetText(opt, IDC_CDOPT_FOLDER));
    std::wstring file = ExpandTemplate(FileTemplateFor(m, GetText(opt, IDC_CDOPT_FILE)), m, 0);
    std::wstring text = folder + L"\\" + file + mediaaccess::RipFormatExtension(SelectedFormat(g_cdDialog));
    SetDlgItemTextW(opt, IDC_CDOPT_PREVIEW, text.c_str());
}

void ShowOptionsChecks(HWND opt, const RipOptions& o) {
    SetDlgItemTextW(opt, IDC_CDOPT_FOLDER, o.folderTpl.c_str());
    SetDlgItemTextW(opt, IDC_CDOPT_FILE, o.fileTpl.c_str());
    CheckDlgButton(opt, IDC_CDOPT_EMBED, o.embedCover ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(opt, IDC_CDOPT_FOLDERJPG, o.saveFolderJpg ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(opt, IDC_CDOPT_CUE, o.writeCue ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(opt, IDC_CDOPT_M3U, o.writeM3u ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(opt, IDC_CDOPT_EJECT, o.ejectWhenDone ? BST_CHECKED : BST_UNCHECKED);
}

INT_PTR CALLBACK OptionsDlgProc(HWND opt, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(opt);
            SetDlgItemTextW(opt, IDC_CDOPT_VARIABLES, T("%albumartist%, %artist%, %album%, %year%, %track%, "
                "%tracktotal%, %title%, %disc%, %disctotal%, %genre%, %label%. A backslash makes a sub-folder."));
            ShowOptionsChecks(opt, g_opts);
            UpdatePreview(opt);
            return TRUE;   // the focus goes to the first field, once
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_CDOPT_FOLDER:
                case IDC_CDOPT_FILE:
                    if (HIWORD(wParam) == EN_CHANGE) UpdatePreview(opt);
                    return TRUE;
                case IDC_CDOPT_DEFAULTS:
                    ShowOptionsChecks(opt, RipOptions());
                    UpdatePreview(opt);
                    return TRUE;
                case IDOK: {
                    RipOptions o;
                    o.folderTpl = GetText(opt, IDC_CDOPT_FOLDER);
                    o.fileTpl = GetText(opt, IDC_CDOPT_FILE);
                    if (o.folderTpl.empty()) o.folderTpl = kDefaultFolderTpl;
                    if (o.fileTpl.empty()) o.fileTpl = kDefaultFileTpl;
                    o.embedCover = IsDlgButtonChecked(opt, IDC_CDOPT_EMBED) == BST_CHECKED;
                    o.saveFolderJpg = IsDlgButtonChecked(opt, IDC_CDOPT_FOLDERJPG) == BST_CHECKED;
                    o.writeCue = IsDlgButtonChecked(opt, IDC_CDOPT_CUE) == BST_CHECKED;
                    o.writeM3u = IsDlgButtonChecked(opt, IDC_CDOPT_M3U) == BST_CHECKED;
                    o.ejectWhenDone = IsDlgButtonChecked(opt, IDC_CDOPT_EJECT) == BST_CHECKED;
                    g_opts = o;
                    EndDialog(opt, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(opt, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void OnOptions(HWND dlg) {
    if (DialogBoxW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_CD_RIP_OPTIONS), dlg, OptionsDlgProc) == IDOK) {
        SaveRipSettings(dlg);
    }
}

// ---- Fields ----

void OnFieldChanged(HWND dlg, int id) {
    if (g_filling) return;
    std::wstring v = GetText(dlg, id);
    int row = SelectedRow(dlg);
    switch (id) {
        case IDC_CDRIP_ALBUM:     g_meta.album = v; break;
        case IDC_CDRIP_ARTIST:    g_meta.albumArtist = v; break;
        case IDC_CDRIP_YEAR:      g_meta.year = v; break;
        case IDC_CDRIP_GENRE:     g_meta.genre = v; break;
        case IDC_CDRIP_DISC:      g_meta.discNumber = _wtoi(v.c_str()); break;
        case IDC_CDRIP_DISCTOTAL: g_meta.discTotal = _wtoi(v.c_str()); break;
        case IDC_CDRIP_TTITLE:
            if (row < 0 || row >= (int)g_meta.tracks.size()) return;
            g_meta.tracks[row].title = v;
            RenameRow(dlg, row);   // not the focused control: silent
            break;
        case IDC_CDRIP_TARTIST:
            if (row < 0 || row >= (int)g_meta.tracks.size()) return;
            g_meta.tracks[row].artist = v;
            break;
        default:
            return;
    }
    g_metaEdited = true;
}

// F2 on a track: the edit box holds the title alone, without "01 - ".
LRESULT CALLBACK LabelEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    // Enter and Escape belong to the edit box, not to the window's buttons.
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wParam, lParam);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

bool OnListNotify(HWND dlg, NMHDR* nm) {
    if (nm->idFrom != IDC_CDRIP_TRACKS) return false;
    switch (nm->code) {
        case LVN_KEYDOWN: {
            // A list view never starts editing on F2 by itself.
            const NMLVKEYDOWN* kd = reinterpret_cast<const NMLVKEYDOWN*>(nm);
            if (kd->wVKey == VK_F2) {
                int row = ListView_GetNextItem(nm->hwndFrom, -1, LVNI_FOCUSED);
                if (row >= 0) ListView_EditLabel(nm->hwndFrom, row);
            }
            return false;
        }
        case LVN_ITEMCHANGED: {
            const NMLISTVIEW* lv = reinterpret_cast<const NMLISTVIEW*>(nm);
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) {
                FillTrackFields(dlg);
            }
            return false;
        }
        case LVN_BEGINLABELEDIT: {
            const NMLVDISPINFOW* di = reinterpret_cast<const NMLVDISPINFOW*>(nm);
            if (di->item.iItem < 0 || di->item.iItem >= (int)g_meta.tracks.size()) {
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);   // nothing to edit yet
                return true;
            }
            HWND edit = ListView_GetEditControl(nm->hwndFrom);
            if (edit) {
                SetWindowTextW(edit, TrackTitle(di->item.iItem).c_str());
                SendMessageW(edit, EM_SETSEL, 0, -1);
                SetWindowSubclass(edit, LabelEditProc, 1, 0);
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, FALSE);
            return true;
        }
        case LVN_ENDLABELEDIT: {
            const NMLVDISPINFOW* di = reinterpret_cast<const NMLVDISPINFOW*>(nm);
            int row = di->item.iItem;
            if (di->item.pszText && row >= 0 && row < (int)g_meta.tracks.size()) {
                g_meta.tracks[row].title = di->item.pszText;
                g_metaEdited = true;
                RenameRow(dlg, row);       // "01 - New title", said by the reader
                FillTrackFields(dlg);
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, FALSE);   // the row text is set above
            return true;
        }
    }
    return false;
}

// ---- Disc and drives ----

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
// disc is read again (the window reopened), so the ticked tracks and the names
// are kept. Another disc starts a new lookup.
void RefreshDisc(HWND dlg) {
    const DiscDrive* d = SelectedDrive(dlg);
    g_contents = d ? mediaaccess::ReadDiscContents(*d) : DiscContents();
    DiscContents shown = (g_contents.type == DriveMediaType::AudioCD) ? g_contents : DiscContents();
    bool changed = !SameDisc(shown, g_listContents);
    if (changed) {
        SaveMetaIfEdited();
        ++g_lookupToken;   // results for the previous disc are now stale
        g_lookup = CdLookupResult();
        g_listContents = shown;
        g_meta = mediaaccess::DefaultRelease(g_listContents);
        FillTrackList(dlg);
        if (g_listContents.tracks.empty()) g_meta = CdReleaseMeta();
        FillMetaFields(dlg);
    }
    UpdateTitleAndStatus(dlg);
    EnableWindow(GetDlgItem(dlg, IDC_CDRIP_EJECT), d && d->canOpen);
    if (changed) StartLookup(dlg, true);
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
        RefreshDisc(dlg);   // the list empties: the disc is gone
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

std::wstring IniString(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[MAX_PATH * 4] = {};
    GetPrivateProfileStringW(L"CdRip", key, def, buf, _countof(buf), g_configPath.c_str());
    return buf;
}

bool IniBool(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(L"CdRip", key, def ? 1 : 0, g_configPath.c_str()) != 0;
}

void IniWrite(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"CdRip", key, v.c_str(), g_configPath.c_str());
}

const wchar_t* QualityKey(mediaaccess::RipFormat f) {
    switch (f) {
        case mediaaccess::RipFormat::FLAC: return L"Quality_FLAC";
        case mediaaccess::RipFormat::MP3:  return L"Quality_MP3";
        case mediaaccess::RipFormat::OGG:  return L"Quality_OGG";
        case mediaaccess::RipFormat::OPUS: return L"Quality_OPUS";
        default:                           return nullptr;
    }
}

// The format list is WAV, FLAC, MP3, Ogg Vorbis[, Opus]: its index is the
// RipFormat value (Opus is last, so it can be left out).
mediaaccess::RipFormat SelectedFormat(HWND dlg) {
    int sel = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_GETCURSEL, 0, 0);
    return (sel >= 0 && sel <= 4) ? (mediaaccess::RipFormat)sel : mediaaccess::RipFormat::FLAC;
}

void FillQualityCombo(HWND dlg) {
    mediaaccess::RipFormat f = SelectedFormat(dlg);
    HWND combo = GetDlgItem(dlg, IDC_CDRIP_QUALITY);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int n = mediaaccess::RipQualityCount(f);
    for (int i = 0; i < n; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)mediaaccess::RipQualityLabel(f, i).c_str());
    }
    const wchar_t* key = QualityKey(f);
    int q = key ? GetPrivateProfileIntW(L"CdRip", key, mediaaccess::RipDefaultQuality(f), g_configPath.c_str()) : 0;
    SendMessageW(combo, CB_SETCURSEL, (q >= 0 && q < n) ? q : 0, 0);
}

void LoadRipSettings(HWND dlg) {
    int count = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_GETCOUNT, 0, 0);
    int format = GetPrivateProfileIntW(L"CdRip", L"Format", (int)mediaaccess::RipFormat::FLAC, g_configPath.c_str());
    SendDlgItemMessageW(dlg, IDC_CDRIP_FORMAT, CB_SETCURSEL, (format >= 0 && format < count) ? format : 1, 0);
    FillQualityCombo(dlg);
    std::wstring root = IniString(L"OutputRoot", L"");
    SetDlgItemTextW(dlg, IDC_CDRIP_FOLDER, root.empty() ? DefaultOutputRoot().c_str() : root.c_str());
    g_opts.folderTpl = IniString(L"FolderTemplate", kDefaultFolderTpl);
    g_opts.fileTpl = IniString(L"FileTemplate", kDefaultFileTpl);
    g_opts.embedCover = IniBool(L"EmbedCover", true);
    g_opts.saveFolderJpg = IniBool(L"SaveFolderJpg", true);
    g_opts.writeCue = IniBool(L"WriteCue", true);
    g_opts.writeM3u = IniBool(L"WriteM3u", true);
    g_opts.ejectWhenDone = IniBool(L"EjectWhenDone", false);
}

void SaveRipSettings(HWND dlg) {
    mediaaccess::RipFormat f = SelectedFormat(dlg);
    IniWrite(L"Format", std::to_wstring((int)f));
    const wchar_t* key = QualityKey(f);
    if (key) IniWrite(key, std::to_wstring((int)SendDlgItemMessageW(dlg, IDC_CDRIP_QUALITY, CB_GETCURSEL, 0, 0)));
    IniWrite(L"OutputRoot", GetText(dlg, IDC_CDRIP_FOLDER));
    IniWrite(L"FolderTemplate", g_opts.folderTpl);
    IniWrite(L"FileTemplate", g_opts.fileTpl);
    IniWrite(L"EmbedCover", g_opts.embedCover ? L"1" : L"0");
    IniWrite(L"SaveFolderJpg", g_opts.saveFolderJpg ? L"1" : L"0");
    IniWrite(L"WriteCue", g_opts.writeCue ? L"1" : L"0");
    IniWrite(L"WriteM3u", g_opts.writeM3u ? L"1" : L"0");
    IniWrite(L"EjectWhenDone", g_opts.ejectWhenDone ? L"1" : L"0");
}

void InitRipControls(HWND dlg) {
    HWND combo = GetDlgItem(dlg, IDC_CDRIP_FORMAT);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)T("WAV (uncompressed)"));
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)T("FLAC (lossless)"));
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"MP3");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Ogg Vorbis");
    if (mediaaccess::IsOpusAvailable()) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Opus");
    SetWindowSubclass(combo, DriveComboProc, 2, 0);   // same "First," / "Last," rule
    SetWindowSubclass(GetDlgItem(dlg, IDC_CDRIP_QUALITY), DriveComboProc, 3, 0);
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

// ---- File names ----

// One folder or file name: no character Windows refuses, no control
// character, no trailing space or dot, no reserved device name, 120 at most.
std::wstring CleanName(const std::wstring& in) {
    std::wstring s;
    for (wchar_t c : in) {
        if (c < 32) continue;
        s += wcschr(L"\\/:*?\"<>|", c) ? L'-' : c;
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    if (s.size() > 120) s.resize(120);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    static const wchar_t* reserved[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3",
        L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4",
        L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
    std::wstring stem = s.substr(0, s.find(L'.'));
    for (const wchar_t* r : reserved) {
        if (_wcsicmp(stem.c_str(), r) == 0) { s += L"_"; break; }
    }
    return s;
}

// Name templates (plan 4.6): %albumartist% %artist% %album% %year% %track%
// %tracktotal% %title% %disc% %disctotal% %genre% %label%. Each part between
// backslashes is cleaned on its own; an empty "()" left by a missing value
// goes away, and so does a part left empty.
std::wstring ExpandTemplate(const std::wstring& tpl, const CdReleaseMeta& m, int row) {
    std::wstring title, artist;
    int number = 0;
    if (row >= 0 && row < (int)g_rowTrack.size()) {
        number = g_listContents.tracks[g_rowTrack[row]].number;
        title = TrackTitle(row);
        if (row < (int)m.tracks.size()) artist = m.tracks[row].artist;
    }
    if (artist.empty()) artist = m.albumArtist;
    const std::pair<const wchar_t*, std::wstring> vars[] = {
        { L"%albumartist%", m.albumArtist }, { L"%artist%", artist }, { L"%album%", m.album },
        { L"%year%", m.year }, { L"%track%", Format(L"%02d", number) },
        { L"%tracktotal%", Format(L"%02d", (int)g_rowTrack.size()) }, { L"%title%", title },
        { L"%disc%", std::to_wstring(m.discNumber) }, { L"%disctotal%", std::to_wstring(m.discTotal) },
        { L"%genre%", m.genre }, { L"%label%", m.label },
    };
    std::wstring out, part;
    auto flush = [&]() {
        for (const wchar_t* empty : { L"()", L"[]" }) {
            size_t at;
            while ((at = part.find(empty)) != std::wstring::npos) part.erase(at, 2);
        }
        std::wstring clean = CleanName(part);
        if (!clean.empty()) out += (out.empty() ? L"" : L"\\") + clean;
        part.clear();
    };
    for (size_t i = 0; i < tpl.size();) {
        if (tpl[i] == L'\\' || tpl[i] == L'/') { flush(); ++i; continue; }
        bool matched = false;
        for (const auto& v : vars) {
            size_t len = wcslen(v.first);
            if (_wcsnicmp(tpl.c_str() + i, v.first, len) == 0) {
                for (wchar_t c : v.second) part += (c == L'\\' || c == L'/') ? L'-' : c;
                i += len;
                matched = true;
                break;
            }
        }
        if (!matched) part += tpl[i++];
    }
    flush();
    return out;
}

// The default file template becomes "1-01 - Title" for a multi-disc set and
// "01 - Artist - Title" for a compilation (plan 4.6); a template the user
// wrote is used as it is.
std::wstring FileTemplateFor(const CdReleaseMeta& m, const std::wstring& tpl) {
    if (tpl != kDefaultFileTpl) return tpl;
    if (m.discTotal > 1) return L"%disc%-%track% - %title%";
    if (m.compilation) return L"%track% - %artist% - %title%";
    return tpl;
}

std::wstring AlbumFolderName(const CdReleaseMeta& m, const std::wstring& tpl) {
    std::wstring rel = (m.album.empty() && m.albumArtist.empty()) ? std::wstring() : ExpandTemplate(tpl, m, -1);
    if (rel.empty()) {   // nothing known about the disc: a dated folder
        SYSTEMTIME st;
        GetLocalTime(&st);
        rel = Format(T("Audio CD %04d-%02d-%02d %02dh%02d"), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    }
    return rel;
}

std::wstring TrackFileName(int row) {
    std::wstring name = ExpandTemplate(FileTemplateFor(g_meta, g_opts.fileTpl), g_meta, row);
    return name.empty() ? Format(L"%02d", g_listContents.tracks[g_rowTrack[row]].number) : name;
}

// ---- Extraction ----

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
        mediaaccess::RipTrack rt = { t.index, t.number, t.seconds, TrackFileName(row) };
        rt.title = TrackTitle(row);
        if (row < (int)g_meta.tracks.size()) {
            rt.artist = g_meta.tracks[row].artist;
            rt.isrc = g_meta.tracks[row].isrc;
            rt.recordingMbid = g_meta.tracks[row].recordingMbid;
        }
        if (rt.artist.empty()) rt.artist = g_meta.albumArtist;
        job.tracks.push_back(rt);
    }
    if (job.tracks.empty()) {
        SpeakW(T("No track is ticked"));
        return;
    }
    job.format = SelectedFormat(dlg);
    job.quality = (int)SendDlgItemMessageW(dlg, IDC_CDRIP_QUALITY, CB_GETCURSEL, 0, 0);
    std::wstring rootDir = GetText(dlg, IDC_CDRIP_FOLDER);
    if (rootDir.empty()) rootDir = DefaultOutputRoot();
    while (!rootDir.empty() && (rootDir.back() == L'\\' || rootDir.back() == L'/')) rootDir.pop_back();
    std::wstring albumRel = AlbumFolderName(g_meta, g_opts.folderTpl);
    job.folder = rootDir + L"\\" + albumRel;
    job.meta = g_meta;
    job.trackTotal = (int)g_rowTrack.size();
    job.embedCover = g_opts.embedCover;
    job.saveFolderJpg = g_opts.saveFolderJpg;
    job.writeCue = g_opts.writeCue;
    job.writeM3u = g_opts.writeM3u;
    job.listName = CleanName(g_meta.album.empty() ? albumRel.substr(albumRel.find_last_of(L'\\') + 1) : g_meta.album);
    SaveRipSettings(dlg);

    // Never replace files without asking.
    const wchar_t* ext = mediaaccess::RipFormatExtension(job.format);
    int existing = 0;
    for (const mediaaccess::RipTrack& t : job.tracks) {
        if (GetFileAttributesW((job.folder + L"\\" + t.fileName + ext).c_str()) != INVALID_FILE_ATTRIBUTES) ++existing;
    }
    if (existing > 0) {
        std::wstring q = Format(T("%d of these files already exist in %ls. Replace them?"),
                                existing, job.folder.c_str());
        if (MessageBoxW(dlg, q.c_str(), T("Extract audio CD"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }

    // The names used for the files are the ones remembered for this disc.
    g_metaEdited = true;
    SaveMetaIfEdited();

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
    std::wstring name = Format(T("Track %d"), number);
    for (int row = 0; row < (int)g_rowTrack.size(); ++row) {
        if (g_listContents.tracks[g_rowTrack[row]].number == number) { name = TrackTitle(row); break; }
    }
    std::wstring text = Format(T("Track %d of %d: %ls"), pos, count, name.c_str());
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
    struct Entry { int cmd; const char* key; const char* action; };
    const Entry entries[] = {
        { IDM_CDRIP_START,    "Extract / Stop",          "CDRIP_START" },
        { IDM_CDRIP_PROGRESS, "Announce the progress",   "CDRIP_PROGRESS" },
        { IDM_CDRIP_REFRESH,  "Look the disc up again",  "CDRIP_REFRESH" },
        { IDM_CDRIP_RELEASE,  "Choose another edition",  "CDRIP_RELEASE" },
    };
    HMENU menu = CreatePopupMenu();
    for (const Entry& e : entries) {
        std::wstring text = std::wstring(T(e.key)) + ShortcutHint(e.action);
        AppendMenuW(menu, MF_STRING, e.cmd, text.c_str());
    }
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, dlg, nullptr);
    DestroyMenu(menu);
    if (cmd) SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

void HideDialog(HWND dlg) {
    ShowWindow(dlg, SW_HIDE);
    SaveRipSettings(dlg);
    SaveMetaIfEdited();
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
                case IDC_CDRIP_EDITIONS:
                case IDM_CDRIP_RELEASE:
                    OnChooseEdition(dlg);
                    return TRUE;
                case IDM_CDRIP_REFRESH:
                    StartLookup(dlg, false);
                    return TRUE;
                case IDC_CDRIP_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        FillQualityCombo(dlg);   // not focused: silent
                        SaveRipSettings(dlg);
                    }
                    return TRUE;
                case IDC_CDRIP_QUALITY:
                    if (HIWORD(wParam) == CBN_SELCHANGE) SaveRipSettings(dlg);
                    return TRUE;
                case IDC_CDRIP_OPTIONS:
                    OnOptions(dlg);
                    return TRUE;
                case IDC_CDRIP_ALBUM: case IDC_CDRIP_ARTIST: case IDC_CDRIP_YEAR:
                case IDC_CDRIP_GENRE: case IDC_CDRIP_DISC: case IDC_CDRIP_DISCTOTAL:
                case IDC_CDRIP_TTITLE: case IDC_CDRIP_TARTIST:
                    if (HIWORD(wParam) == EN_CHANGE) OnFieldChanged(dlg, LOWORD(wParam));
                    return TRUE;
                case IDC_CDRIP_COMPILATION:
                    if (!g_filling) {
                        g_meta.compilation = IsDlgButtonChecked(dlg, IDC_CDRIP_COMPILATION) == BST_CHECKED;
                        g_metaEdited = true;
                    }
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

        case WM_NOTIFY:
            return OnListNotify(dlg, reinterpret_cast<NMHDR*>(lParam)) ? TRUE : FALSE;

        case WM_DEVICECHANGE:
            OnMediaChange(dlg, wParam, lParam);
            return TRUE;

        case WM_CONTEXTMENU:
            if ((HWND)wParam == GetDlgItem(dlg, IDC_CDRIP_TRACKS)) {
                ShowTrackMenu(dlg, lParam);
                return TRUE;
            }
            break;

        case WM_CDMETA_RESULT:
            OnLookupResult(dlg, (int)wParam, reinterpret_cast<CdLookupResult*>(lParam));
            return TRUE;

        case WM_CDRIP_TRACK_START:
            OnTrackStart(dlg, (int)wParam, (int)LOWORD(lParam), (int)HIWORD(lParam));
            return TRUE;
        case WM_CDRIP_DONE: {
            OnRipEnded(dlg, ((int)wParam == 1) ? std::wstring(T("Extraction done: 1 track"))
                                               : Format(T("Extraction done: %d tracks"), (int)wParam), true);
            const DiscDrive* d = SelectedDrive(dlg);
            bool open = false;
            if (g_opts.ejectWhenDone && d && !mediaaccess::IsDriveTrayOpen(*d) &&
                mediaaccess::ToggleDriveTray(*d, &open) && open) {
                g_trayOpenedByUs = true;   // "Disc removed" is not said on top of the end
                RefreshDisc(dlg);
            }
            return TRUE;
        }
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
