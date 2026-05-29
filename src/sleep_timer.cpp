// =============================================================================
// sleep_timer.cpp — countdown that stops playback after N minutes
//
// Design notes:
//   - Tick on WM_TIMER (IDT_SLEEP_TIMER) every 1 second from the main window.
//   - End time stored as GetTickCount64() value so system sleep doesn't drift
//     the countdown.
//   - Fade is applied by writing g_volume directly and calling SetVolume(),
//     which routes to BASS / MPV / DAISY uniformly. On cancel/expiry/restart
//     we restore the volume we sampled at SleepStart() time. User volume
//     adjustments during the fade are overwritten on next tick — documented.
// =============================================================================

#include "mediaaccess/sleep_timer.h"
#include "mediaaccess/player.h"
#include "mediaaccess/daisy_player.h"
#include "mediaaccess/tts_player.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/globals.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <string>

// Globals owned elsewhere.
extern HWND  g_hwnd;
extern float g_volume;
extern std::wstring g_configPath;

namespace mediaaccess {

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

struct SleepState {
    bool       active        = false;
    ULONGLONG  endTickCount  = 0;     // GetTickCount64 milliseconds at expiry
    float      savedVolume   = 1.0f;
    bool       volumeSaved   = false;
    bool       fadeActive    = false; // true while we're inside the 30 s fade
    bool       fadeEnabled   = true;  // user preference
};

static SleepState g_s;
static const int kFadeWindowSec = 30;

// -----------------------------------------------------------------------------
// Settings persistence — only the fade-enabled preference is persisted.
// -----------------------------------------------------------------------------

static bool LoadFadePref() {
    if (g_configPath.empty()) return true;
    return GetPrivateProfileIntW(L"SleepTimer", L"FadeEnabled", 1,
                                 g_configPath.c_str()) != 0;
}

static void SaveFadePref(bool enabled) {
    if (g_configPath.empty()) return;
    WritePrivateProfileStringW(L"SleepTimer", L"FadeEnabled",
                               enabled ? L"1" : L"0", g_configPath.c_str());
}

bool SleepFadeEnabled() {
    static bool loaded = false;
    if (!loaded) { g_s.fadeEnabled = LoadFadePref(); loaded = true; }
    return g_s.fadeEnabled;
}

void SleepSetFadeEnabled(bool enabled) {
    g_s.fadeEnabled = enabled;
    SaveFadePref(enabled);
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void SaveVolumeIfNeeded() {
    if (!g_s.volumeSaved) {
        g_s.savedVolume = g_volume;
        g_s.volumeSaved = true;
    }
}

static void RestoreVolume() {
    if (g_s.volumeSaved) {
        g_volume = g_s.savedVolume;
        SetVolume(g_volume);
        g_s.volumeSaved = false;
    }
    g_s.fadeActive = false;
}

// Unified Stop dispatch — only one playback engine is active at a time in
// MediaAccess (single-instance, single-stream architecture).
static void StopActiveEngine() {
    if (DaisyIsActive()) {
        DaisyStop();
    } else if (TtsIsSpeaking()) {
        TtsStop();
    } else {
        Stop();   // handles BASS + MPV via g_activeEngine
    }
}

static std::wstring FormatRemainingSpeak(int seconds) {
    if (seconds < 0) seconds = 0;
    int m = seconds / 60;
    int s = seconds % 60;
    wchar_t buf[128];
    if (m > 0 && s > 0) {
        swprintf(buf, 128, L"%d %s %d %s %s",
                 m, (m > 1) ? T("minutes") : T("minute"),
                 s, (s > 1) ? T("seconds") : T("second"),
                 T("remaining"));
    } else if (m > 0) {
        swprintf(buf, 128, L"%d %s %s",
                 m, (m > 1) ? T("minutes") : T("minute"),
                 T("remaining"));
    } else {
        swprintf(buf, 128, L"%d %s %s",
                 s, (s > 1) ? T("seconds") : T("second"),
                 T("remaining"));
    }
    return buf;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void SleepStart(int minutes) {
    if (minutes <= 0) return;
    // If a timer was already running, restore the original volume so we
    // don't accumulate fade state across restarts.
    if (g_s.active) RestoreVolume();

    g_s.active        = true;
    g_s.endTickCount  = GetTickCount64() + (ULONGLONG)minutes * 60ULL * 1000ULL;
    g_s.fadeActive    = false;
    SaveVolumeIfNeeded();

    if (g_hwnd) SetTimer(g_hwnd, IDT_SLEEP_TIMER, 1000, nullptr);

    wchar_t msg[128];
    if (minutes >= 60 && minutes % 60 == 0) {
        int h = minutes / 60;
        swprintf(msg, 128, L"%s %d %s", T("Sleep timer set for"),
                 h, (h > 1) ? T("hours") : T("hour"));
    } else {
        swprintf(msg, 128, L"%s %d %s", T("Sleep timer set for"),
                 minutes, (minutes > 1) ? T("minutes") : T("minute"));
    }
    SpeakW(msg);
}

void SleepCancel() {
    if (!g_s.active) {
        Speak(Ts("No sleep timer active"));
        return;
    }
    if (g_hwnd) KillTimer(g_hwnd, IDT_SLEEP_TIMER);
    RestoreVolume();
    g_s.active = false;
    Speak(Ts("Sleep timer cancelled"));
}

bool SleepIsActive()   { return g_s.active; }
bool SleepIsFading()   { return g_s.fadeActive; }

int SleepRemainingSeconds() {
    if (!g_s.active) return -1;
    ULONGLONG now = GetTickCount64();
    if (now >= g_s.endTickCount) return 0;
    return (int)((g_s.endTickCount - now) / 1000ULL);
}

void SleepSpeakRemaining() {
    if (!g_s.active) { Speak(Ts("No sleep timer active")); return; }
    SpeakW(FormatRemainingSpeak(SleepRemainingSeconds()));
}

void SleepShutdown() {
    if (g_s.active && g_hwnd) KillTimer(g_hwnd, IDT_SLEEP_TIMER);
    RestoreVolume();
    g_s.active = false;
}

void SleepOnTick() {
    if (!g_s.active) return;
    int remaining = SleepRemainingSeconds();

    // Expiry — kill timer first, then stop, then restore volume so the user
    // doesn't briefly hear the original level before silence.
    if (remaining <= 0) {
        if (g_hwnd) KillTimer(g_hwnd, IDT_SLEEP_TIMER);
        g_s.active = false;
        StopActiveEngine();
        RestoreVolume();
        Speak(Ts("Sleep timer expired"));
        return;
    }

    // Fade window (last kFadeWindowSec seconds): apply a linear ramp from
    // savedVolume down to 0. SetVolume() routes to BASS/MPV; DaisyApplyVolume()
    // is called by the next ChannelPlay anyway. For currently-playing audio
    // we set the global g_volume and re-apply.
    if (g_s.fadeEnabled && remaining <= kFadeWindowSec) {
        SaveVolumeIfNeeded();
        g_s.fadeActive = true;
        float factor = (float)remaining / (float)kFadeWindowSec;
        if (factor < 0.0f) factor = 0.0f;
        g_volume = g_s.savedVolume * factor;
        SetVolume(g_volume);
    }
}

// -----------------------------------------------------------------------------
// Custom-duration dialog
// -----------------------------------------------------------------------------

static int s_customMinutesOut = 0;

static INT_PTR CALLBACK CustomMinutesDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            SetDlgItemTextW(dlg, IDC_SLEEP_CUSTOM_EDIT, L"30");
            SendDlgItemMessageW(dlg, IDC_SLEEP_CUSTOM_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, IDC_SLEEP_CUSTOM_EDIT));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[32] = {0};
                GetDlgItemTextW(dlg, IDC_SLEEP_CUSTOM_EDIT, buf, 32);
                int v = _wtoi(buf);
                if (v <= 0 || v > 1440) {
                    MessageBoxW(dlg,
                        T("Please enter a number of minutes between 1 and 1440."),
                        T("Sleep timer"), MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                s_customMinutesOut = v;
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

bool SleepPromptCustomMinutes(HWND owner, int& minutesOut) {
    s_customMinutesOut = 0;
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDD_SLEEP_TIMER),
                           owner, CustomMinutesDlgProc);
    if (r != IDOK || s_customMinutesOut <= 0) return false;
    minutesOut = s_customMinutesOut;
    return true;
}

} // namespace mediaaccess
