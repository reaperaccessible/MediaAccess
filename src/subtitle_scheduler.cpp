// =============================================================================
// subtitle_scheduler.cpp — subtitle parsing (and, later, prefetch scheduling).
// See subtitle_scheduler.h for the design.
// =============================================================================

#include "mediaaccess/subtitle_scheduler.h"
#include "mediaaccess/utils.h"          // Utf8ToWide
#include "mediaaccess/edge_tts_client.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/youtube.h"        // GetFfmpegLocation
#include <windows.h>
#include "bass.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace mediaaccess {

namespace {

// Split into lines on '\n', dropping a trailing '\r' (CRLF) and any UTF-8 BOM.
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    // Skip UTF-8 BOM if present.
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        start = 3;
    std::string cur;
    for (size_t i = start; i < s.size(); i++) {
        char c = s[i];
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

bool IsBlank(const std::string& s) {
    for (char c : s) if (!isspace((unsigned char)c)) return false;
    return true;
}

// Parse "HH:MM:SS,mmm" / "MM:SS.mmm" (hours optional, ',' or '.' for ms) to sec.
// Returns -1 on failure.
double ParseTimecode(const std::string& tc) {
    int parts[3] = {0, 0, 0};   // we fill from the right: sec, min, hour
    int ms = 0;
    std::string t;
    for (char c : tc) if (!isspace((unsigned char)c)) t += c;  // trim spaces
    if (t.empty()) return -1;

    // Split off milliseconds at ',' or '.'.
    size_t dot = t.find_first_of(",.");
    std::string hms = (dot == std::string::npos) ? t : t.substr(0, dot);
    if (dot != std::string::npos) {
        std::string msStr = t.substr(dot + 1);
        if (msStr.empty()) return -1;
        // Pad/truncate to 3 digits.
        msStr = (msStr + "000").substr(0, 3);
        for (char c : msStr) { if (!isdigit((unsigned char)c)) return -1; ms = ms * 10 + (c - '0'); }
    }

    // Split hms on ':' from the right into up to 3 fields.
    int nfields = 0;
    size_t end = hms.size();
    while (end != std::string::npos && nfields < 3) {
        size_t colon = hms.rfind(':', end == 0 ? std::string::npos : end - 1);
        std::string field = (colon == std::string::npos) ? hms.substr(0, end)
                                                          : hms.substr(colon + 1, end - colon - 1);
        if (field.empty()) return -1;
        int val = 0;
        for (char c : field) { if (!isdigit((unsigned char)c)) return -1; val = val * 10 + (c - '0'); }
        parts[nfields++] = val;
        if (colon == std::string::npos) break;
        end = colon;
    }
    // parts[0]=sec, [1]=min, [2]=hour
    return parts[2] * 3600.0 + parts[1] * 60.0 + parts[0] + ms / 1000.0;
}

// Remove markup: <...> (HTML/VTT tags) and {...} (SSA/ASS overrides).
std::string StripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int depthAngle = 0, depthBrace = 0;
    for (char c : s) {
        if (c == '<') { depthAngle++; continue; }
        if (c == '>') { if (depthAngle > 0) depthAngle--; continue; }
        if (c == '{') { depthBrace++; continue; }
        if (c == '}') { if (depthBrace > 0) depthBrace--; continue; }
        if (depthAngle == 0 && depthBrace == 0) out += c;
    }
    return out;
}

} // namespace

std::vector<SubCue> ParseSubtitles(const std::string& utf8Data) {
    std::vector<SubCue> cues;
    std::vector<std::string> lines = SplitLines(utf8Data);

    for (size_t i = 0; i < lines.size(); i++) {
        size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;

        double start = ParseTimecode(lines[i].substr(0, arrow));
        // End time is the token right after "-->"; ignore any VTT cue settings.
        std::string rest = lines[i].substr(arrow + 3);
        size_t e = 0; while (e < rest.size() && isspace((unsigned char)rest[e])) e++;
        size_t e2 = e; while (e2 < rest.size() && !isspace((unsigned char)rest[e2])) e2++;
        double end = ParseTimecode(rest.substr(e, e2 - e));
        if (start < 0 || end < 0) continue;

        // Gather text lines until a blank line (cue separator).
        std::wstring text;
        size_t j = i + 1;
        for (; j < lines.size() && !IsBlank(lines[j]); j++) {
            std::string clean = StripTags(lines[j]);
            if (clean.empty()) continue;
            if (!text.empty()) text += L' ';
            text += Utf8ToWide(clean);
        }
        i = j;  // continue scanning after this cue's text block

        if (end < start) std::swap(start, end);
        if (!text.empty()) cues.push_back({start, end, std::move(text)});
    }

    std::sort(cues.begin(), cues.end(),
              [](const SubCue& a, const SubCue& b) { return a.startSec < b.startSec; });
    return cues;
}

// =============================================================================
// Scheduler
//
// Threading model: SubStart/SubStop/SubOnTimePos/SubOnSeek/SubOnPause are all
// called from the app thread (the only thread that touches BASS). The worker
// thread only synthesizes and writes cue buffers under g_mtx; it never touches
// BASS or the playback fields (g_clipStream/g_curCue/g_ducked).
// =============================================================================

namespace {

enum CueState { ST_PENDING, ST_QUEUED, ST_SYNTH, ST_READY, ST_FAILED, ST_PLAYED };

struct SchedCue {
    SubCue                      cue;
    int                         state = ST_PENDING;
    int                         fails = 0;     // synth failures so far (bounded retry)
    std::vector<unsigned char>  mp3;
};

const int kMaxSynthRetries = 2;

std::mutex               g_mtx;
std::vector<SchedCue>    g_cues;
std::deque<int>          g_queue;
std::condition_variable  g_cv;
std::thread              g_worker;
bool                     g_running   = false;
std::string              g_voice;
std::string              g_rate = "+0%";
double                   g_lookahead = 2.5;
double                   g_duckLevel = 0.3;
double                   g_speed     = 1.0;     // current playback speed (for lookahead scaling)
SubDuckFn                g_duckFn    = nullptr;
SubFallbackFn            g_fallbackFn = nullptr;

// Playback state — app thread only.
HSTREAM g_clipStream = 0;
int     g_curCue     = -1;
bool    g_ducked     = false;

void Duck(double level) { if (g_duckFn) g_duckFn(level); }

void WorkerLoop() {
    for (;;) {
        int idx; std::string voice, rate; std::wstring text;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_running || !g_queue.empty(); });
            if (!g_running) return;
            idx = g_queue.front(); g_queue.pop_front();
            if (idx < 0 || idx >= (int)g_cues.size() || g_cues[idx].state != ST_QUEUED) continue;
            g_cues[idx].state = ST_SYNTH;
            voice = g_voice;
            text  = g_cues[idx].cue.text;
            rate  = g_rate;
        }
        std::vector<unsigned char> mp3; std::string err;
        bool ok = EdgeSynthesize(voice, text, mp3, rate, "+0Hz", &err);
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_running) return;
            if (idx >= 0 && idx < (int)g_cues.size()) {
                SchedCue& c = g_cues[idx];
                if (ok) { c.mp3 = std::move(mp3); c.state = ST_READY; }
                else {
                    // Bounded retry on transient failures (network blip / token);
                    // re-queue via PENDING until exhausted, then give up (FAILED).
                    c.fails++;
                    c.state = (c.fails <= kMaxSynthRetries) ? ST_PENDING : ST_FAILED;
                    LogF("subtts", "cue %d synth failed (try %d): %s", idx, c.fails, err.c_str());
                }
            }
        }
    }
}

// Stop + free the current clip and restore volume. App thread only.
void StopClip() {
    if (g_clipStream) { BASS_ChannelStop(g_clipStream); BASS_StreamFree(g_clipStream); g_clipStream = 0; }
    g_curCue = -1;
    if (g_ducked) { Duck(1.0); g_ducked = false; }
}

} // namespace

void SubSetDuckCallback(SubDuckFn fn) { g_duckFn = fn; }
void SubSetFallbackCallback(SubFallbackFn fn) { g_fallbackFn = fn; }
void SubSetSpeed(double speed) { std::lock_guard<std::mutex> lk(g_mtx); g_speed = speed; }

void SubStart(const std::vector<SubCue>& cues, const std::string& edgeVoice,
              double lookaheadSec, double duckLevel, const std::string& rate) {
    SubStop();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cues.clear(); g_cues.reserve(cues.size());
    for (const auto& c : cues) { SchedCue s; s.cue = c; g_cues.push_back(std::move(s)); }
    g_queue.clear();
    g_voice = edgeVoice; g_rate = rate; g_lookahead = lookaheadSec; g_duckLevel = duckLevel;
    g_speed = 1.0;
    g_curCue = -1; g_clipStream = 0; g_ducked = false;
    g_running = true;
    g_worker = std::thread(WorkerLoop);
    LogF("subtts", "started: %zu cues, voice=%s, lookahead=%.1fs",
         g_cues.size(), edgeVoice.c_str(), lookaheadSec);
}

void SubStop() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_running && !g_worker.joinable()) return;
        g_running = false;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    StopClip();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cues.clear(); g_queue.clear();
}

void SubOnTimePos(double pos) {
    // 1) reap a finished clip
    if (g_clipStream && BASS_ChannelIsActive(g_clipStream) == BASS_ACTIVE_STOPPED) {
        BASS_StreamFree(g_clipStream); g_clipStream = 0; g_curCue = -1;
        if (g_ducked) { Duck(1.0); g_ducked = false; }
    }

    int toPlay = -1;
    std::vector<unsigned char> mp3;
    std::wstring fallbackText;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_running) return;
        // 2) prefetch cues entering the lookahead window. The window is widened
        // at fast playback (synthesis latency is wall-clock, so video advances
        // faster and we need more video-seconds of lead).
        double lead = g_lookahead * (g_speed > 1.0 ? g_speed : 1.0);
        bool queued = false;
        for (int i = 0; i < (int)g_cues.size(); i++) {
            SchedCue& s = g_cues[i];
            if (s.state == ST_PENDING && s.cue.endSec > pos && s.cue.startSec <= pos + lead) {
                s.state = ST_QUEUED; g_queue.push_back(i); queued = true;
            }
        }
        if (queued) g_cv.notify_all();
        // 3) the cue currently on screen = latest-start, not-yet-handled cue whose
        //    window contains pos. Act on its state.
        int sel = -1;
        for (int i = 0; i < (int)g_cues.size(); i++) {
            SchedCue& s = g_cues[i];
            if (s.state != ST_PLAYED && s.cue.startSec <= pos && pos < s.cue.endSec)
                if (sel < 0 || s.cue.startSec > g_cues[sel].cue.startSec) sel = i;
        }
        if (sel >= 0 && sel != g_curCue) {
            if (g_cues[sel].state == ST_READY) {
                toPlay = sel;
                mp3 = g_cues[sel].mp3;
                g_cues[sel].state = ST_PLAYED;
            } else if (g_cues[sel].state == ST_FAILED) {
                // Synthesis gave up for this line — read it via the screen reader
                // so the user is not left in silence on that cue.
                fallbackText = g_cues[sel].cue.text;
                g_cues[sel].state = ST_PLAYED;
            }
            // PENDING/QUEUED/SYNTH: not ready yet — leave it for a later tick.
        }
    }

    if (!fallbackText.empty() && g_fallbackFn) g_fallbackFn(fallbackText);

    // 4) start the chosen clip (cutting any current one = overlap policy)
    if (toPlay >= 0) {
        if (g_clipStream) { BASS_ChannelStop(g_clipStream); BASS_StreamFree(g_clipStream); g_clipStream = 0; }
        HSTREAM st = BASS_StreamCreateFile(BASS_FILE_MEMCOPY, mp3.data(), 0, mp3.size(), BASS_SAMPLE_FLOAT);
        if (st) {
            if (!g_ducked) { Duck(g_duckLevel); g_ducked = true; }
            BASS_ChannelPlay(st, FALSE);
            g_clipStream = st; g_curCue = toPlay;
        } else {
            LogF("subtts", "BASS clip create failed code=%d", BASS_ErrorGetCode());
        }
    }
}

void SubOnSeek(double pos) {
    StopClip();
    std::lock_guard<std::mutex> lk(g_mtx);
    for (SchedCue& s : g_cues) {
        if (s.cue.endSec <= pos)                                   s.state = ST_PLAYED;  // already past
        else if (!s.mp3.empty())                                   s.state = ST_READY;   // reuse buffer
        else if (s.state != ST_SYNTH && s.state != ST_QUEUED)    { s.state = ST_PENDING; s.fails = 0; }
    }
}

void SubOnPause(bool paused) {
    if (paused) StopClip();
}

// -----------------------------------------------------------------------------
// High-level: source subtitles for a media file, then start scheduling.
// -----------------------------------------------------------------------------

namespace {

std::string ReadWholeFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out; char buf[8192]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0) out.append(buf, rd);
    CloseHandle(h);
    return out;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Look for a sidecar subtitle next to the media (same base name).
std::wstring FindSidecar(const std::wstring& media) {
    size_t dot = media.find_last_of(L'.');
    std::wstring base = (dot == std::wstring::npos) ? media : media.substr(0, dot);
    const wchar_t* exts[] = {L".srt", L".vtt"};
    for (const wchar_t* e : exts) { std::wstring p = base + e; if (FileExists(p)) return p; }
    return L"";
}

// Read a file with a few retries: ffmpeg has just created it and an AV scan can
// briefly lock the fresh file, making the first open fail (observed on Windows).
std::string ReadFileRetry(const std::wstring& path, int tries = 15) {
    for (int i = 0; i < tries; i++) {
        std::string s = ReadWholeFile(path);
        if (!s.empty()) return s;
        Sleep(100);
    }
    return "";
}

// Extract a subtitle stream to outSrt via the bundled ffmpeg. `ffIndex` is the
// container-wide ffmpeg stream index (use the active track), or -1 for 0:s:0.
bool ExtractEmbeddedSrt(const std::wstring& media, const std::wstring& outSrt, int ffIndex) {
    std::wstring ff = GetFfmpegLocation();
    if (ff.empty()) { LogF("subtts", "ffmpeg not found for subtitle extraction"); return false; }
    DeleteFileW(outSrt.c_str());
    std::wstring map = (ffIndex >= 0) ? (L"0:" + std::to_wstring(ffIndex)) : L"0:s:0";
    std::wstring cmd = L"\"" + ff + L"\" -y -loglevel error -i \"" + media +
                       L"\" -map " + map + L" \"" + outSrt + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        LogF("subtts", "CreateProcess(ffmpeg) failed err=%lu", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return FileExists(outSrt);
}

} // namespace

std::vector<SubCue> SubExtractCues(const std::wstring& mediaPath, int subFfIndex) {
    std::vector<SubCue> cues;

    std::wstring sidecar = FindSidecar(mediaPath);
    if (!sidecar.empty()) {
        cues = ParseSubtitles(ReadFileRetry(sidecar));
        LogF("subtts", "sidecar subtitle %ls -> %zu cues", sidecar.c_str(), cues.size());
    }
    if (cues.empty()) {
        wchar_t tmpDir[MAX_PATH]; GetTempPathW(MAX_PATH, tmpDir);
        std::wstring tmp = std::wstring(tmpDir) + L"mediaaccess_subs.srt";
        if (ExtractEmbeddedSrt(mediaPath, tmp, subFfIndex)) {
            std::string data = ReadFileRetry(tmp);
            cues = ParseSubtitles(data);
            LogF("subtts", "embedded extraction (ff-index %d, %zu bytes) -> %zu cues",
                 subFfIndex, data.size(), cues.size());
            DeleteFileW(tmp.c_str());   // don't leave the temp .srt behind
        }
    }
    return cues;
}

bool SubStartForMedia(const std::wstring& mediaPath, const std::string& edgeVoice,
                      double lookaheadSec, double duckLevel, int subFfIndex,
                      const std::string& rate, std::wstring* err) {
    std::vector<SubCue> cues = SubExtractCues(mediaPath, subFfIndex);
    if (cues.empty()) {
        if (err) *err = L"No subtitles found for this video";
        return false;
    }
    SubStart(cues, edgeVoice, lookaheadSec, duckLevel, rate);
    return true;
}

} // namespace mediaaccess
