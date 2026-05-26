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

void DoSpeak() {
    if (!g_speechInitialized) return;

    EnterCriticalSection(&g_speechCS);

    if (g_speechQueue.empty()) {
        LeaveCriticalSection(&g_speechCS);
        return;
    }

    // Determine interrupt from the first entry
    bool interrupt = g_speechQueue.front().interrupt;

    // Concatenate all queued messages
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
