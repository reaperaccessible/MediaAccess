#include "ui_internal.h"
#include "mediaaccess/translations.h"
#include "UniversalSpeech.h"  // v1.91 — speechStop() to suppress "not available"
#include "mediaaccess/youtube.h"  // ClearYouTubeCache, GetYouTubeCacheSize
#include "mediaaccess/database.h"          // book library folders
#include "mediaaccess/books_dialog.h"      // RescanBookLibrary
#include "mediaaccess/tts_player.h"        // SAPI voice list / set active
#include "mediaaccess/book_text_window.h"  // theme + always-hide
#include "mediaaccess/wasapi_loopback.h"   // v1.94 — system loopback device list
#include "mediaaccess/edge_tts_client.h"   // Edge voice list + preview synthesis
#include "bass.h"                          // preview playback
#include <set>
#include <cmath>
#include <thread>                          // off-UI-thread voice preview synthesis

// Dialog-private message: a background voice-preview synthesis finished.
// lParam = std::vector<unsigned char>* MP3 (heap, owned here) or nullptr on failure.
#define WM_SUB_PREVIEW_READY (WM_APP + 41)

// Dialog-private message (v2.44): the background Edge voice-catalog fetch
// finished, so the voice/language combos (filled from the offline fallback at
// init to avoid blocking the UI thread) can be refreshed from the cache.
#define WM_SUB_VOICES_READY (WM_APP + 42)

// Edge subtitle-voice picker state (Options > Speech). s_subEdgeVoices is the
// full catalog; s_subEdgeShown maps the currently-shown (language-filtered)
// combo rows back to indices in it. Populated in WM_INITDIALOG.
static std::vector<mediaaccess::EdgeVoice> s_subEdgeVoices;
static std::vector<int>                    s_subEdgeShown;
static HSTREAM                             s_subPreviewStream = 0;

// Ducking choices for the "Video volume while speaking" combo.
struct SubDuckOpt { const wchar_t* label; double value; };
static const SubDuckOpt s_subDuckOpts[] = {
    {L"100% (off)", 1.00}, {L"75%", 0.75}, {L"50%", 0.50},
    {L"30%", 0.30}, {L"15%", 0.15}, {L"0% (mute)", 0.00},
};

// Speech-rate choices (percent offset) for the "Speech rate" combo.
struct SubRateOpt { const wchar_t* label; int value; };
static const SubRateOpt s_subRateOpts[] = {
    {L"-50%", -50}, {L"-25%", -25}, {L"-10%", -10}, {L"Normal", 0},
    {L"+10%", 10}, {L"+25%", 25}, {L"+50%", 50}, {L"+75%", 75}, {L"+100%", 100},
};
static int SubRatePercentFromCombo(HWND hwnd) {
    int i = (int)SendMessageW(GetDlgItem(hwnd, IDC_SUBTITLE_RATE), CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < (int)(sizeof(s_subRateOpts)/sizeof(s_subRateOpts[0]))) return s_subRateOpts[i].value;
    return 0;
}

// v2.44 — localized combo labels. The numeric part is language-neutral; only
// the textual hint goes through T() ("(off)"/"(mute)" for ducking, "Normal" for
// the rate) so a French user no longer sees English words in those combos.
static std::wstring SubDuckLabel(int i) {
    double v = s_subDuckOpts[i].value;
    wchar_t pct[16]; swprintf(pct, 16, L"%d%%", (int)(v * 100.0 + 0.5));
    std::wstring s = pct;
    if (v >= 1.0)      s += std::wstring(L" ") + T("(off)");
    else if (v <= 0.0) s += std::wstring(L" ") + T("(mute)");
    return s;
}
static std::wstring SubRateLabel(int i) {
    int v = s_subRateOpts[i].value;
    if (v == 0) return T("Normal");
    wchar_t b[16]; swprintf(b, 16, L"%+d%%", v);
    return b;
}

// Distinct, sorted voice locales (e.g. "fr-FR", "en-US").
static std::vector<std::wstring> SubEdgeLocales() {
    std::set<std::wstring> s;
    for (auto& v : s_subEdgeVoices) if (!v.locale.empty()) s.insert(Utf8ToWide(v.locale));
    return std::vector<std::wstring>(s.begin(), s.end());
}

// Fill the voice combo with voices matching langFilter ("" = all); select the
// row whose short name equals selectShortName (else the first). Updates
// s_subEdgeShown so apply/preview can resolve the chosen row.
static void SubEdgePopulateVoices(HWND hwnd, const std::wstring& langFilter,
                                  const std::wstring& selectShortName) {
    HWND combo = GetDlgItem(hwnd, IDC_SUBTITLE_EDGE_VOICE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    s_subEdgeShown.clear();
    int sel = -1;
    for (size_t i = 0; i < s_subEdgeVoices.size(); i++) {
        if (!langFilter.empty() && Utf8ToWide(s_subEdgeVoices[i].locale) != langFilter) continue;
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s_subEdgeVoices[i].displayName.c_str());
        if (Utf8ToWide(s_subEdgeVoices[i].shortName) == selectShortName) sel = (int)s_subEdgeShown.size();
        s_subEdgeShown.push_back((int)i);
    }
    if (sel < 0 && !s_subEdgeShown.empty()) sel = 0;
    SendMessageW(combo, CB_SETCURSEL, sel, 0);
}

// Fill the language-filter combo ("All languages" + each locale) from the
// current s_subEdgeVoices, preselecting the locale of `selectShortName`, then
// populate the voice combo. Factored out so both WM_INITDIALOG and the async
// WM_SUB_VOICES_READY refresh share one path. v2.44.
static void SubEdgeFillLangAndVoices(HWND hwnd, const std::wstring& selectShortName) {
    std::wstring curLoc;
    for (auto& v : s_subEdgeVoices)
        if (Utf8ToWide(v.shortName) == selectShortName) { curLoc = Utf8ToWide(v.locale); break; }
    HWND lang = GetDlgItem(hwnd, IDC_SUBTITLE_EDGE_LANG);
    SendMessageW(lang, CB_RESETCONTENT, 0, 0);
    SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)T("All languages"));
    auto locs = SubEdgeLocales();
    int langSel = 0;
    for (size_t i = 0; i < locs.size(); i++) {
        SendMessageW(lang, CB_ADDSTRING, 0, (LPARAM)locs[i].c_str());
        if (locs[i] == curLoc) langSel = (int)i + 1;
    }
    SendMessageW(lang, CB_SETCURSEL, langSel, 0);
    SubEdgePopulateVoices(hwnd, langSel == 0 ? L"" : curLoc, selectShortName);
}

// Update the small hint under the SoundFont path field that tells the user
// what will actually be used when the field is empty. Three cases:
//   - User typed a path: hide the hint (empty label)
//   - Field empty, bundled FluidR3_GM.sf2 exists: show "Using bundled FluidR3_GM..."
//   - Field empty, no bundled SF: show "Using BASSMIDI built-in synth"
static void UpdateMidiBundledLabel(HWND hwnd) {
    wchar_t cur[MAX_PATH] = {0};
    GetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, cur, MAX_PATH);
    if (cur[0] != L'\0') {
        SetDlgItemTextW(hwnd, IDC_LABEL_MIDI_SF_BUNDLED, L"");
        return;
    }
    // Field is empty — figure out which fallback will actually fire
    wchar_t exePath[MAX_PATH];
    bool haveBundled = false;
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            std::wstring bundled = std::wstring(exePath) + L"lib\\FluidR3_GM.sf2";
            DWORD attrs = GetFileAttributesW(bundled.c_str());
            haveBundled = (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
        }
    }
    if (haveBundled) {
        SetDlgItemTextW(hwnd, IDC_LABEL_MIDI_SF_BUNDLED,
                        T("Using bundled FluidR3_GM (Frank Wen, MIT license)."));
    } else {
        SetDlgItemTextW(hwnd, IDC_LABEL_MIDI_SF_BUNDLED,
                        T("Using BASSMIDI built-in synth (basic sound)."));
    }
}

// Show only the controls for the currently-active tab and hide all others.
//
// Why this is one giant function instead of separate per-tab dialogs:
// keeping every control on a single dialog template lets NVDA's tab-order
// follow a single linear path. If we split into multiple sub-dialogs the
// screen-reader virtual cursor has to bounce in and out of child windows
// and announce extra "dialog" framing for each tab switch. The trade-off
// is a long array per tab, but the accessibility win is worth it.
//
// IMPORTANT: every IDC_* control in the Options dialog template MUST appear
// in exactly one of the arrays below. A control listed nowhere stays visible
// across all tabs (looks like a layout bug); a control listed twice gets
// shown when either tab is active.
void ShowTabControls(HWND hwnd, int tab) {
    // Tab indices: 0=Playback, 1=Recording, 2=Downloads, 3=Speech, 4=Movement,
    //              5=Effects, 6=Advanced, 7=YouTube, 8=SoundTouch, 9=Speedy,
    //              10=Signalsmith, 11=MIDI, 12=Books
    // (Global Hotkeys tab removed — accessible via Tools → Actions instead.)

    // Playback tab controls (tab 0)
    int playbackCtrls[] = {IDC_SOUNDCARD, IDC_ALLOW_AMPLIFY, IDC_REMEMBER_STATE, IDC_REMEMBER_POS, IDC_BRING_TO_FRONT, IDC_LOAD_FOLDER, IDC_MINIMIZE_TO_TRAY, IDC_VOLUME_STEP, IDC_SHOW_TITLE, IDC_AUTO_ADVANCE, IDC_PLAYLIST_FOLLOW, IDC_CHECK_UPDATES, IDC_MULTI_INSTANCE, IDC_REGISTER_FILE_TYPES, IDC_ANNOUNCE_ON_FOCUS, IDC_AUTO_FOLLOW_DEVICE, IDC_DOWNLOAD_PATH, IDC_DOWNLOAD_BROWSE, IDC_REWIND_ON_PAUSE, IDC_REWIND_LABEL,
                           IDC_LABEL_PLAYBACK_OUTPUT_DEVICE, IDC_LABEL_PLAYBACK_REMEMBER_POS, IDC_LABEL_PLAYBACK_VOLUME_STEP,
                           IDC_HISTORY_LIMIT, IDC_LABEL_PLAYBACK_HISTORY_LIMIT,
                           IDC_LANGUAGE_COMBO, IDC_LABEL_LANGUAGE};
    // Recording tab controls (tab 1)
    int recordingCtrls[] = {IDC_REC_PATH, IDC_REC_BROWSE, IDC_REC_TEMPLATE, IDC_REC_FORMAT, IDC_REC_BITRATE,
                            IDC_LABEL_RECORDING_DESCRIPTION, IDC_LABEL_RECORDING_OUTPUT_FOLDER, IDC_LABEL_RECORDING_TEMPLATE, IDC_LABEL_RECORDING_TEMPLATE_HELP, IDC_LABEL_RECORDING_FORMAT, IDC_LABEL_RECORDING_BITRATE, IDC_LABEL_RECORDING_BITRATE_NOTE,
                            IDC_REC_SOURCE, IDC_REC_SOURCE_LABEL, IDC_REC_SYSTEM_DEVICE, IDC_REC_SYSTEM_DEVICE_LABEL};  // v1.94
    // Downloads tab controls (tab 2)
    int downloadsCtrls[] = {IDC_DOWNLOAD_PATH, IDC_DOWNLOAD_BROWSE, IDC_DOWNLOAD_ORGANIZE,
                            IDC_LABEL_DOWNLOADS_DESCRIPTION, IDC_LABEL_DOWNLOADS_FOLDER};
    // Speech tab controls (tab 3)
    int speechCtrls[] = {IDC_SPEECH_TRACKCHANGE, IDC_SPEECH_VOLUME, IDC_SPEECH_EFFECT, IDC_SPEECH_YT_HYBRID,
                         IDC_SPEECH_SEEK_POSITION, IDC_SPEAK_SUBTITLES,
                         IDC_SUBTITLE_EDGE, IDC_SUBTITLE_EDGE_VOICE, IDC_LABEL_SUBTITLE_EDGE_VOICE,
                         IDC_SUBTITLE_EDGE_LANG, IDC_LABEL_SUBTITLE_EDGE_LANG, IDC_SUBTITLE_EDGE_PREVIEW,
                         IDC_SUBTITLE_DUCK, IDC_LABEL_SUBTITLE_DUCK,
                         IDC_SUBTITLE_RATE, IDC_LABEL_SUBTITLE_RATE,
                         IDC_LABEL_SPEECH_DESCRIPTION};
    // Movement tab controls (tab 4)
    int movementCtrls[] = {IDC_SEEK_1S, IDC_SEEK_5S, IDC_SEEK_10S, IDC_SEEK_30S, IDC_SEEK_1M, IDC_SEEK_5M, IDC_SEEK_10M,
                           IDC_SEEK_30M, IDC_SEEK_1H, IDC_SEEK_1T, IDC_SEEK_5T, IDC_SEEK_10T, IDC_CHAPTER_SEEK,
                           IDC_SEEK_SUBTITLE,
                           IDC_LABEL_MOVEMENT_DESCRIPTION};
    // Effects tab controls (tab 5)
    int effectCtrls[] = {IDC_EFFECT_VOLUME, IDC_EFFECT_PITCH, IDC_EFFECT_TEMPO, IDC_EFFECT_RATE, IDC_RATE_STEP_MODE,
                         IDC_DSP_REVERB, IDC_DSP_ECHO, IDC_DSP_EQ, IDC_DSP_COMPRESSOR, IDC_DSP_STEREOWIDTH,
                         IDC_DSP_CENTERCANCEL, IDC_DSP_SPATIAL, IDC_DSP_CONVOLUTION, IDC_CONV_IR, IDC_CONV_BROWSE,
                         IDC_LABEL_EFFECTS_DESCRIPTION, IDC_LABEL_EFFECTS_STEP, IDC_LABEL_EFFECTS_DSP_DESCRIPTION, IDC_LABEL_EFFECTS_REVERB, IDC_LABEL_EFFECTS_IR_FILE};
    // Advanced tab controls (tab 7)
    int advancedCtrls[] = {IDC_BUFFER_SIZE, IDC_UPDATE_PERIOD, IDC_TEMPO_ALGORITHM,
                           IDC_EQ_BASS_FREQ, IDC_EQ_MID_FREQ, IDC_EQ_TREBLE_FREQ,
                           IDC_LEGACY_VOLUME, IDC_DISABLE_BATCH, IDC_RESET_LIST_ORDER,
                           IDC_LABEL_ADVANCED_BUFFER_DESC, IDC_LABEL_ADVANCED_BUFFER_SIZE, IDC_LABEL_ADVANCED_UPDATE_PERIOD, IDC_LABEL_ADVANCED_LATENCY_NOTE, IDC_LABEL_ADVANCED_TEMPO_ALGO, IDC_LABEL_ADVANCED_EQ_FREQ, IDC_LABEL_ADVANCED_EQ_BASS, IDC_LABEL_ADVANCED_EQ_MID, IDC_LABEL_ADVANCED_EQ_TREBLE};
    // YouTube tab controls (tab 8) — yt-dlp path/browse removed (bundled + auto-updated)
    int youtubeCtrls[] = {IDC_YT_APIKEY, IDC_YT_CLEAR_ON_EXIT, IDC_YT_CLEAR_NOW, IDC_YT_CACHE_LIMIT, IDC_LABEL_YT_LIMIT,
                          IDC_YT_DOWNLOAD_PATH, IDC_YT_DOWNLOAD_PATH_BROWSE, IDC_LABEL_YT_DOWNLOAD_PATH,
                          IDC_LABEL_YOUTUBE_API_KEY, IDC_LABEL_YOUTUBE_API_HELP, IDC_LABEL_YOUTUBE_API_NOTE};
    // SoundTouch tab controls (tab 9)
    int soundtouchCtrls[] = {IDC_ST_AA_FILTER, IDC_ST_AA_LENGTH, IDC_ST_QUICK_ALGO, IDC_ST_SEQUENCE,
                             IDC_ST_SEEKWINDOW, IDC_ST_OVERLAP, IDC_ST_PREVENT_CLICK, IDC_ST_ALGORITHM,
                             IDC_LABEL_SOUNDTOUCH_DESCRIPTION, IDC_LABEL_SOUNDTOUCH_AA_LENGTH, IDC_LABEL_SOUNDTOUCH_INTERPOLATION, IDC_LABEL_SOUNDTOUCH_SEQUENCE, IDC_LABEL_SOUNDTOUCH_SEEKWINDOW, IDC_LABEL_SOUNDTOUCH_OVERLAP, IDC_LABEL_SOUNDTOUCH_AUTO_NOTE};
    // Speedy tab controls (tab 10)
    int speedyCtrls[] = {IDC_SPEEDY_NONLINEAR,
                         IDC_LABEL_SPEEDY_DESCRIPTION, IDC_LABEL_SPEEDY_INFO1, IDC_LABEL_SPEEDY_INFO2};
    // Signalsmith tab controls (tab 11)
    int signalsmithCtrls[] = {IDC_SS_PRESET, IDC_SS_TONALITY,
                              IDC_LABEL_SIGNALSMITH_DESCRIPTION, IDC_LABEL_SIGNALSMITH_QUALITY, IDC_LABEL_SIGNALSMITH_TONALITY, IDC_LABEL_SIGNALSMITH_HARMONICS, IDC_LABEL_SIGNALSMITH_VALUES};
    // MIDI tab controls (tab 11)
    int midiCtrls[] = {IDC_MIDI_SOUNDFONT, IDC_MIDI_SF_BROWSE, IDC_MIDI_VOICES, IDC_MIDI_SINC,
                       IDC_LABEL_MIDI_DESCRIPTION, IDC_LABEL_MIDI_SOUNDFONT, IDC_LABEL_MIDI_VOICES, IDC_LABEL_MIDI_VOICES_RANGE,
                       IDC_LABEL_MIDI_SF_BUNDLED};
    // Books tab controls (tab 12) — DAISY / EPUB library + TTS + text window
    int booksCtrls[] = {IDC_BOOK_FOLDERS_LIST, IDC_BOOK_FOLDER_ADD, IDC_BOOK_FOLDER_REMOVE,
                        IDC_BOOK_RESCAN, IDC_LABEL_BOOK_FOLDERS,
                        IDC_BOOK_TTS_VOICE, IDC_LABEL_BOOK_TTS_VOICE,
                        IDC_BOOK_TEXT_THEME, IDC_LABEL_BOOK_TEXT_THEME,
                        IDC_BOOK_HIDE_TEXT_WINDOW,
                        IDC_LABEL_BOOK_SKIP_GROUP,
                        IDC_BOOK_SKIP_PAGES, IDC_BOOK_SKIP_NOTES,
                        IDC_BOOK_SKIP_SIDEBARS, IDC_BOOK_SKIP_PRODNOTES,
                        IDC_BOOK_SKIP_FOOTNOTES, IDC_BOOK_SKIP_REFERENCES};



    // Show/hide playback controls
    for (int id : playbackCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide recording controls
    for (int id : recordingCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide downloads controls
    for (int id : downloadsCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 2 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide speech controls
    for (int id : speechCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 3 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide movement controls
    for (int id : movementCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 4 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide effect controls
    for (int id : effectCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 5 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide advanced controls
    for (int id : advancedCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 6 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide YouTube controls
    for (int id : youtubeCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 7 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide SoundTouch controls
    for (int id : soundtouchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 8 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide Speedy controls
    for (int id : speedyCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 9 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide Signalsmith controls
    for (int id : signalsmithCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 10 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide MIDI controls
    for (int id : midiCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 11 ? SW_SHOW : SW_HIDE);
    }

    // Show/hide Books library controls
    for (int id : booksCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 12 ? SW_SHOW : SW_HIDE);
    }
}

// ---------------------------------------------------------------------------
// Ctrl+Tab / Ctrl+Shift+Tab tab navigation for the Options dialog.
//
// A plain modal DialogBoxW eats VK_TAB internally (for control-to-control
// focus traversal) before WM_KEYDOWN can reach OptionsDlgProc, so we cannot
// reliably intercept Ctrl+Tab in the dialog proc. The canonical Win32 way to
// add Ctrl+Tab to a tabbed dialog is a thread-local WH_GETMESSAGE hook that
// is installed while the dialog is open and removed when it closes. The hook
// sees every message pumped for this thread (including the ones the dialog
// manager consumes), so it can spot Ctrl+Tab / Ctrl+Shift+Tab, swallow them,
// and cycle the tab control instead.
//
static HHOOK  s_optionsMsgHook = nullptr;
static HWND   s_optionsDlg     = nullptr;

// Switch to a tab as if the user had clicked it: update the tab control's
// selection, swap the visible controls, and move focus to the tab control so
// the screen reader announces the new tab name (same path used at dialog open
// and on plain Tab navigation). 'forward' true = next, false = previous; both
// wrap around.
static void OptionsCycleTab(HWND hwnd, bool forward) {
    HWND hTab = GetDlgItem(hwnd, IDC_TAB);
    if (!hTab) return;
    int count = TabCtrl_GetItemCount(hTab);
    if (count <= 1) return;
    int cur = TabCtrl_GetCurSel(hTab);
    if (cur < 0) cur = 0;
    int next = forward ? (cur + 1) % count
                       : (cur - 1 + count) % count;
    TabCtrl_SetCurSel(hTab, next);
    ShowTabControls(hwnd, next);
    // Refocus the tab control so NVDA/JAWS/Narrator read "Category: <name>,
    // tab N of M". TCN_SELCHANGE is not auto-raised by TabCtrl_SetCurSel, so
    // the focus move is what drives the announcement here.
    SetFocus(hTab);
}

// Thread-local message hook: catch Ctrl+Tab / Ctrl+Shift+Tab while the Options
// dialog is open. Returning after marking the message handled is not possible
// from WH_GETMESSAGE (we must pass it on), so we neutralise the keystroke by
// rewriting it to WM_NULL once we've acted on it, preventing the dialog
// manager from also treating the Tab as control traversal.
static LRESULT CALLBACK OptionsGetMsgHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE && s_optionsDlg) {
        MSG* m = reinterpret_cast<MSG*>(lParam);
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_TAB &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Only act for messages belonging to our dialog (its tab control
            // or any child within it).
            if (m->hwnd == s_optionsDlg || IsChild(s_optionsDlg, m->hwnd)) {
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                OptionsCycleTab(s_optionsDlg, !back);
                m->message = WM_NULL;   // consume: don't let the dialog see Tab
            }
        }
    }
    return CallNextHookEx(s_optionsMsgHook, code, wParam, lParam);
}

// Options dialog procedure
INT_PTR CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            // v1.91 — Drain the screen-reader queue immediately. DialogBox
            // disables the parent window (g_hwnd) before WM_INITDIALOG fires,
            // which emits EVENT_OBJECT_STATECHANGE with STATE_SYSTEM_UNAVAILABLE
            // on the parent. NVDA then verbalises "non disponible" / "not
            // available" at the start of the dialog-open sequence. The focus
            // event we post below (via SetFocus on IDC_TAB) is queued AFTER
            // this speechStop, so the dialog title + tab name announcement
            // still plays. Net result: the bare "non disponible" prefix is
            // gone.
            speechStop();
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(T("Playback"));
            TabCtrl_InsertItem(hTab, 0, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Recording"));
            TabCtrl_InsertItem(hTab, 1, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Downloads"));
            TabCtrl_InsertItem(hTab, 2, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Speech"));
            TabCtrl_InsertItem(hTab, 3, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Movement"));
            TabCtrl_InsertItem(hTab, 4, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Effects"));
            TabCtrl_InsertItem(hTab, 5, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Advanced"));
            TabCtrl_InsertItem(hTab, 6, &tie);
            tie.pszText = const_cast<LPWSTR>(T("YouTube"));
            TabCtrl_InsertItem(hTab, 7, &tie);
            tie.pszText = const_cast<LPWSTR>(T("SoundTouch"));
            TabCtrl_InsertItem(hTab, 8, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Speedy"));
            TabCtrl_InsertItem(hTab, 9, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Signalsmith"));
            TabCtrl_InsertItem(hTab, 10, &tie);
            tie.pszText = const_cast<LPWSTR>(T("MIDI"));
            TabCtrl_InsertItem(hTab, 11, &tie);
            tie.pszText = const_cast<LPWSTR>(T("Books"));
            TabCtrl_InsertItem(hTab, 12, &tie);

            // v1.90 — Hide all non-Playback tab controls BEFORE populating
            // anything. Reason: a later EnableWindow(IDC_REC_BITRATE, FALSE)
            // for non-lossy record formats was firing EVENT_OBJECT_STATECHANGE
            // with STATE_SYSTEM_UNAVAILABLE while the combo was still visible,
            // which NVDA verbalized as "not available" / "non disponible" at
            // dialog open time. Hiding non-Playback controls first masks them
            // with STATE_SYSTEM_INVISIBLE; NVDA then ignores their state
            // changes during populate. Without this, the bug was reproducible
            // on every Ctrl+, regardless of focus initial / tab name fixes.
            ShowTabControls(hwnd, 0);

            // Populate Books library folders list
            {
                HWND hList = GetDlgItem(hwnd, IDC_BOOK_FOLDERS_LIST);
                SendMessageW(hList, LB_RESETCONTENT, 0, 0);
                for (const auto& f : GetBookLibraryFolders()) {
                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
                }
            }

            // Populate TTS voice combo
            {
                HWND hVoice = GetDlgItem(hwnd, IDC_BOOK_TTS_VOICE);
                SendMessageW(hVoice, CB_RESETCONTENT, 0, 0);
                // First item = "(Windows default)" with empty id
                SendMessageW(hVoice, CB_ADDSTRING, 0,
                             (LPARAM)T("(Windows default voice)"));
                auto voices = mediaaccess::TtsListVoices();
                int activeIdx = 0;  // default = first item
                std::wstring activeId = mediaaccess::TtsGetActiveVoiceId();
                for (size_t i = 0; i < voices.size(); ++i) {
                    int idx = (int)SendMessageW(hVoice, CB_ADDSTRING, 0,
                                                (LPARAM)voices[i].displayName.c_str());
                    if (idx >= 0 && !activeId.empty() && voices[i].id == activeId) {
                        activeIdx = idx;
                    }
                }
                SendMessageW(hVoice, CB_SETCURSEL, activeIdx, 0);
            }

            // Populate text-window theme combo
            {
                HWND hTheme = GetDlgItem(hwnd, IDC_BOOK_TEXT_THEME);
                SendMessageW(hTheme, CB_RESETCONTENT, 0, 0);
                SendMessageW(hTheme, CB_ADDSTRING, 0, (LPARAM)T("Standard"));
                SendMessageW(hTheme, CB_ADDSTRING, 0, (LPARAM)T("High contrast"));
                SendMessageW(hTheme, CB_ADDSTRING, 0, (LPARAM)T("Large"));
                SendMessageW(hTheme, CB_SETCURSEL,
                             (int)mediaaccess::BookTextWindowGetTheme(), 0);
            }

            // "Always hide text window" checkbox
            CheckDlgButton(hwnd, IDC_BOOK_HIDE_TEXT_WINDOW,
                           mediaaccess::BookTextWindowGetAlwaysHide()
                               ? BST_CHECKED : BST_UNCHECKED);

            // DAISY "skip" checkboxes. Bit layout in g_bookSkipMask:
            //   Page=bit0, Note=bit1, Sidebar=bit2, Prodnote=bit3,
            //   Footnote=bit4, Reference=bit5.
            // Must stay in sync with daisy_player.cpp's skip-class check.
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_PAGES,
                           (g_bookSkipMask & (1u << 0)) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_NOTES,
                           (g_bookSkipMask & (1u << 1)) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_SIDEBARS,
                           (g_bookSkipMask & (1u << 2)) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_PRODNOTES,
                           (g_bookSkipMask & (1u << 3)) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_FOOTNOTES,
                           (g_bookSkipMask & (1u << 4)) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_BOOK_SKIP_REFERENCES,
                           (g_bookSkipMask & (1u << 5)) ? BST_CHECKED : BST_UNCHECKED);

            // (Global Hotkeys tab removed — see Tools → Actions instead.)

            // Populate sound card combo box
            HWND hCombo = GetDlgItem(hwnd, IDC_SOUNDCARD);
            BASS_DEVICEINFO info;
            int currentIndex = 0;

            for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
                if (info.flags & BASS_DEVICE_ENABLED) {
                    // Convert device name to wide string
                    int len = MultiByteToWideChar(CP_ACP, 0, info.name, -1, nullptr, 0);
                    std::wstring wideName(len, 0);
                    MultiByteToWideChar(CP_ACP, 0, info.name, -1, &wideName[0], len);

                    int idx = static_cast<int>(SendMessageW(hCombo, CB_ADDSTRING, 0,
                        reinterpret_cast<LPARAM>(wideName.c_str())));
                    SendMessageW(hCombo, CB_SETITEMDATA, idx, i);

                    if (i == g_selectedDevice || (g_selectedDevice == -1 && (info.flags & BASS_DEVICE_DEFAULT))) {
                        currentIndex = idx;
                    }
                }
            }
            SendMessageW(hCombo, CB_SETCURSEL, currentIndex, 0);

            // Set amplify checkbox
            CheckDlgButton(hwnd, IDC_ALLOW_AMPLIFY, g_allowAmplify ? BST_CHECKED : BST_UNCHECKED);

            // Set remember playback state checkbox
            CheckDlgButton(hwnd, IDC_REMEMBER_STATE, g_rememberState ? BST_CHECKED : BST_UNCHECKED);

            // Set bring to front checkbox
            CheckDlgButton(hwnd, IDC_BRING_TO_FRONT, g_bringToFront ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_LOAD_FOLDER, g_loadFolder ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_MINIMIZE_TO_TRAY, g_minimizeToTray ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SHOW_TITLE, g_showTitleInWindow ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_AUTO_ADVANCE, g_autoAdvance ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_PLAYLIST_FOLLOW, g_playlistFollowPlayback ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHECK_UPDATES, g_checkForUpdates ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_ANNOUNCE_ON_FOCUS, g_announceTrackOnFocus ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_MULTI_INSTANCE, g_allowMultipleInstances ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_AUTO_FOLLOW_DEVICE, g_autoFollowDevice ? BST_CHECKED : BST_UNCHECKED);

            SetDlgItemInt(hwnd, IDC_REWIND_ON_PAUSE, g_rewindOnPauseMs, FALSE);

            // Set download path and organize checkbox
            SetDlgItemTextW(hwnd, IDC_DOWNLOAD_PATH, g_downloadPath.c_str());
            CheckDlgButton(hwnd, IDC_DOWNLOAD_ORGANIZE, g_downloadOrganizeByFeed ? BST_CHECKED : BST_UNCHECKED);

            // Populate volume step combo box
            {
                HWND hVolStepCombo = GetDlgItem(hwnd, IDC_VOLUME_STEP);
                const wchar_t* stepLabels[] = {L"1%", L"2%", L"5%", L"10%", L"15%", L"20%", L"25%"};
                const int stepValues[] = {1, 2, 5, 10, 15, 20, 25};
                int stepIndex = 1;  // Default to 2%
                for (int i = 0; i < 7; i++) {
                    SendMessageW(hVolStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(stepLabels[i]));
                    if (static_cast<int>(g_volumeStep * 100 + 0.5f) == stepValues[i]) {
                        stepIndex = i;
                    }
                }
                SendMessageW(hVolStepCombo, CB_SETCURSEL, stepIndex, 0);
            }

            // v2.11 — populate history-size combo (max 50, FIFO eviction).
            {
                HWND hHist = GetDlgItem(hwnd, IDC_HISTORY_LIMIT);
                const int histValues[] = {10, 20, 30, 40, 50};
                int histIndex = 4;  // default to 50
                for (int i = 0; i < 5; i++) {
                    wchar_t lbl[8];
                    swprintf(lbl, 8, L"%d", histValues[i]);
                    SendMessageW(hHist, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lbl));
                    if (g_historyLimit == histValues[i]) histIndex = i;
                }
                SendMessageW(hHist, CB_SETCURSEL, histIndex, 0);
            }

            // Populate remember position combo box
            HWND hPosCombo = GetDlgItem(hwnd, IDC_REMEMBER_POS);
            const wchar_t* posLabels[] = {L"Off", L"5 minutes", L"10 minutes", L"20 minutes", L"30 minutes", L"45 minutes", L"60 minutes"};
            int posIndex = 0;
            for (int i = 0; i < g_posThresholdCount; i++) {
                SendMessageW(hPosCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(posLabels[i]));
                if (g_posThresholds[i] == g_rememberPosMinutes) {
                    posIndex = i;
                }
            }
            SendMessageW(hPosCombo, CB_SETCURSEL, posIndex, 0);

            // Set seek amount checkboxes
            CheckDlgButton(hwnd, IDC_SEEK_1S, g_seekEnabled[0] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5S, g_seekEnabled[1] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10S, g_seekEnabled[2] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_30S, g_seekEnabled[3] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_1M, g_seekEnabled[4] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5M, g_seekEnabled[5] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10M, g_seekEnabled[6] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_30M, g_seekEnabled[7] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_1H, g_seekEnabled[8] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_1T, g_seekEnabled[9] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_5T, g_seekEnabled[10] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_10T, g_seekEnabled[11] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHAPTER_SEEK, g_chapterSeekEnabled ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SEEK_SUBTITLE, g_subtitleSeekEnabled ? BST_CHECKED : BST_UNCHECKED);

            // Set file types registration checkbox
            CheckDlgButton(hwnd, IDC_REGISTER_FILE_TYPES, g_registerFileTypes ? BST_CHECKED : BST_UNCHECKED);

            // Set effect checkboxes
            CheckDlgButton(hwnd, IDC_EFFECT_VOLUME, g_effectEnabled[0] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_PITCH, g_effectEnabled[1] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_TEMPO, g_effectEnabled[2] ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_EFFECT_RATE, g_effectEnabled[3] ? BST_CHECKED : BST_UNCHECKED);

            // Set rate step mode combobox
            {
                HWND hRateStepCombo = GetDlgItem(hwnd, IDC_RATE_STEP_MODE);
                SendMessageW(hRateStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"0.01x"));
                SendMessageW(hRateStepCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Semitone"));
                SendMessageW(hRateStepCombo, CB_SETCURSEL, g_rateStepMode, 0);
            }

            // Set reverb algorithm combobox
            {
                HWND hReverbCombo = GetDlgItem(hwnd, IDC_DSP_REVERB);
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Freeverb (Musical)"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"DX8 (DirectX)"));
                SendMessageW(hReverbCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"I3DL2 (Environmental)"));
                SendMessageW(hReverbCombo, CB_SETCURSEL, g_reverbAlgorithm, 0);
            }

            // Set DSP effect checkboxes
            CheckDlgButton(hwnd, IDC_DSP_ECHO, IsDSPEffectEnabled(DSPEffectType::Echo) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_EQ, IsDSPEffectEnabled(DSPEffectType::EQ) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_COMPRESSOR, IsDSPEffectEnabled(DSPEffectType::Compressor) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_STEREOWIDTH, IsDSPEffectEnabled(DSPEffectType::StereoWidth) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_CENTERCANCEL, IsDSPEffectEnabled(DSPEffectType::CenterCancel) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_CONVOLUTION, IsDSPEffectEnabled(DSPEffectType::Convolution) ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_DSP_SPATIAL, IsDSPEffectEnabled(DSPEffectType::SpatialAudio) ? BST_CHECKED : BST_UNCHECKED);

            // Display current IR file path (just filename)
            if (!g_convolutionIRPath.empty()) {
                std::wstring filename = g_convolutionIRPath;
                size_t pos = filename.find_last_of(L"\\/");
                if (pos != std::wstring::npos) {
                    filename = filename.substr(pos + 1);
                }
                SetDlgItemTextW(hwnd, IDC_CONV_IR, filename.c_str());
            }

            // Populate buffer size combo box
            {
                HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
                int bufferIndex = 3;  // Default to 500ms
                for (int i = 0; i < g_bufferSizeCount; i++) {
                    wchar_t label[32];
                    swprintf(label, 32, L"%d ms", g_bufferSizes[i]);
                    SendMessageW(hBufferCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                    if (g_bufferSizes[i] == g_bufferSize) {
                        bufferIndex = i;
                    }
                }
                SendMessageW(hBufferCombo, CB_SETCURSEL, bufferIndex, 0);
            }

            // Populate update period combo box
            {
                HWND hUpdateCombo = GetDlgItem(hwnd, IDC_UPDATE_PERIOD);
                int updateIndex = 4;  // Default to 100ms
                for (int i = 0; i < g_updatePeriodCount; i++) {
                    wchar_t label[32];
                    swprintf(label, 32, L"%d ms", g_updatePeriods[i]);
                    SendMessageW(hUpdateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
                    if (g_updatePeriods[i] == g_updatePeriod) {
                        updateIndex = i;
                    }
                }
                SendMessageW(hUpdateCombo, CB_SETCURSEL, updateIndex, 0);
            }

            // Populate tempo algorithm combo box
            {
                HWND hAlgoCombo = GetDlgItem(hwnd, IDC_TEMPO_ALGORITHM);
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SoundTouch (BASS_FX) - Fast, good for speech"));
#ifdef USE_SPEEDY
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speedy (Google) - Nonlinear speech speedup"));
#else
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Speedy (coming soon)"));
#endif
#ifdef USE_SIGNALSMITH
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Signalsmith Stretch - High quality time/pitch"));
#else
                SendMessageW(hAlgoCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Signalsmith (coming soon)"));
#endif
                SendMessageW(hAlgoCombo, CB_SETCURSEL, g_tempoAlgorithm, 0);
            }

            // Initialize EQ frequency edit controls
            {
                wchar_t buf[32];
                swprintf(buf, 32, L"%.0f", g_eqBassFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_BASS_FREQ, buf);
                swprintf(buf, 32, L"%.0f", g_eqMidFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_MID_FREQ, buf);
                swprintf(buf, 32, L"%.0f", g_eqTrebleFreq);
                SetDlgItemTextW(hwnd, IDC_EQ_TREBLE_FREQ, buf);
            }

            // Initialize legacy volume checkbox
            CheckDlgButton(hwnd, IDC_LEGACY_VOLUME, g_legacyVolume ? BST_CHECKED : BST_UNCHECKED);

            // Initialize disable batch delay checkbox
            CheckDlgButton(hwnd, IDC_DISABLE_BATCH, g_disableBatchDelay ? BST_CHECKED : BST_UNCHECKED);

            // Initialize YouTube tab (yt-dlp path is bundled/auto-detected, no UI)
            SetDlgItemTextW(hwnd, IDC_YT_APIKEY, g_ytApiKey.c_str());
            CheckDlgButton(hwnd, IDC_YT_CLEAR_ON_EXIT, g_clearYtCacheOnExit ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(hwnd, IDC_YT_CACHE_LIMIT, g_ytCacheLimitMB, FALSE);
            // v1.71 — show current YouTube download folder (empty string is fine; it
            // signals to the user that the historical default is in effect).
            SetDlgItemTextW(hwnd, IDC_YT_DOWNLOAD_PATH, g_ytDownloadPath.c_str());

            // Initialize Recording tab
            {
                // Set default recording path to user's Music folder if not set
                if (g_recordPath.empty()) {
                    wchar_t musicPath[MAX_PATH];
                    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
                        g_recordPath = musicPath;
                    }
                }
                SetDlgItemTextW(hwnd, IDC_REC_PATH, g_recordPath.c_str());
                SetDlgItemTextW(hwnd, IDC_REC_TEMPLATE, g_recordTemplate.c_str());

                // Format combo: WAV, MP3, OGG, FLAC
                HWND hFormatCombo = GetDlgItem(hwnd, IDC_REC_FORMAT);
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAV (lossless)"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MP3"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"OGG Vorbis"));
                SendMessageW(hFormatCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FLAC (lossless)"));
                SendMessageW(hFormatCombo, CB_SETCURSEL, g_recordFormat, 0);

                // Bitrate combo (for MP3/OGG)
                HWND hBitrateCombo = GetDlgItem(hwnd, IDC_REC_BITRATE);
                int bitrates[] = {128, 160, 192, 224, 256, 320};
                int bitrateIndex = 2;  // Default to 192
                for (int i = 0; i < 6; i++) {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%d kbps", bitrates[i]);
                    SendMessageW(hBitrateCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
                    if (bitrates[i] == g_recordBitrate) {
                        bitrateIndex = i;
                    }
                }
                SendMessageW(hBitrateCombo, CB_SETCURSEL, bitrateIndex, 0);

                // Enable bitrate only for lossy formats (MP3=1, OGG=2)
                BOOL enableBitrate = (g_recordFormat == 1 || g_recordFormat == 2);
                EnableWindow(hBitrateCombo, enableBitrate);

                // v1.94 — Recording source combo (0 = MediaAccess output, 1 = system).
                HWND hSourceCombo = GetDlgItem(hwnd, IDC_REC_SOURCE);
                SendMessageW(hSourceCombo, CB_RESETCONTENT, 0, 0);
                SendMessageW(hSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("MediaAccess output")));
                SendMessageW(hSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("Windows system output")));
                SendMessageW(hSourceCombo, CB_SETCURSEL, (g_recordSource == 1) ? 1 : 0, 0);

                // v1.94 — System loopback device combo. Item 0 = "Automatic"
                // (bwaIndex sentinel -1); each subsequent item stores its real
                // BASSWASAPI index in the item data so OK can read it back.
                HWND hSysDevCombo = GetDlgItem(hwnd, IDC_REC_SYSTEM_DEVICE);
                SendMessageW(hSysDevCombo, CB_RESETCONTENT, 0, 0);
                int autoItem = (int)SendMessageW(hSysDevCombo, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(T("Automatic (follow MediaAccess output)")));
                SendMessageW(hSysDevCombo, CB_SETITEMDATA, autoItem, (LPARAM)-1);
                int selDevItem = 0;  // default to Automatic
                {
                    auto devices = mediaaccess::EnumerateLoopbackDevices();
                    for (const auto& d : devices) {
                        int item = (int)SendMessageW(hSysDevCombo, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(d.name.c_str()));
                        SendMessageW(hSysDevCombo, CB_SETITEMDATA, item, (LPARAM)d.bwaIndex);
                        if (d.bwaIndex == g_systemRecordDevice) selDevItem = item;
                    }
                }
                SendMessageW(hSysDevCombo, CB_SETCURSEL, selDevItem, 0);

                // The device combo only matters when the source is the system.
                EnableWindow(hSysDevCombo, g_recordSource == 1);
            }

            // Initialize Speech tab
            CheckDlgButton(hwnd, IDC_SPEECH_TRACKCHANGE, g_speechTrackChange ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_VOLUME, g_speechVolume ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_EFFECT, g_speechEffect ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_YT_HYBRID, g_speechYTHybrid ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEECH_SEEK_POSITION, g_speechSeekPosition ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_SPEAK_SUBTITLES, g_speakSubtitles ? BST_CHECKED : BST_UNCHECKED);  // v1.81
            CheckDlgButton(hwnd, IDC_SUBTITLE_EDGE, g_subtitleUseEdgeVoice ? BST_CHECKED : BST_UNCHECKED);
            {
                // v2.44 — fill from the NON-BLOCKING cached list so opening
                // Options never blocks the UI thread on the network (critical
                // for a screen reader). If the startup prewarm hasn't finished,
                // we get the offline fallback now and refresh asynchronously
                // below via WM_SUB_VOICES_READY.
                s_subEdgeVoices = mediaaccess::EdgeListVoicesCached();
                SubEdgeFillLangAndVoices(hwnd, g_subtitleEdgeVoice);
                if (!mediaaccess::EdgeVoicesReady()) {
                    HWND dlg = hwnd;
                    std::thread([dlg]() {
                        mediaaccess::EdgeListVoices();   // blocking fetch, off the UI thread
                        PostMessageW(dlg, WM_SUB_VOICES_READY, 0, 0);  // dropped if dialog gone
                    }).detach();
                }
                // Ducking combo. The numeric part is fixed; only the parenthetical
                // hint ("(off)"/"(mute)") is localized (v2.44).
                HWND duck = GetDlgItem(hwnd, IDC_SUBTITLE_DUCK);
                SendMessageW(duck, CB_RESETCONTENT, 0, 0);
                int duckSel = 0; double best = 1e9;
                for (int i = 0; i < (int)(sizeof(s_subDuckOpts)/sizeof(s_subDuckOpts[0])); i++) {
                    SendMessageW(duck, CB_ADDSTRING, 0, (LPARAM)SubDuckLabel(i).c_str());
                    double d = fabs(s_subDuckOpts[i].value - g_subtitleDuckLevel);
                    if (d < best) { best = d; duckSel = i; }
                }
                SendMessageW(duck, CB_SETCURSEL, duckSel, 0);
                // Speech-rate combo ("Normal" is localized; v2.44).
                HWND rate = GetDlgItem(hwnd, IDC_SUBTITLE_RATE);
                SendMessageW(rate, CB_RESETCONTENT, 0, 0);
                int rateSel = 0, bestR = 1000;
                for (int i = 0; i < (int)(sizeof(s_subRateOpts)/sizeof(s_subRateOpts[0])); i++) {
                    SendMessageW(rate, CB_ADDSTRING, 0, (LPARAM)SubRateLabel(i).c_str());
                    int d = abs(s_subRateOpts[i].value - g_subtitleEdgeRate);
                    if (d < bestR) { bestR = d; rateSel = i; }
                }
                SendMessageW(rate, CB_SETCURSEL, rateSel, 0);
            }

            // Initialize SoundTouch tab
            {
                CheckDlgButton(hwnd, IDC_ST_AA_FILTER, g_stAntiAliasFilter ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_ST_QUICK_ALGO, g_stQuickAlgorithm ? BST_CHECKED : BST_UNCHECKED);
                CheckDlgButton(hwnd, IDC_ST_PREVENT_CLICK, g_stPreventClick ? BST_CHECKED : BST_UNCHECKED);

                // AA filter length combo (8, 16, 32, 64, 128)
                HWND hAALen = GetDlgItem(hwnd, IDC_ST_AA_LENGTH);
                int aaLengths[] = {8, 16, 32, 64, 128};
                int aaIndex = 2;  // Default to 32
                for (int i = 0; i < 5; i++) {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%d", aaLengths[i]);
                    SendMessageW(hAALen, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(buf));
                    if (aaLengths[i] == g_stAAFilterLength) aaIndex = i;
                }
                SendMessageW(hAALen, CB_SETCURSEL, aaIndex, 0);

                // Interpolation algorithm combo
                HWND hAlgo = GetDlgItem(hwnd, IDC_ST_ALGORITHM);
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Linear"));
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cubic"));
                SendMessageW(hAlgo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Shannon"));
                SendMessageW(hAlgo, CB_SETCURSEL, g_stAlgorithm, 0);

                // Sequence, seek window, overlap edit controls
                wchar_t buf[16];
                swprintf(buf, 16, L"%d", g_stSequenceMs);
                SetDlgItemTextW(hwnd, IDC_ST_SEQUENCE, buf);
                swprintf(buf, 16, L"%d", g_stSeekWindowMs);
                SetDlgItemTextW(hwnd, IDC_ST_SEEKWINDOW, buf);
                swprintf(buf, 16, L"%d", g_stOverlapMs);
                SetDlgItemTextW(hwnd, IDC_ST_OVERLAP, buf);
            }

            // Initialize Speedy tab
            {
                CheckDlgButton(hwnd, IDC_SPEEDY_NONLINEAR, g_speedyNonlinear ? BST_CHECKED : BST_UNCHECKED);
            }

            // Initialize Signalsmith tab
            {
                HWND hPreset = GetDlgItem(hwnd, IDC_SS_PRESET);
                SendMessageW(hPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Default (higher quality)"));
                SendMessageW(hPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Cheaper (lower CPU)"));
                SendMessageW(hPreset, CB_SETCURSEL, g_ssPreset, 0);

                wchar_t buf[32];
                swprintf(buf, 32, L"%d", g_ssTonalityLimit);
                SetDlgItemTextW(hwnd, IDC_SS_TONALITY, buf);
            }

            // Initialize MIDI tab controls
            {
                SetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, g_midiSoundFont.c_str());

                wchar_t buf[32];
                swprintf(buf, 32, L"%d", g_midiMaxVoices);
                SetDlgItemTextW(hwnd, IDC_MIDI_VOICES, buf);

                CheckDlgButton(hwnd, IDC_MIDI_SINC, g_midiSincInterp ? BST_CHECKED : BST_UNCHECKED);

                // Show "(Using bundled FluidR3_GM)" hint when path empty
                UpdateMidiBundledLabel(hwnd);
            }

            // Populate language combo
            {
                HWND hLangCombo = GetDlgItem(hwnd, IDC_LANGUAGE_COMBO);
                SendMessageW(hLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
                SendMessageW(hLangCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Français"));
                SendMessageW(hLangCombo, CB_SETCURSEL, (g_language == "fr") ? 1 : 0, 0);
            }

            // Localize dialog controls now that all combo items are populated
            LocalizeDialog(hwnd);

            // Show only playback tab controls initially
            ShowTabControls(hwnd, 0);

            // v1.88: force initial focus onto the tab control so NVDA opens
            // with "Options dialog" then "Category: Playback, tab 1 of N"
            // instead of a stray "not available" from whatever control would
            // otherwise be picked. We return FALSE to tell Windows we handled
            // the focus ourselves.
            SetFocus(GetDlgItem(hwnd, IDC_TAB));

            // Install the Ctrl+Tab / Ctrl+Shift+Tab navigation hook for the
            // lifetime of this (modal) dialog. Thread-local — removed in
            // WM_DESTROY.
            s_optionsDlg = hwnd;
            s_optionsMsgHook = SetWindowsHookExW(WH_GETMESSAGE, OptionsGetMsgHook,
                                                 nullptr, GetCurrentThreadId());
            return FALSE;
        }

        case WM_DESTROY: {
            if (s_optionsMsgHook) {
                UnhookWindowsHookEx(s_optionsMsgHook);
                s_optionsMsgHook = nullptr;
            }
            // v2.44 — free a still-playing preview stream on any close path (not
            // just IDOK/IDCANCEL) so an Alt+F4 / Esc that bypasses them can't
            // leak the BASS stream.
            if (s_subPreviewStream) {
                BASS_ChannelStop(s_subPreviewStream);
                BASS_StreamFree(s_subPreviewStream);
                s_subPreviewStream = 0;
            }
            s_optionsDlg = nullptr;
            break;
        }

        case WM_SUB_VOICES_READY: {
            // v2.44 — the background voice-catalog fetch finished; the dialog
            // was populated from the offline fallback at init, so refresh the
            // language + voice combos from the now-ready cache, preserving the
            // current selection. lParam unused (we re-read the shared cache).
            std::wstring sel = g_subtitleEdgeVoice;   // captured from the OLD state
            int vi = (int)SendMessageW(GetDlgItem(hwnd, IDC_SUBTITLE_EDGE_VOICE), CB_GETCURSEL, 0, 0);
            if (vi >= 0 && vi < (int)s_subEdgeShown.size() &&
                s_subEdgeShown[vi] < (int)s_subEdgeVoices.size())
                sel = Utf8ToWide(s_subEdgeVoices[s_subEdgeShown[vi]].shortName);
            s_subEdgeVoices = mediaaccess::EdgeListVoicesCached();
            SubEdgeFillLangAndVoices(hwnd, sel);
            return TRUE;
        }

        case WM_SUB_PREVIEW_READY: {
            // Background voice preview finished; play it on the UI thread.
            auto* mp3 = reinterpret_cast<std::vector<unsigned char>*>(lParam);
            if (mp3) {
                if (s_subPreviewStream) { BASS_ChannelStop(s_subPreviewStream); BASS_StreamFree(s_subPreviewStream); }
                s_subPreviewStream = BASS_StreamCreateFile(BASS_FILE_MEMCOPY, mp3->data(), 0,
                                                           mp3->size(), BASS_SAMPLE_FLOAT);
                if (s_subPreviewStream) BASS_ChannelPlay(s_subPreviewStream, FALSE);
                delete mp3;
            } else {
                Speak(Ts("Preview failed"));
            }
            return TRUE;
        }

        case WM_NOTIFY: {
            NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (pnmh->idFrom == IDC_TAB && pnmh->code == TCN_SELCHANGE) {
                int tab = TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_TAB));
                ShowTabControls(hwnd, tab);
            }
            break;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Get selected device
                    HWND hCombo = GetDlgItem(hwnd, IDC_SOUNDCARD);
                    int sel = static_cast<int>(SendMessageW(hCombo, CB_GETCURSEL, 0, 0));
                    int newDevice = static_cast<int>(SendMessageW(hCombo, CB_GETITEMDATA, sel, 0));

                    // Get amplify setting
                    bool newAmplify = (IsDlgButtonChecked(hwnd, IDC_ALLOW_AMPLIFY) == BST_CHECKED);

                    // Get remember playback state setting
                    g_rememberState = (IsDlgButtonChecked(hwnd, IDC_REMEMBER_STATE) == BST_CHECKED);

                    // Get bring to front setting
                    g_bringToFront = (IsDlgButtonChecked(hwnd, IDC_BRING_TO_FRONT) == BST_CHECKED);
                    g_loadFolder = (IsDlgButtonChecked(hwnd, IDC_LOAD_FOLDER) == BST_CHECKED);
                    g_minimizeToTray = (IsDlgButtonChecked(hwnd, IDC_MINIMIZE_TO_TRAY) == BST_CHECKED);
                    g_showTitleInWindow = (IsDlgButtonChecked(hwnd, IDC_SHOW_TITLE) == BST_CHECKED);
                    g_autoAdvance = (IsDlgButtonChecked(hwnd, IDC_AUTO_ADVANCE) == BST_CHECKED);
                    g_playlistFollowPlayback = (IsDlgButtonChecked(hwnd, IDC_PLAYLIST_FOLLOW) == BST_CHECKED);
                    g_checkForUpdates = (IsDlgButtonChecked(hwnd, IDC_CHECK_UPDATES) == BST_CHECKED);
                    g_announceTrackOnFocus = (IsDlgButtonChecked(hwnd, IDC_ANNOUNCE_ON_FOCUS) == BST_CHECKED);
                    g_allowMultipleInstances = (IsDlgButtonChecked(hwnd, IDC_MULTI_INSTANCE) == BST_CHECKED);
                    g_autoFollowDevice = (IsDlgButtonChecked(hwnd, IDC_AUTO_FOLLOW_DEVICE) == BST_CHECKED);
                    UpdateWindowTitle();  // Apply immediately

                    g_rewindOnPauseMs = GetDlgItemInt(hwnd, IDC_REWIND_ON_PAUSE, nullptr, FALSE);
                    if (g_rewindOnPauseMs < 0) g_rewindOnPauseMs = 0;

                    // Get download settings
                    {
                        wchar_t dlPath[MAX_PATH];
                        GetDlgItemTextW(hwnd, IDC_DOWNLOAD_PATH, dlPath, MAX_PATH);
                        g_downloadPath = dlPath;
                        g_downloadOrganizeByFeed = (IsDlgButtonChecked(hwnd, IDC_DOWNLOAD_ORGANIZE) == BST_CHECKED);
                    }

                    // Get volume step setting
                    {
                        HWND hVolStepCombo = GetDlgItem(hwnd, IDC_VOLUME_STEP);
                        int volStepSel = static_cast<int>(SendMessageW(hVolStepCombo, CB_GETCURSEL, 0, 0));
                        const int stepValues[] = {1, 2, 5, 10, 15, 20, 25};
                        if (volStepSel >= 0 && volStepSel < 7) {
                            g_volumeStep = stepValues[volStepSel] / 100.0f;
                        }
                    }

                    // v2.11 — get history-size setting (clamped 1..50) and apply
                    // it immediately so an over-cap DB shrinks now.
                    {
                        HWND hHist = GetDlgItem(hwnd, IDC_HISTORY_LIMIT);
                        int histSel = static_cast<int>(SendMessageW(hHist, CB_GETCURSEL, 0, 0));
                        const int histValues[] = {10, 20, 30, 40, 50};
                        if (histSel >= 0 && histSel < 5) {
                            g_historyLimit = histValues[histSel];
                            if (g_historyLimit < 1)  g_historyLimit = 1;
                            if (g_historyLimit > 50) g_historyLimit = 50;
                            PruneSongHistoryToLimit();
                        }
                    }

                    // Get remember position threshold
                    HWND hPosCombo = GetDlgItem(hwnd, IDC_REMEMBER_POS);
                    int posSel = static_cast<int>(SendMessageW(hPosCombo, CB_GETCURSEL, 0, 0));
                    if (posSel >= 0 && posSel < g_posThresholdCount) {
                        g_rememberPosMinutes = g_posThresholds[posSel];
                    }

                    // Apply device change if needed
                    if (newDevice != g_selectedDevice) {
                        ReinitBass(newDevice);
                    }

                    // Apply amplify setting
                    g_allowAmplify = newAmplify;

                    // Clamp volume if amplify was disabled
                    if (!g_allowAmplify && g_volume > MAX_VOLUME_NORMAL) {
                        SetVolume(MAX_VOLUME_NORMAL);
                    }

                    // Get seek amount checkboxes
                    g_seekEnabled[0] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1S) == BST_CHECKED);
                    g_seekEnabled[1] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5S) == BST_CHECKED);
                    g_seekEnabled[2] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10S) == BST_CHECKED);
                    g_seekEnabled[3] = (IsDlgButtonChecked(hwnd, IDC_SEEK_30S) == BST_CHECKED);
                    g_seekEnabled[4] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1M) == BST_CHECKED);
                    g_seekEnabled[5] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5M) == BST_CHECKED);
                    g_seekEnabled[6] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10M) == BST_CHECKED);
                    g_seekEnabled[7] = (IsDlgButtonChecked(hwnd, IDC_SEEK_30M) == BST_CHECKED);
                    g_seekEnabled[8] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1H) == BST_CHECKED);
                    g_seekEnabled[9] = (IsDlgButtonChecked(hwnd, IDC_SEEK_1T) == BST_CHECKED);
                    g_seekEnabled[10] = (IsDlgButtonChecked(hwnd, IDC_SEEK_5T) == BST_CHECKED);
                    g_seekEnabled[11] = (IsDlgButtonChecked(hwnd, IDC_SEEK_10T) == BST_CHECKED);
                    g_chapterSeekEnabled = (IsDlgButtonChecked(hwnd, IDC_CHAPTER_SEEK) == BST_CHECKED);
                    g_subtitleSeekEnabled = (IsDlgButtonChecked(hwnd, IDC_SEEK_SUBTITLE) == BST_CHECKED);

                    // Validate current seek index - ensure it points to an enabled amount
                    if (!g_seekEnabled[g_currentSeekIndex]) {
                        for (int i = 0; i < g_seekAmountCount; i++) {
                            if (g_seekEnabled[i]) {
                                g_currentSeekIndex = i;
                                break;
                            }
                        }
                    }

                    // Update file types registration setting
                    {
                        bool newRegister = (IsDlgButtonChecked(hwnd, IDC_REGISTER_FILE_TYPES) == BST_CHECKED);
                        if (newRegister && !g_registerFileTypes) {
                            // Just enabled - register all file types now
                            RegisterAllFileTypes();
                        } else if (!newRegister && g_registerFileTypes) {
                            // Just disabled - unregister all file types now
                            UnregisterAllFileTypes();
                        }
                        g_registerFileTypes = newRegister;
                    }

                    // Get effect checkboxes
                    g_effectEnabled[0] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_VOLUME) == BST_CHECKED);
                    g_effectEnabled[1] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_PITCH) == BST_CHECKED);
                    g_effectEnabled[2] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_TEMPO) == BST_CHECKED);
                    g_effectEnabled[3] = (IsDlgButtonChecked(hwnd, IDC_EFFECT_RATE) == BST_CHECKED);

                    // Get rate step mode
                    {
                        HWND hRateStepCombo = GetDlgItem(hwnd, IDC_RATE_STEP_MODE);
                        int rateStepSel = static_cast<int>(SendMessageW(hRateStepCombo, CB_GETCURSEL, 0, 0));
                        if (rateStepSel >= 0 && rateStepSel <= 1) {
                            g_rateStepMode = rateStepSel;
                        }
                    }

                    // Validate current effect index - ensure it points to an enabled effect
                    if (!g_effectEnabled[g_currentEffectIndex]) {
                        for (int i = 0; i < 4; i++) {
                            if (g_effectEnabled[i]) {
                                g_currentEffectIndex = i;
                                break;
                            }
                        }
                    }

                    // Get reverb algorithm from combobox
                    {
                        HWND hReverbCombo = GetDlgItem(hwnd, IDC_DSP_REVERB);
                        int reverbSel = static_cast<int>(SendMessageW(hReverbCombo, CB_GETCURSEL, 0, 0));
                        if (reverbSel >= 0 && reverbSel <= 3) {
                            SetReverbAlgorithm(reverbSel);
                        }
                    }

                    // Get DSP effect checkboxes and enable/disable effects
                    EnableDSPEffect(DSPEffectType::Echo, IsDlgButtonChecked(hwnd, IDC_DSP_ECHO) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::EQ, IsDlgButtonChecked(hwnd, IDC_DSP_EQ) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::Compressor, IsDlgButtonChecked(hwnd, IDC_DSP_COMPRESSOR) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::StereoWidth, IsDlgButtonChecked(hwnd, IDC_DSP_STEREOWIDTH) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::CenterCancel, IsDlgButtonChecked(hwnd, IDC_DSP_CENTERCANCEL) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::Convolution, IsDlgButtonChecked(hwnd, IDC_DSP_CONVOLUTION) == BST_CHECKED);
                    EnableDSPEffect(DSPEffectType::SpatialAudio, IsDlgButtonChecked(hwnd, IDC_DSP_SPATIAL) == BST_CHECKED);

                    // Get buffer settings
                    {
                        HWND hBufferCombo = GetDlgItem(hwnd, IDC_BUFFER_SIZE);
                        int bufferSel = static_cast<int>(SendMessageW(hBufferCombo, CB_GETCURSEL, 0, 0));
                        if (bufferSel >= 0 && bufferSel < g_bufferSizeCount) {
                            g_bufferSize = g_bufferSizes[bufferSel];
                            BASS_SetConfig(BASS_CONFIG_BUFFER, g_bufferSize);
                        }

                        HWND hUpdateCombo = GetDlgItem(hwnd, IDC_UPDATE_PERIOD);
                        int updateSel = static_cast<int>(SendMessageW(hUpdateCombo, CB_GETCURSEL, 0, 0));
                        if (updateSel >= 0 && updateSel < g_updatePeriodCount) {
                            g_updatePeriod = g_updatePeriods[updateSel];
                            BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, g_updatePeriod);
                        }

                        HWND hAlgoCombo = GetDlgItem(hwnd, IDC_TEMPO_ALGORITHM);
                        int algoSel = static_cast<int>(SendMessageW(hAlgoCombo, CB_GETCURSEL, 0, 0));
                        if (algoSel >= 0 && algoSel < static_cast<int>(TempoAlgorithm::COUNT)) {
                            g_tempoAlgorithm = algoSel;
                        }

                        // Get EQ frequencies
                        wchar_t freqBuf[32];
                        GetDlgItemTextW(hwnd, IDC_EQ_BASS_FREQ, freqBuf, 32);
                        float bassFreq = static_cast<float>(_wtof(freqBuf));
                        if (bassFreq >= 20.0f && bassFreq <= 500.0f) g_eqBassFreq = bassFreq;

                        GetDlgItemTextW(hwnd, IDC_EQ_MID_FREQ, freqBuf, 32);
                        float midFreq = static_cast<float>(_wtof(freqBuf));
                        if (midFreq >= 200.0f && midFreq <= 5000.0f) g_eqMidFreq = midFreq;

                        GetDlgItemTextW(hwnd, IDC_EQ_TREBLE_FREQ, freqBuf, 32);
                        float trebleFreq = static_cast<float>(_wtof(freqBuf));
                        if (trebleFreq >= 2000.0f && trebleFreq <= 20000.0f) g_eqTrebleFreq = trebleFreq;

                        // Get legacy volume setting
                        bool wasLegacy = g_legacyVolume;
                        g_legacyVolume = (IsDlgButtonChecked(hwnd, IDC_LEGACY_VOLUME) == BST_CHECKED);

                        // Get disable batch delay setting
                        g_disableBatchDelay = (IsDlgButtonChecked(hwnd, IDC_DISABLE_BATCH) == BST_CHECKED);

                        // Handle mode switch
                        if (wasLegacy != g_legacyVolume && g_fxStream) {
                            if (g_legacyVolume) {
                                // Switching TO legacy: apply volume via BASS_ATTRIB_VOL
                                float curvedVolume = g_muted ? 0.0f : (g_volume * g_volume);
                                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
                            } else {
                                // Switching FROM legacy: reset BASS_ATTRIB_VOL to 1.0 so DSP works
                                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, 1.0f);
                                // Ensure volume DSP is set up
                                ApplyDSPEffects();
                            }
                        }
                    }

                    // Get YouTube settings (yt-dlp path managed automatically)
                    {
                        wchar_t buf[512];
                        GetDlgItemTextW(hwnd, IDC_YT_APIKEY, buf, 512);
                        g_ytApiKey = buf;
                    }
                    g_clearYtCacheOnExit = (IsDlgButtonChecked(hwnd, IDC_YT_CLEAR_ON_EXIT) == BST_CHECKED);
                    {
                        BOOL ok = FALSE;
                        int v = (int)GetDlgItemInt(hwnd, IDC_YT_CACHE_LIMIT, &ok, FALSE);
                        if (ok && v >= 0) g_ytCacheLimitMB = v;
                    }
                    // v1.71 — capture the user-chosen YouTube download folder. The
                    // Edit control is editable so the user can either Browse to pick
                    // a folder, or simply clear the field to revert to the historical
                    // default. Any invalid value (typo, removed USB stick, etc.) is
                    // handled by the silent fallback in youtube.cpp.
                    {
                        wchar_t buf[MAX_PATH];
                        GetDlgItemTextW(hwnd, IDC_YT_DOWNLOAD_PATH, buf, MAX_PATH);
                        g_ytDownloadPath = buf;
                    }

                    // Get Recording settings
                    {
                        wchar_t buf[512];
                        GetDlgItemTextW(hwnd, IDC_REC_PATH, buf, 512);
                        g_recordPath = buf;
                        GetDlgItemTextW(hwnd, IDC_REC_TEMPLATE, buf, 512);
                        g_recordTemplate = buf;

                        HWND hFormatCombo = GetDlgItem(hwnd, IDC_REC_FORMAT);
                        int formatSel = static_cast<int>(SendMessageW(hFormatCombo, CB_GETCURSEL, 0, 0));
                        if (formatSel >= 0 && formatSel <= 3) g_recordFormat = formatSel;

                        HWND hBitrateCombo = GetDlgItem(hwnd, IDC_REC_BITRATE);
                        int bitrateSel = static_cast<int>(SendMessageW(hBitrateCombo, CB_GETCURSEL, 0, 0));
                        int bitrates[] = {128, 160, 192, 224, 256, 320};
                        if (bitrateSel >= 0 && bitrateSel < 6) g_recordBitrate = bitrates[bitrateSel];

                        // v1.94 — recording source + system device. Be defensive:
                        // never change the source while a recording is in progress
                        // (legacy OR system), so we can't swap engines mid-capture.
                        bool recActive = g_isRecording || mediaaccess::IsSystemCapturing();
                        if (!recActive) {
                            int srcSel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_REC_SOURCE), CB_GETCURSEL, 0, 0));
                            if (srcSel == 0 || srcSel == 1) g_recordSource = srcSel;
                        }
                        // The device choice is harmless to update anytime; it only
                        // takes effect at the next system capture start.
                        HWND hSysDevCombo = GetDlgItem(hwnd, IDC_REC_SYSTEM_DEVICE);
                        int devSel = static_cast<int>(SendMessageW(hSysDevCombo, CB_GETCURSEL, 0, 0));
                        if (devSel >= 0) {
                            g_systemRecordDevice = (int)SendMessageW(hSysDevCombo, CB_GETITEMDATA, devSel, 0);
                        }
                    }

                    // Get Speech settings
                    g_speechTrackChange = (IsDlgButtonChecked(hwnd, IDC_SPEECH_TRACKCHANGE) == BST_CHECKED);
                    g_speechVolume = (IsDlgButtonChecked(hwnd, IDC_SPEECH_VOLUME) == BST_CHECKED);
                    g_speechEffect = (IsDlgButtonChecked(hwnd, IDC_SPEECH_EFFECT) == BST_CHECKED);
                    g_speechYTHybrid = (IsDlgButtonChecked(hwnd, IDC_SPEECH_YT_HYBRID) == BST_CHECKED);
                    g_speechSeekPosition = (IsDlgButtonChecked(hwnd, IDC_SPEECH_SEEK_POSITION) == BST_CHECKED);
                    g_speakSubtitles = (IsDlgButtonChecked(hwnd, IDC_SPEAK_SUBTITLES) == BST_CHECKED);  // v1.81
                    g_subtitleUseEdgeVoice = (IsDlgButtonChecked(hwnd, IDC_SUBTITLE_EDGE) == BST_CHECKED);
                    {
                        int idx = (int)SendMessageW(GetDlgItem(hwnd, IDC_SUBTITLE_EDGE_VOICE), CB_GETCURSEL, 0, 0);
                        if (idx >= 0 && idx < (int)s_subEdgeShown.size())
                            g_subtitleEdgeVoice = Utf8ToWide(s_subEdgeVoices[s_subEdgeShown[idx]].shortName);
                        int di = (int)SendMessageW(GetDlgItem(hwnd, IDC_SUBTITLE_DUCK), CB_GETCURSEL, 0, 0);
                        if (di >= 0 && di < (int)(sizeof(s_subDuckOpts)/sizeof(s_subDuckOpts[0])))
                            g_subtitleDuckLevel = s_subDuckOpts[di].value;
                        g_subtitleEdgeRate = SubRatePercentFromCombo(hwnd);
                    }
                    extern void RefreshSubtitleEdge();
                    RefreshSubtitleEdge();   // apply method/voice/ducking change immediately

                    // Get SoundTouch settings
                    {
                        g_stAntiAliasFilter = (IsDlgButtonChecked(hwnd, IDC_ST_AA_FILTER) == BST_CHECKED);
                        g_stQuickAlgorithm = (IsDlgButtonChecked(hwnd, IDC_ST_QUICK_ALGO) == BST_CHECKED);
                        g_stPreventClick = (IsDlgButtonChecked(hwnd, IDC_ST_PREVENT_CLICK) == BST_CHECKED);

                        wchar_t buf[32];
                        GetDlgItemTextW(hwnd, IDC_ST_AA_LENGTH, buf, 32);
                        int aaLen = _wtoi(buf);
                        if (aaLen >= 8 && aaLen <= 128) g_stAAFilterLength = aaLen;

                        GetDlgItemTextW(hwnd, IDC_ST_SEQUENCE, buf, 32);
                        int seq = _wtoi(buf);
                        if (seq >= 0 && seq <= 200) g_stSequenceMs = seq;

                        GetDlgItemTextW(hwnd, IDC_ST_SEEKWINDOW, buf, 32);
                        int seek = _wtoi(buf);
                        if (seek >= 0 && seek <= 100) g_stSeekWindowMs = seek;

                        GetDlgItemTextW(hwnd, IDC_ST_OVERLAP, buf, 32);
                        int overlap = _wtoi(buf);
                        if (overlap >= 0 && overlap <= 50) g_stOverlapMs = overlap;

                        HWND hAlgoCombo = GetDlgItem(hwnd, IDC_ST_ALGORITHM);
                        int algoSel = static_cast<int>(SendMessageW(hAlgoCombo, CB_GETCURSEL, 0, 0));
                        if (algoSel >= 0 && algoSel <= 2) g_stAlgorithm = algoSel;
                    }

                    // Get Speedy settings
                    {
                        g_speedyNonlinear = (IsDlgButtonChecked(hwnd, IDC_SPEEDY_NONLINEAR) == BST_CHECKED);
                    }

                    // Get Signalsmith settings
                    {
                        HWND hPresetCombo = GetDlgItem(hwnd, IDC_SS_PRESET);
                        int presetSel = static_cast<int>(SendMessageW(hPresetCombo, CB_GETCURSEL, 0, 0));
                        if (presetSel >= 0 && presetSel <= 1) g_ssPreset = presetSel;

                        wchar_t buf[32];
                        GetDlgItemTextW(hwnd, IDC_SS_TONALITY, buf, 32);
                        int tonality = _wtoi(buf);
                        if (tonality >= 0 && tonality <= 20000) g_ssTonalityLimit = tonality;
                    }

                    // Get MIDI settings
                    {
                        wchar_t buf[MAX_PATH];
                        GetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, buf, MAX_PATH);
                        g_midiSoundFont = buf;

                        wchar_t voicesBuf[32];
                        GetDlgItemTextW(hwnd, IDC_MIDI_VOICES, voicesBuf, 32);
                        int voices = _wtoi(voicesBuf);
                        if (voices >= 1 && voices <= 1000) g_midiMaxVoices = voices;

                        g_midiSincInterp = (IsDlgButtonChecked(hwnd, IDC_MIDI_SINC) == BST_CHECKED);
                    }

                    // Get Language setting
                    {
                        int langSel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_LANGUAGE_COMBO), CB_GETCURSEL, 0, 0));
                        std::string newLang = (langSel == 1) ? "fr" : "en";
                        if (newLang != g_language) {
                            g_language = newLang;
                            SetLanguage(g_language.c_str());
                            // Refresh main menu immediately so the change is visible without restart
                            LocalizeMenu(GetMenu(g_hwnd));
                            DrawMenuBar(g_hwnd);
                            MessageBoxW(hwnd, T("Language change will fully apply after restart."), L"MediaAccess", MB_OK | MB_ICONINFORMATION);
                        }
                    }

                    // Gather DAISY skippable-content checkboxes into the
                    // global bitmask before persisting. Bit layout must
                    // match daisy_player.cpp's skip-class test.
                    g_bookSkipMask = 0;
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_PAGES)      == BST_CHECKED) g_bookSkipMask |= (1u << 0);
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_NOTES)      == BST_CHECKED) g_bookSkipMask |= (1u << 1);
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_SIDEBARS)   == BST_CHECKED) g_bookSkipMask |= (1u << 2);
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_PRODNOTES)  == BST_CHECKED) g_bookSkipMask |= (1u << 3);
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_FOOTNOTES)  == BST_CHECKED) g_bookSkipMask |= (1u << 4);
                    if (IsDlgButtonChecked(hwnd, IDC_BOOK_SKIP_REFERENCES) == BST_CHECKED) g_bookSkipMask |= (1u << 5);

                    // Save settings
                    SaveSettings();

                    if (s_subPreviewStream) { BASS_ChannelStop(s_subPreviewStream); BASS_StreamFree(s_subPreviewStream); s_subPreviewStream = 0; }
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }

                case IDCANCEL:
                    if (s_subPreviewStream) { BASS_ChannelStop(s_subPreviewStream); BASS_StreamFree(s_subPreviewStream); s_subPreviewStream = 0; }
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;

                case IDC_YT_CLEAR_NOW: {
                    // Clear-now button: confirmation prompt that mirrors the
                    // Help > Clear YouTube cache item, so users get the same
                    // affordance from inside Options.
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
                    return TRUE;
                }

                case IDC_REC_BROWSE: {
                    // Browse for recording output folder
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = T("Select recording output folder");
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t folderPath[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, folderPath)) {
                            SetDlgItemTextW(hwnd, IDC_REC_PATH, folderPath);
                        }
                        CoTaskMemFree(pidl);
                    }
                    return TRUE;
                }

                case IDC_DOWNLOAD_BROWSE: {
                    // Browse for downloads folder
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = T("Select downloads folder");
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t folderPath[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, folderPath)) {
                            SetDlgItemTextW(hwnd, IDC_DOWNLOAD_PATH, folderPath);
                        }
                        CoTaskMemFree(pidl);
                    }
                    return TRUE;
                }

                case IDC_YT_DOWNLOAD_PATH_BROWSE: {
                    // v1.71 — Browse for YouTube downloads folder. Same pattern as
                    // IDC_DOWNLOAD_BROWSE above. Saving the dialog with an empty
                    // path resets MediaAccess to the historical default; the edit
                    // field is NOT ES_READONLY so the user can simply Delete the
                    // path to revert. Free-typed invalid paths fall back silently
                    // to the legacy folder via youtube.cpp::GetDownloadsTargetDir.
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = T("Select YouTube downloads folder");
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t folderPath[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, folderPath)) {
                            SetDlgItemTextW(hwnd, IDC_YT_DOWNLOAD_PATH, folderPath);
                        }
                        CoTaskMemFree(pidl);
                    }
                    return TRUE;
                }

                // -----------------------------------------------------------
                // Books library tab — Add/Remove a folder of DAISY/EPUB
                // books, and Rescan to refresh the SQLite catalogue.
                // -----------------------------------------------------------
                case IDC_BOOK_FOLDER_ADD: {
                    BROWSEINFOW bi = {0};
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = T("Select a folder containing DAISY or EPUB books");
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t folderPath[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, folderPath)) {
                            AddBookLibraryFolder(folderPath);
                            // Repopulate the list from DB
                            HWND hList = GetDlgItem(hwnd, IDC_BOOK_FOLDERS_LIST);
                            SendMessageW(hList, LB_RESETCONTENT, 0, 0);
                            for (const auto& f : GetBookLibraryFolders()) {
                                SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)f.c_str());
                            }
                            // Auto-scan the new folder so the library populates
                            // immediately without forcing the user to click Rescan.
                            int added = mediaaccess::RescanBookLibrary();
                            wchar_t buf[128];
                            swprintf(buf, 128,
                                T("Scan complete: %d book(s) added or updated."), added);
                            SpeakW(buf);
                        }
                        CoTaskMemFree(pidl);
                    }
                    return TRUE;
                }

                case IDC_BOOK_FOLDER_REMOVE: {
                    HWND hList = GetDlgItem(hwnd, IDC_BOOK_FOLDERS_LIST);
                    int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                    if (sel < 0) return TRUE;
                    int len = (int)SendMessageW(hList, LB_GETTEXTLEN, sel, 0);
                    if (len < 0) return TRUE;
                    std::wstring path(len + 1, L'\0');
                    SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)path.data());
                    path.resize(wcslen(path.c_str()));
                    RemoveBookLibraryFolder(path);
                    SendMessageW(hList, LB_DELETESTRING, sel, 0);
                    if (sel >= (int)SendMessageW(hList, LB_GETCOUNT, 0, 0)) sel--;
                    if (sel >= 0) SendMessageW(hList, LB_SETCURSEL, sel, 0);
                    return TRUE;
                }

                case IDC_BOOK_RESCAN: {
                    int added = mediaaccess::RescanBookLibrary();
                    wchar_t buf[128];
                    swprintf(buf, 128,
                        T("Scan complete: %d book(s) added or updated."), added);
                    SpeakW(buf);
                    return TRUE;
                }

                case IDC_BOOK_TTS_VOICE: {
                    if (HIWORD(wParam) != CBN_SELCHANGE) break;
                    int sel = (int)SendDlgItemMessageW(hwnd, IDC_BOOK_TTS_VOICE, CB_GETCURSEL, 0, 0);
                    if (sel < 0) return TRUE;
                    if (sel == 0) {
                        mediaaccess::TtsSetActiveVoiceId(L"");  // Windows default
                    } else {
                        auto voices = mediaaccess::TtsListVoices();
                        int voiceIdx = sel - 1;
                        if (voiceIdx >= 0 && voiceIdx < (int)voices.size()) {
                            mediaaccess::TtsSetActiveVoiceId(voices[voiceIdx].id);
                        }
                    }
                    return TRUE;
                }

                case IDC_SUBTITLE_EDGE_LANG: {
                    if (HIWORD(wParam) != CBN_SELCHANGE) break;
                    int li = (int)SendDlgItemMessageW(hwnd, IDC_SUBTITLE_EDGE_LANG, CB_GETCURSEL, 0, 0);
                    std::wstring filter;
                    if (li > 0) {
                        auto locs = SubEdgeLocales();
                        if (li - 1 < (int)locs.size()) filter = locs[li - 1];
                    }
                    SubEdgePopulateVoices(hwnd, filter, g_subtitleEdgeVoice);
                    return TRUE;
                }

                case IDC_SUBTITLE_EDGE_PREVIEW: {
                    int vi = (int)SendDlgItemMessageW(hwnd, IDC_SUBTITLE_EDGE_VOICE, CB_GETCURSEL, 0, 0);
                    if (vi < 0 || vi >= (int)s_subEdgeShown.size()) return TRUE;
                    std::string voice = s_subEdgeVoices[s_subEdgeShown[vi]].shortName;  // copy for the worker
                    std::wstring sample = (voice.rfind("fr", 0) == 0)
                        ? L"Bonjour, ceci est un aperçu de la voix des sous-titres."
                        : L"Hello, this is a preview of the subtitle voice.";
                    char rb[16]; sprintf_s(rb, "%+d%%", SubRatePercentFromCombo(hwnd));
                    std::string rate = rb;
                    HWND dlg = hwnd;
                    // Synthesize off the UI thread (network round trip) and post the
                    // MP3 back to the dialog to play — keeps Options responsive.
                    std::thread([dlg, voice, sample, rate]() {
                        auto* mp3 = new std::vector<unsigned char>();
                        std::string err;
                        mediaaccess::EdgeClearCancel();   // v2.44 — a prior SubStop may have armed cancel
                        bool ok = mediaaccess::EdgeSynthesize(voice, sample, *mp3, rate, "+0Hz", &err)
                                  && !mp3->empty();
                        if (!ok) { delete mp3; mp3 = nullptr; }
                        PostMessageW(dlg, WM_SUB_PREVIEW_READY, 0, (LPARAM)mp3);
                    }).detach();
                    return TRUE;
                }

                case IDC_BOOK_TEXT_THEME: {
                    if (HIWORD(wParam) != CBN_SELCHANGE) break;
                    int sel = (int)SendDlgItemMessageW(hwnd, IDC_BOOK_TEXT_THEME, CB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel > 2) return TRUE;
                    mediaaccess::BookTextWindowSetTheme((mediaaccess::BookTextTheme)sel);
                    return TRUE;
                }

                case IDC_BOOK_HIDE_TEXT_WINDOW:
                    mediaaccess::BookTextWindowSetAlwaysHide(
                        IsDlgButtonChecked(hwnd, IDC_BOOK_HIDE_TEXT_WINDOW) == BST_CHECKED);
                    return TRUE;

                case IDC_REC_FORMAT:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        // Enable bitrate only for lossy formats (MP3=1, OGG=2)
                        int format = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_REC_FORMAT), CB_GETCURSEL, 0, 0));
                        BOOL enableBitrate = (format == 1 || format == 2);
                        EnableWindow(GetDlgItem(hwnd, IDC_REC_BITRATE), enableBitrate);
                    }
                    return TRUE;

                // v1.94 — live-toggle the system-device combo with the source.
                case IDC_REC_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int src = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_REC_SOURCE), CB_GETCURSEL, 0, 0));
                        EnableWindow(GetDlgItem(hwnd, IDC_REC_SYSTEM_DEVICE), src == 1);
                    }
                    return TRUE;

                case IDC_MIDI_SF_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"SoundFont Files (*.sf2;*.sf3;*.sfz)\0*.sf2;*.sf3;*.sfz\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = T("Select SoundFont file");
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        SetDlgItemTextW(hwnd, IDC_MIDI_SOUNDFONT, filePath);
                        UpdateMidiBundledLabel(hwnd);
                    }
                    return TRUE;
                }

                case IDC_MIDI_SOUNDFONT:
                    // Live refresh the "Using bundled FluidR3_GM" hint as the
                    // user types or clears the path.
                    if (HIWORD(wParam) == EN_CHANGE) {
                        UpdateMidiBundledLabel(hwnd);
                    }
                    return TRUE;

                case IDC_CONV_BROWSE: {
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {sizeof(ofn)};
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"IR Files (*.wav;*.flac;*.ogg;*.mp3)\0*.wav;*.flac;*.ogg;*.mp3\0"
                                      L"WAV Files (*.wav)\0*.wav\0"
                                      L"FLAC Files (*.flac)\0*.flac\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = T("Select Impulse Response file");
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        g_convolutionIRPath = filePath;
                        // Display just the filename
                        std::wstring filename = filePath;
                        size_t pos = filename.find_last_of(L"\\/");
                        if (pos != std::wstring::npos) {
                            filename = filename.substr(pos + 1);
                        }
                        SetDlgItemTextW(hwnd, IDC_CONV_IR, filename.c_str());
                        // Load the IR file
                        ConvolutionReverb* conv = GetConvolutionReverb();
                        if (conv) {
                            conv->LoadIR(filePath);
                        }
                    }
                    return TRUE;
                }

                case IDC_RESET_LIST_ORDER: {
                    ResetRadioSortOrder();
                    ResetPodcastSortOrder();
                    Speak(Ts("Order reset to alphabetical"));
                    return TRUE;
                }

                // (Global hotkeys are managed from Tools → Actions in the
                // Global category — no dedicated tab here.)
            }
            break;
    }

    return FALSE;
}

// Show options dialog
void ShowOptionsDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_OPTIONS), g_hwnd, OptionsDlgProc);
}

// ---------------------------------------------------------------------------
// AuditOptionsLayout — runtime layout checker.
//
// Walks every control in IDD_OPTIONS in the current language, measures the
// localized text with GetTextExtentPoint32W using the dialog's actual font,
// and flags any STATIC/BUTTON whose text no longer fits. The Options dialog
// is created modeless and never shown — we only need the layout, not the
// interactive state.
//
// Padding heuristics (in pixels, matched against MS Shell Dlg 8pt rendering):
//   - Checkbox / radio glyph + gap : 18 px
//   - Pushbutton internal margin    : 10 px
//   - Static label slack             :  2 px
// ---------------------------------------------------------------------------
struct AuditIssue {
    int  id;
    std::wstring text;
    std::wstring cls;
    int  controlPx;
    int  neededPx;
};

struct AuditCtx {
    HDC                       hdc;
    std::vector<AuditIssue>*  issues;
};

static BOOL CALLBACK AuditEnumProc(HWND child, LPARAM lp) {
    auto* ctx = reinterpret_cast<AuditCtx*>(lp);

    wchar_t cls[64] = {0};
    GetClassNameW(child, cls, 64);

    // Only measure controls whose displayed text lives in GetWindowText().
    // Skip combos (text is the dropdown selection, varies at runtime),
    // edits, listboxes, tab control, and the dialog itself.
    bool isStatic  = (_wcsicmp(cls, L"Static") == 0);
    bool isButton  = (_wcsicmp(cls, L"Button") == 0);
    if (!isStatic && !isButton) return TRUE;

    int len = GetWindowTextLengthW(child);
    if (len <= 0) return TRUE;
    std::wstring text(len + 1, L'\0');
    GetWindowTextW(child, &text[0], len + 1);
    text.resize(len);

    // Strip the & accelerator marker before measuring (it's not drawn).
    std::wstring measured;
    measured.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] != L'&') continue;
        if (text[i] == L'&' && i + 1 < text.size() && text[i + 1] == L'&') { measured += L'&'; ++i; continue; }
        measured += text[i];
    }

    RECT rc;
    GetWindowRect(child, &rc);
    int controlPx = rc.right - rc.left;

    SIZE sz;
    GetTextExtentPoint32W(ctx->hdc, measured.c_str(), (int)measured.size(), &sz);

    int padding = 2;
    if (isButton) {
        LONG style = GetWindowLongW(child, GWL_STYLE);
        DWORD bt   = style & BS_TYPEMASK;
        bool isCheckOrRadio =
            (bt == BS_CHECKBOX || bt == BS_AUTOCHECKBOX ||
             bt == BS_RADIOBUTTON || bt == BS_AUTORADIOBUTTON ||
             bt == BS_3STATE || bt == BS_AUTO3STATE);
        padding = isCheckOrRadio ? 18 : 10;
    }

    int needed = sz.cx + padding;
    if (needed > controlPx) {
        AuditIssue iss;
        iss.id        = GetDlgCtrlID(child);
        iss.text      = text;
        iss.cls       = cls;
        iss.controlPx = controlPx;
        iss.neededPx  = needed;
        ctx->issues->push_back(iss);
    }
    return TRUE;
}

void AuditOptionsLayout() {
    HWND hDlg = CreateDialogParamW(GetModuleHandle(nullptr),
                                   MAKEINTRESOURCEW(IDD_OPTIONS),
                                   g_hwnd, OptionsDlgProc, 0);
    if (!hDlg) {
        MessageBoxW(g_hwnd, T("Could not create Options dialog for audit."),
                    T("Layout audit"), MB_OK | MB_ICONERROR);
        return;
    }

    // Force every tab's controls to be visible so EnumChildWindows sees them
    // with their final layout. ShowTabControls hides off-tab controls; we
    // want them all measurable.
    for (HWND c = GetWindow(hDlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        ShowWindow(c, SW_SHOW);
    }

    std::vector<AuditIssue> issues;
    HDC hdc = GetDC(hDlg);
    HFONT hFont = (HFONT)SendMessageW(hDlg, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = hFont ? SelectObject(hdc, hFont) : nullptr;

    AuditCtx ctx{hdc, &issues};
    EnumChildWindows(hDlg, AuditEnumProc, reinterpret_cast<LPARAM>(&ctx));

    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(hDlg, hdc);
    DestroyWindow(hDlg);

    if (issues.empty()) {
        MessageBoxW(g_hwnd, T("No truncated controls found."),
                    T("Layout audit"), MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Build a textual report and save it next to MediaAccess.exe.
    std::wstring msg = T("Truncated controls:");
    msg += L"\n\n";
    for (const auto& e : issues) {
        wchar_t line[768];
        std::wstring shown = e.text;
        if (shown.size() > 60) shown = shown.substr(0, 57) + L"...";
        swprintf(line, 768, L"  [%d] %s — \"%s\" (needs %d px, has %d px)\n",
                 e.id, e.cls.c_str(), shown.c_str(), e.neededPx, e.controlPx);
        msg += line;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (wchar_t* slash = wcsrchr(exePath, L'\\')) {
        *(slash + 1) = L'\0';
        wcscat_s(exePath, MAX_PATH, L"layout_audit.txt");
        if (FILE* f = _wfopen(exePath, L"w, ccs=UTF-8")) {
            fputws(msg.c_str(), f);
            fclose(f);
            msg += L"\n";
            msg += T("Report saved to:");
            msg += L"\n";
            msg += exePath;
        }
    }

    MessageBoxW(g_hwnd, msg.c_str(), T("Layout audit"), MB_OK | MB_ICONWARNING);
}
