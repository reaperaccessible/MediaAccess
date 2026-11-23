// Crossfade, phase 5 — volume analysis (see include/mediaaccess/loudness.h).
#include "mediaaccess/loudness.h"
#include "globals.h"
#include "utils.h"
#include "accessibility.h"
#include "player.h"
#include "database.h"
#include "resource.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/video_engine.h"
#include "mediaaccess/disc_drives.h"
#include "bass.h"
#include <atomic>
#include <cmath>
#include <map>
#include <thread>
#include <algorithm>

namespace mediaaccess {

namespace {

// One biquad, direct form I, per channel.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double Run(double x) {
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

// ITU-R BS.1770 K-weighting at any sample rate (the coefficients of
// libebur128, derived from the 48 kHz filters).
void KWeighting(double fs, Biquad& shelf, Biquad& highpass) {
    const double kPi = 3.14159265358979323846;
    double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    double K = tan(kPi * f0 / fs);
    double Vh = pow(10.0, G / 20.0);
    double Vb = pow(Vh, 0.4996667741545416);
    double a0 = 1.0 + K / Q + K * K;
    shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
    shelf.b1 = 2.0 * (K * K - Vh) / a0;
    shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
    shelf.a1 = 2.0 * (K * K - 1.0) / a0;
    shelf.a2 = (1.0 - K / Q + K * K) / a0;
    f0 = 38.13547087602444; Q = 0.5003270373238773;
    K = tan(kPi * f0 / fs);
    a0 = 1.0 + K / Q + K * K;
    highpass.b0 = 1.0; highpass.b1 = -2.0; highpass.b2 = 1.0;
    highpass.a1 = 2.0 * (K * K - 1.0) / a0;
    highpass.a2 = (1.0 - K / Q + K * K) / a0;
}

bool IsAnalysable(const std::wstring& path) {
    const wchar_t* p = path.c_str();
    if (path.empty() || IsURL(p) || IsVideoFile(path) || IsMpvOnlyAudioFile(path) || IsCdaFile(p))
        return false;
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

struct BatchResult {
    std::wstring path;
    LoudnessInfo info;
    bool hasTag = false;      // the file carries its own gain: not analysed
};

std::vector<std::wstring> s_queue;
std::atomic<size_t> s_nextIndex{0};
std::atomic<int>    s_active{0};
volatile bool       s_cancel = false;
bool                s_running = false;     // UI thread
int                 s_total = 0;           // UI thread
int                 s_done = 0;            // UI thread: files finished (any outcome)
int                 s_measured = 0;        // UI thread: files actually measured
std::vector<BatchResult> s_results;        // UI thread

void Worker() {
    for (;;) {
        if (s_cancel) break;
        size_t i = s_nextIndex.fetch_add(1);
        if (i >= s_queue.size()) break;
        auto* r = new BatchResult;
        r->path = s_queue[i];
        HSTREAM tagProbe = BASS_StreamCreateFile(FALSE, r->path.c_str(), 0, 0,
                                                 BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
        if (tagProbe) {
            r->hasTag = StreamHasOwnGain(tagProbe);
            BASS_StreamFree(tagProbe);
        }
        if (!r->hasTag) r->info = MeasureLoudness(r->path, &s_cancel);
        if (!g_hwnd || !PostMessage(g_hwnd, WM_LOUDNESS_RESULT, 1, reinterpret_cast<LPARAM>(r))) delete r;
    }
    // The last worker out says the batch is over.
    if (s_active.fetch_sub(1) == 1 && g_hwnd) PostMessage(g_hwnd, WM_LOUDNESS_RESULT, 0, 0);
}

std::string Fmt(const char* key, int a, int b = 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), Ts(key).c_str(), a, b);
    return buf;
}

// Album gains of the batch: tracks of the same folder and album tag, energy
// average of their loudness weighted by duration; album peak = highest peak.
void SaveAlbumGains() {
    struct Acc { double energy = 0, seconds = 0, peak = 0; std::vector<size_t> members; };
    std::map<std::wstring, Acc> albums;
    for (size_t i = 0; i < s_results.size(); ++i) {
        const BatchResult& r = s_results[i];
        if (!r.info.ok || r.info.album.empty() || r.info.seconds <= 0) continue;
        std::wstring dir = r.path.substr(0, r.path.find_last_of(L"\\/") + 1);
        std::wstring key = dir + L"|" + Utf8ToWide(r.info.album);
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        Acc& a = albums[key];
        a.energy += r.info.seconds * pow(10.0, r.info.lufs / 10.0);
        a.seconds += r.info.seconds;
        a.peak = (std::max)(a.peak, r.info.peak);
        a.members.push_back(i);
    }
    for (auto& kv : albums) {
        const Acc& a = kv.second;
        if (a.members.size() < 2 || a.seconds <= 0) continue;
        double albumLufs = 10.0 * log10(a.energy / a.seconds);
        for (size_t i : a.members) {
            const BatchResult& r = s_results[i];
            LoudnessRow row;
            row.lufs = r.info.lufs;
            row.peak = r.info.peak;
            row.seconds = r.info.seconds;
            row.hasAlbum = true;
            row.albumLufs = albumLufs;
            row.albumPeak = a.peak;
            SaveLoudnessDB(r.path, row);
        }
    }
}

} // namespace

LoudnessInfo MeasureLoudness(const std::wstring& path, const volatile bool* cancel) {
    LoudnessInfo out;
    HSTREAM h = BASS_StreamCreateFile(FALSE, path.c_str(), 0, 0,
                                      BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    if (!h) return out;
    BASS_CHANNELINFO ci = {};
    BASS_ChannelGetInfo(h, &ci);
    if (!ci.freq || !ci.chans) { BASS_StreamFree(h); return out; }
    out.album = StreamAlbumTag(h);
    const int ch = static_cast<int>(ci.chans);
    // BS.1770 channel weights: L, R, C = 1, LFE = 0, surrounds = 1.41. A mono
    // file is heard on both speakers: counted twice.
    std::vector<double> weight(ch, 1.0);
    if (ch == 1) weight[0] = 2.0;
    if (ch >= 6) { weight[3] = 0.0; weight[4] = 1.41; weight[5] = 1.41; }
    std::vector<Biquad> shelf(ch), hp(ch);
    for (int c = 0; c < ch; ++c) KWeighting(ci.freq, shelf[c], hp[c]);

    const size_t subFrames = ci.freq / 10;            // 100 ms
    std::vector<double> subEnergy;                     // weighted mean square per 100 ms
    std::vector<float> buf(4096 * ch);
    double acc = 0.0, peak = 0.0;
    size_t accFrames = 0, totalFrames = 0;
    for (;;) {
        if (cancel && *cancel) { BASS_StreamFree(h); return out; }
        DWORD got = BASS_ChannelGetData(h, buf.data(), static_cast<DWORD>(buf.size() * sizeof(float)) | BASS_DATA_FLOAT);
        if (got == (DWORD)-1 || got == 0) break;
        size_t frames = got / sizeof(float) / ch;
        for (size_t f = 0; f < frames; ++f) {
            double e = 0.0;
            for (int c = 0; c < ch; ++c) {
                double x = buf[f * ch + c];
                double ax = fabs(x);
                if (ax > peak) peak = ax;
                double y = hp[c].Run(shelf[c].Run(x));
                e += weight[c] * y * y;
            }
            acc += e;
            if (++accFrames == subFrames) {
                subEnergy.push_back(acc / subFrames);
                acc = 0.0;
                accFrames = 0;
            }
        }
        totalFrames += frames;
    }
    BASS_StreamFree(h);
    out.seconds = static_cast<double>(totalFrames) / ci.freq;
    out.peak = peak;
    // 400 ms blocks every 100 ms, then the two gates.
    std::vector<double> blocks;
    for (size_t i = 0; i + 4 <= subEnergy.size(); ++i)
        blocks.push_back((subEnergy[i] + subEnergy[i + 1] + subEnergy[i + 2] + subEnergy[i + 3]) / 4.0);
    auto lufsOf = [](double e) { return -0.691 + 10.0 * log10(e > 1e-20 ? e : 1e-20); };
    double sum = 0.0;
    size_t n = 0;
    for (double b : blocks) if (lufsOf(b) > -70.0) { sum += b; ++n; }
    if (!n) return out;                                // silence: nothing to say
    double relGate = lufsOf(sum / n) - 10.0;
    sum = 0.0; n = 0;
    for (double b : blocks) if (lufsOf(b) > -70.0 && lufsOf(b) > relGate) { sum += b; ++n; }
    if (!n) return out;
    out.lufs = lufsOf(sum / n);
    out.ok = true;
    return out;
}

bool LoudnessBatchRunning() { return s_running; }

void LoudnessBatchToggle(const std::vector<std::wstring>& paths) {
    if (s_running) {                                   // the same key stops it
        s_cancel = true;
        return;                                        // said when the workers are out
    }
    s_queue.clear();
    for (const std::wstring& p : paths) {
        if (!IsAnalysable(p)) continue;
        LoudnessRow row;
        if (LoadLoudnessDB(p, row)) continue;          // measured already
        if (std::find(s_queue.begin(), s_queue.end(), p) == s_queue.end()) s_queue.push_back(p);
    }
    if (s_queue.empty()) {
        Speak(Ts("No track to analyse"));
        return;
    }
    s_cancel = false;
    s_running = true;
    s_total = static_cast<int>(s_queue.size());
    s_done = 0;
    s_measured = 0;
    s_results.clear();
    s_nextIndex = 0;
    // Started by the user: up to 80 % of the processor, eight threads at most.
    unsigned cores = std::thread::hardware_concurrency();
    int threads = (std::max)(1, (std::min)({ static_cast<int>(cores * 8 / 10), 8, s_total }));
    s_active = threads;
    for (int i = 0; i < threads; ++i) std::thread(Worker).detach();
    LogF("Loudness", "analysis of %d tracks, %d threads", s_total, threads);
    Speak(s_total == 1 ? Ts("Analysing the volume of 1 track") : Fmt("Analysing the volume of %d tracks", s_total));
}

void LoudnessBatchSpeakProgress() {
    if (!s_running) {
        Speak(Ts("No volume analysis running"));
        return;
    }
    Speak(Fmt("Volume analysis: %d of %d", s_done, s_total));
}

void HandleLoudnessResult(LPARAM lParam) {
    if (lParam) {
        auto* r = reinterpret_cast<BatchResult*>(lParam);
        ++s_done;
        if (r->hasTag) ++s_measured;   // its volume is known: it counts as analysed
        if (r->info.ok) {
            ++s_measured;
            LoudnessRow row;
            row.lufs = r->info.lufs;
            row.peak = r->info.peak;
            row.seconds = r->info.seconds;
            SaveLoudnessDB(r->path, row);
            LogF("Loudness", "%.2f LUFS, peak %.3f, %.1f s: %ls", r->info.lufs, r->info.peak,
                 r->info.seconds, r->path.c_str());
        }
        s_results.push_back(std::move(*r));
        delete r;
        return;
    }
    // All the workers are out.
    if (!s_running) return;
    s_running = false;
    SaveAlbumGains();
    const int n = s_measured;
    std::string msg;
    if (s_cancel) msg = n == 1 ? Ts("Volume analysis stopped, 1 track analysed")
                               : Fmt("Volume analysis stopped, %d tracks analysed", n);
    else          msg = n == 1 ? Ts("1 track analysed") : Fmt("%d tracks analysed", n);
    s_results.clear();
    s_queue.clear();
    Speak(msg);
    CrossfadeGainRefresh();   // the song playing may have just been measured
}

void LoudnessBatchShutdown() {
    s_cancel = true;
    for (int i = 0; i < 500 && s_active.load() > 0; ++i) Sleep(10);
}

} // namespace mediaaccess
