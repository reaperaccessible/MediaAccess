// =============================================================================
// accessibility.cpp — screen-reader output routed through UniversalSpeech.
//
// UniversalSpeech tries (in priority order) NVDA → JAWS → Window-Eyes →
// Microsoft SAPI when none of the dedicated screen readers are running.
// We don't pick a backend ourselves — speechSay() picks whichever is alive.
//
// Why the queue + WM_SPEAK indirection: callers fire Speak() from any
// thread (BASS sync callbacks, background download threads, etc.) but the
// screen reader bridges must be invoked on the UI thread. We push the text
// onto a guarded queue and post WM_SPEAK so DoSpeak() drains it on the
// main thread. Multiple announcements that pile up between drains get
// concatenated with ", " so the user hears one fluent utterance instead of
// a stuttering sequence.
// =============================================================================

#include "accessibility.h"
#include "globals.h"
#include "resource.h"
#include "utils.h"
#include <string>
#include <queue>

#ifdef USE_UNIVERSAL_SPEECH
#include "UniversalSpeech.h"

struct SpeechEntry {
    std::wstring text;
    bool interrupt;
};

static std::queue<SpeechEntry> g_speechQueue;
static CRITICAL_SECTION g_speechCS;
static bool g_speechCSInitialized = false;
static bool g_speechInitialized = false;

// Drain the queue and speak. Called on the UI thread in response to
// WM_SPEAK. The "interrupt" flag of the FIRST queued message wins — if any
// upstream caller asked to interrupt, we stop the current utterance before
// speaking the merged batch.
void DoSpeak() {
    if (!g_speechInitialized) return;
    // v1.77 — Suppress any speech once shutdown has started. WM_SPEAK
    // messages posted before WM_DESTROY can still be in the queue when
    // we get here; without this guard, they would compete with the
    // screen reader's own focus-transfer announcement (NVDA reading the
    // window that takes focus when MediaAccess closes) and either
    // truncate it or be truncated by it. Reported by user Gilles after
    // Alt+F4 on a playing file in his Downloads folder: NVDA started
    // reading "Téléchargements" but only "téléchar" came through.
    if (g_isShuttingDown) return;

    EnterCriticalSection(&g_speechCS);

    if (g_speechQueue.empty()) {
        LeaveCriticalSection(&g_speechCS);
        return;
    }

    bool interrupt = g_speechQueue.front().interrupt;

    // Merge all queued messages into a single utterance so rapid-fire
    // calls (e.g. effect tweaks with key auto-repeat) don't stutter.
    std::wstring combined;
    while (!g_speechQueue.empty()) {
        if (!combined.empty()) combined += L", ";
        combined += g_speechQueue.front().text;
        g_speechQueue.pop();
    }

    LeaveCriticalSection(&g_speechCS);

    if (!combined.empty()) {
        if (interrupt) {
            speechStop();
        }
        speechSay(combined.c_str(), interrupt ? 1 : 0);
    }
}

// Thread-safe: enqueue + wake the UI thread via WM_SPEAK. Drops the
// message silently if speech isn't initialized or the main HWND is gone
// (e.g. shutdown is in progress).
void Speak(const char* text, bool interrupt) {
    // v1.77 — Drop late callers (BASS sync threads, scheduler ticks, ICY
    // metadata pushes, etc.) once shutdown has started. Belt-and-braces
    // with the DoSpeak() guard above: this stops new entries reaching the
    // queue in the first place, that one stops already-queued entries
    // from being spoken when the WM_SPEAK is finally drained.
    if (g_isShuttingDown) return;
    if (g_speechInitialized && g_hwnd) {
        EnterCriticalSection(&g_speechCS);
        g_speechQueue.push({Utf8ToWide(text), interrupt});
        LeaveCriticalSection(&g_speechCS);
        PostMessage(g_hwnd, WM_SPEAK, 0, 0);
    }
}

void Speak(const std::string& text, bool interrupt) {
    Speak(text.c_str(), interrupt);
}

void SpeakW(const wchar_t* text, bool interrupt) {
    if (g_isShuttingDown) return;   // v1.77 — see Speak() comment above
    if (g_speechInitialized && g_hwnd) {
        EnterCriticalSection(&g_speechCS);
        g_speechQueue.push({text, interrupt});
        LeaveCriticalSection(&g_speechCS);
        PostMessage(g_hwnd, WM_SPEAK, 0, 0);
    }
}

void SpeakW(const std::wstring& text, bool interrupt) {
    SpeakW(text.c_str(), interrupt);
}

bool InitSpeech(HWND hwnd) {
    (void)hwnd;  // Not needed for Universal Speech
    if (!g_speechCSInitialized) {
        InitializeCriticalSection(&g_speechCS);
        g_speechCSInitialized = true;
    }
    g_speechInitialized = true;
    return true;
}

void FreeSpeech() {
    // v1.77 — Do NOT call speechStop() here. speechStop() is a GLOBAL
    // screen-reader stop (UniversalSpeech API), not a "stop MediaAccess
    // queue" — it purges whatever utterance is currently being spoken by
    // NVDA / JAWS / Narrator, even when the queue isn't ours.
    //
    // When MediaAccess closes (WM_DESTROY → FreeSpeech), the screen
    // reader is in the middle of announcing whatever takes focus next
    // (the previous app, Explorer if launched from there, the Start
    // menu, etc.). Calling speechStop() would cut that announcement
    // mid-word. User Gilles reported hearing "téléchar..." instead of
    // "Téléchargements" after Alt+F4 on a file in his Downloads folder.
    //
    // The DoSpeak() and Speak() guards already prevent any late
    // MediaAccess speech from going out once g_isShuttingDown is set,
    // so we don't need a global purge here.
    if (g_speechInitialized) {
        g_speechInitialized = false;
    }
    if (g_speechCSInitialized) {
        // Drain any remaining MediaAccess-queued entries (do not touch
        // what the screen reader itself may be saying).
        EnterCriticalSection(&g_speechCS);
        while (!g_speechQueue.empty()) g_speechQueue.pop();
        LeaveCriticalSection(&g_speechCS);
        DeleteCriticalSection(&g_speechCS);
        g_speechCSInitialized = false;
    }
}

#else

void DoSpeak() {}
void Speak(const char*, bool) {}
void Speak(const std::string&, bool) {}
void SpeakW(const wchar_t*, bool) {}
void SpeakW(const std::wstring&, bool) {}
bool InitSpeech(HWND) { return false; }
void FreeSpeech() {}

#endif
