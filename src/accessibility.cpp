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
    if (g_speechInitialized) {
        speechStop();
        g_speechInitialized = false;
    }
    if (g_speechCSInitialized) {
        // Drain any remaining entries
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
