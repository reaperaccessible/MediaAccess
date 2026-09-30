#pragma once
#ifndef MEDIAACCESS_NOTIFY_SOUNDS_H
#define MEDIAACCESS_NOTIFY_SOUNDS_H

// v2.74 — download-finished sounds (GitHub issue #16). See
// docs/PLAN_SONS_TELECHARGEMENT.md (local).

#include <string>

// Path of a built-in sound: <exe dir>\sounds\download_success.wav / _failure.wav.
std::wstring BuiltinDownloadSoundPath(bool success);

// Play |path| once, mixed with whatever plays (never pauses playback). Empty
// path = the built-in sound for |success|. A missing or unreadable file falls
// back to the built-in one. A new sound stops the previous one; a long file
// fades out after 5 seconds. UI thread only. Returns false when the chosen
// file could not be used (the built-in sound played instead) or nothing played.
bool PlayNotifySoundFile(const std::wstring& path, bool success);

// Called when a user download ends: plays the chosen sound when
// "Play a sound when a download finishes" is on, else does nothing.
void PlayDownloadSound(bool success);

#endif // MEDIAACCESS_NOTIFY_SOUNDS_H
