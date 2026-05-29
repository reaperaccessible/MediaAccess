#pragma once
#ifndef MEDIAACCESS_SLEEP_TIMER_H
#define MEDIAACCESS_SLEEP_TIMER_H

// =============================================================================
// sleep_timer.h — countdown that stops playback after N minutes
//
// Phase 4 (v1.55). Tick via WM_TIMER (IDT_SLEEP_TIMER) on the main window,
// 1-second granularity. Survives system sleep (uses GetTickCount64). Calls
// the appropriate Stop function depending on the active engine (DAISY, TTS,
// BASS audio, MPV video) on expiry. Optional 30-second linear fadeout
// applied via the global g_volume + SetVolume() path so every engine fades
// uniformly.
//
// Not persisted across sessions — each launch starts with no timer.
// =============================================================================

#include <windows.h>

namespace mediaaccess {

// Start (or restart) the timer for `minutes` minutes. minutes must be > 0.
// Speaks "Sleep timer set for N minutes" via the standard Speak() infra.
void SleepStart(int minutes);

// Cancel an active timer; restore the saved volume if a fade was in
// progress. Speaks "Sleep timer cancelled" if there was one to cancel,
// otherwise speaks "No sleep timer active".
void SleepCancel();

// True while a timer is counting down (regardless of whether the fade has
// started yet).
bool SleepIsActive();

// Speak the remaining time ("12 minutes 5 seconds remaining") or
// "No sleep timer active" if none is running.
void SleepSpeakRemaining();

// Called from the main window's WM_TIMER handler when wParam == IDT_SLEEP_TIMER.
// Drives countdown, fadeout, and expiry. Idempotent if no timer is active.
void SleepOnTick();

// Called from WM_DESTROY so the timer is cleanly killed and the saved
// volume is restored if a fade was running.
void SleepShutdown();

// Read-only accessor — used by the status bar to display "Sleep: 12:05"
// when a timer is active. Returns total remaining seconds, or -1 if no
// timer is running.
int SleepRemainingSeconds();

// Whether a fadeout is currently being applied (last 30 s of the timer).
// Used by the status bar / debugging only.
bool SleepIsFading();

// Preference accessor (loaded from settings).
bool SleepFadeEnabled();
void SleepSetFadeEnabled(bool enabled);

// Open the custom-duration dialog. minutesOut receives the typed value if
// the user clicked OK. Returns true on OK with a valid value.
bool SleepPromptCustomMinutes(HWND owner, int& minutesOut);

} // namespace mediaaccess

#endif // MEDIAACCESS_SLEEP_TIMER_H
