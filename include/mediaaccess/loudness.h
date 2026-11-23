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

// The same measure fed in pieces (CD extraction: the PCM as it is read).
class LoudnessMeter {
public:
    void Begin(unsigned freq, int chans);
    void Add(const float* interleaved, size_t frames);
    // false: nothing above the gates (silence) or nothing fed.
    bool Result(double& lufs, double& peak, double& seconds) const;
private:
    struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    unsigned m_freq = 0;
    int m_chans = 0;
    std::vector<Biquad> m_shelf, m_hp;
    std::vector<double> m_weight, m_sub;
    double m_acc = 0.0, m_peak = 0.0;
    size_t m_accFrames = 0, m_frames = 0;
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
