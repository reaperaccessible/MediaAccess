#pragma once
// Crossfade, phase 5 — "Égaliser le volume" (docs/PLAN_FONDU_ENCHAINE.md).
//
// MeasureLoudness: integrated loudness of a whole file (EBU R128 / ITU-R
// BS.1770: K-weighting, 400 ms blocks every 100 ms, absolute gate -70 LUFS,
// relative gate -10 LU) and its sample peak. Heavy: only ever run by the user
// ("Analyse the volume of the tracks", playlist manager), never by itself.
//
// The batch runs in worker threads; each result is posted to the main window
// (WM_LOUDNESS_RESULT) and kept in the database (table `loudness`), so a file
// is analysed once. ReplayGain 2.0 reference: gain = -18 - LUFS.
#include <windows.h>
#include <string>
#include <vector>

namespace mediaaccess {

struct LoudnessInfo {
    bool   ok = false;
    double lufs = 0.0;      // integrated loudness of the track
    double peak = 0.0;      // sample peak, linear (1.0 = full scale)
    double seconds = 0.0;
    std::string album;      // album tag (UTF-8), to group album gains
};

// Whole-file measure on the calling thread (a worker). `cancel` may be null.
LoudnessInfo MeasureLoudness(const std::wstring& path, const volatile bool* cancel);

// Playlist manager commands.
void LoudnessBatchToggle(const std::vector<std::wstring>& paths);  // start, or stop if running
void LoudnessBatchSpeakProgress();
bool LoudnessBatchRunning();

// WM_LOUDNESS_RESULT (main window): one file done (lParam = heap result).
void HandleLoudnessResult(LPARAM lParam);

// Waits for the workers to stop (application exit, before BASS goes).
void LoudnessBatchShutdown();

} // namespace mediaaccess
