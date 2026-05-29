#pragma once
#ifndef MEDIAACCESS_TTS_PLAYER_H
#define MEDIAACCESS_TTS_PLAYER_H

// =============================================================================
// tts_player.h — SAPI 5 text-to-speech wrapper for the DAISY book reader
//
// Independent from NVDA's screen-reader announcements (those keep going
// through accessibility.cpp's Speak() to NVDA Controller Client). This is
// the long-form narrator used to read text-only DAISY and EPUB books
// continuously. Audio output goes to the Windows default audio device
// (whatever SAPI's voice was configured to use); MediaAccess does not
// route it through BASS in Phase 2.
//
// Threading: SAPI events fire on a worker thread. We translate the
// "end-of-input-stream" notification into a PostMessage on the main
// window so the DAISY player can advance to the next paragraph safely.
// =============================================================================

#include <string>
#include <vector>

namespace mediaaccess {

struct TtsVoice {
    std::wstring id;            // SAPI token id (registry path) — opaque, used by SetActiveVoiceId
    std::wstring displayName;   // Human label, e.g. "Microsoft Hortense Desktop - French"
    std::wstring language;      // "en-US", "fr-FR", etc. ("" if SAPI didn't expose it)
};

// =============================================================================
// Initialization (call once at startup, after CoInitializeEx)
// =============================================================================
bool TtsInit();
void TtsShutdown();

// =============================================================================
// Voice enumeration and selection
// =============================================================================

// List every SAPI voice installed on the machine. Empty vector if SAPI is
// not available. Cached after first call — recompute only across TtsShutdown
// / TtsInit boundaries.
std::vector<TtsVoice> TtsListVoices();

// Set the active voice by its token id. Returns false if id not found.
// The choice is persisted in MediaAccess.ini under [Books] TtsVoiceId so
// it survives restarts. Falls back to the SAPI default voice if id is
// empty or unrecognized.
bool TtsSetActiveVoiceId(const std::wstring& id);

// Returns the currently active voice id (empty = SAPI default).
std::wstring TtsGetActiveVoiceId();

// =============================================================================
// Speech speed
// =============================================================================
//
// Accepts MediaAccess's multiplier convention (1.0 = normal, 2.0 = twice as
// fast, 0.5 = half speed). Internally maps to SAPI's -10..+10 integer rate
// using a logarithmic curve so the perceived speed change is linear with
// the multiplier. The DAISY player calls this from its Rate effect handler.

void TtsSetSpeedMultiplier(double multiplier);
double TtsGetSpeedMultiplier();

// =============================================================================
// Speech control
// =============================================================================

// Speak `text` asynchronously. Replaces any in-flight utterance. Does NOT
// queue — the caller (DaisyPlayer) is responsible for sending the next
// paragraph when EndOfStream fires.
bool TtsSpeak(const std::wstring& text);

// Cancel current speech immediately. No EndOfStream notification.
void TtsStop();

// Pause / resume the current utterance. SAPI supports SPVES_PAUSED.
void TtsPause();
void TtsResume();

bool TtsIsSpeaking();
bool TtsIsPaused();

// =============================================================================
// End-of-stream notification
// =============================================================================
//
// When the in-flight utterance finishes naturally, MediaAccess receives a
// WM_TTS_END_OF_STREAM (WM_USER + 51) on the main window. The DaisyPlayer's
// WndProc handler maps that to "advance to next paragraph".
//
// Define the constant here so both producer and consumer agree.
constexpr unsigned int WM_TTS_END_OF_STREAM = 0x0400 + 51;  // WM_USER + 51

} // namespace mediaaccess

#endif // MEDIAACCESS_TTS_PLAYER_H
