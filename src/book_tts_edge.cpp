// =============================================================================
// book_tts_edge.cpp — sequential Edge neural-voice narrator for text-only books.
// See book_tts_edge.h for the design. Counterpart of subtitle_scheduler.cpp,
// but sequential (no cue timing / ducking / overlap): one paragraph plays, and
// when its BASS stream ends we ask the player to advance to the next.
// =============================================================================

#include <windows.h>
#include "mediaaccess/book_tts_edge.h"
#include "mediaaccess/edge_tts_client.h"
#include "mediaaccess/logger.h"
#include "resource.h"                    // WM_BOOKEDGE_READY / WM_BOOKEDGE_END (needs windows.h first)
#include "bass.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// The main window — worker posts paragraph-ready / clip-end notifications here.
extern HWND g_hwnd;

namespace mediaaccess {

namespace {

const int kMaxSynthRetries = 2;
// Keep only a small window of synthesized paragraphs around the current one so
// a long reading session can't grow the cache without bound (a book has
// thousands of paragraphs). Current + one-ahead prefetch plus a little slack.
const int kCacheRadius = 3;

// ---- Worker-owned state (guarded by g_mtx) --------------------------------
std::mutex                                   g_mtx;
std::condition_variable                       g_cv;
std::thread                                   g_worker;
bool                                          g_running = false;
std::atomic<bool>                             g_cancel{false};
std::deque<int>                               g_queue;     // segIdx to synthesize (front = priority)
std::map<int, std::wstring>                   g_text;      // segIdx -> text to synthesize
std::map<int, std::vector<unsigned char>>     g_cache;     // segIdx -> synthesized MP3
std::map<int, int>                            g_fails;     // segIdx -> synth failure count
std::set<int>                                 g_failed;    // segIdx that gave up permanently
std::string                                   g_voice;
std::string                                   g_rate = "+0%";
int                                           g_curSeg = -1;
std::atomic<int>                              g_gen{0};
// Audio-config epoch: bumped whenever the voice/rate changes or a book is reset,
// i.e. whenever the AUDIO for a given segment index becomes invalid. The worker
// captures it at synth start and discards a result whose epoch is stale, so a
// synth started under the old voice/rate can't pollute the (segment-keyed,
// generation-independent) cache. Distinct from g_gen, which tracks navigation.
std::atomic<int>                              g_epoch{0};

// ---- Playback state (UI thread only, except the two atomics read by the
// BASS mixtime sync thread in ClipEndProc) ----------------------------------
HSTREAM             g_clip        = 0;
std::atomic<int>    g_clipSeg{-1};          // segment of the playing/created clip
std::atomic<int>    g_clipGen{0};           // generation captured when the clip was created
bool                g_paused      = false;
bool                g_pendingPlay = false;  // synth in flight for the current paragraph
float               g_vol         = 1.0f;   // last applied perceptual volume
BookEdgeFallbackFn  g_fallbackFn  = nullptr;

std::string RatePctToSsml(int pct) {
    if (pct < -50) pct = -50;
    if (pct > 100) pct = 100;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%+d%%", pct);   // "+0%", "-10%", "+25%"
    return buf;
}

// Drop cached/queued paragraphs far from the current one, but ALWAYS preserve
// `keep` (the active one-ahead prefetch, which under content-skipping can be
// more than kCacheRadius away from `around`). Holds g_mtx.
void EvictFar(int around, int keep) {
    for (auto it = g_cache.begin(); it != g_cache.end(); ) {
        if (it->first != keep && std::abs(it->first - around) > kCacheRadius) it = g_cache.erase(it);
        else ++it;
    }
    for (auto it = g_text.begin(); it != g_text.end(); ) {
        if (it->first != keep && std::abs(it->first - around) > kCacheRadius) it = g_text.erase(it);
        else ++it;
    }
}

void WorkerLoop() {
    for (;;) {
        int idx; std::string voice, rate; std::wstring text; int myEpoch = 0;
        bool haveText = false;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_running || !g_queue.empty(); });
            if (!g_running) return;
            idx = g_queue.front(); g_queue.pop_front();
            // Already synthesized (or permanently failed) — just re-notify if it
            // is the paragraph we currently want to play.
            if (g_cache.count(idx) || g_failed.count(idx)) {
                bool isCur = (idx == g_curSeg);
                int gen = g_gen.load();
                lk.unlock();
                if (isCur && g_hwnd) PostMessageW(g_hwnd, WM_BOOKEDGE_READY, (WPARAM)gen, (LPARAM)idx);
                continue;
            }
            auto t = g_text.find(idx);
            if (t == g_text.end()) continue;   // text evicted — skip
            text = t->second; voice = g_voice; rate = g_rate;
            myEpoch = g_epoch.load();          // audio config this synth is for
            haveText = true;
        }
        if (!haveText) continue;

        std::vector<unsigned char> mp3; std::string err;
        bool ok = EdgeSynthesize(voice, text, mp3, rate, "+0Hz", &err, &g_cancel);

        bool isCur = false; int gen = 0; bool done = false;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (!g_running) return;
            // The voice/rate changed (or the book was reset) while this synth was
            // in flight: its audio is for a stale config — discard it so it can't
            // pollute the segment-keyed cache and be played by the generation guard.
            if (myEpoch != g_epoch.load()) continue;
            if (ok) {
                g_cache[idx] = std::move(mp3);
                g_fails.erase(idx);
                done = true;
            } else {
                int n = ++g_fails[idx];
                if (n <= kMaxSynthRetries && !g_cancel.load()) {
                    g_queue.push_back(idx);     // bounded retry
                } else {
                    g_failed.insert(idx);       // give up
                    done = true;
                    LogF("booktts", "seg %d synth failed (try %d): %s", idx, n, err.c_str());
                }
            }
            isCur = (idx == g_curSeg);
            gen = g_gen.load();
        }
        if (done && isCur && g_hwnd)
            PostMessageW(g_hwnd, WM_BOOKEDGE_READY, (WPARAM)gen, (LPARAM)idx);
    }
}

void EnsureWorker() {
    // Caller must NOT hold g_mtx. Starts the worker if it isn't running.
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_running) return;
    g_running = true;
    if (g_worker.joinable()) g_worker.join();   // reap a finished thread
    g_worker = std::thread(WorkerLoop);
}

// Stop + free the current clip. UI thread only. Freeing the stream removes its
// end-sync, so no stale WM_BOOKEDGE_END can fire for a stopped clip.
void StopClipInternal() {
    if (g_clip) { BASS_ChannelStop(g_clip); BASS_StreamFree(g_clip); g_clip = 0; }
    g_clipSeg = -1;
}

// BASS end-of-stream sync (mixtime thread): only fires for the currently
// playing clip (freeing removes the sync), so the module statics still hold
// that clip's gen/seg. Post to the UI thread to advance — never call BASS here.
void CALLBACK ClipEndProc(HSYNC, DWORD, DWORD, void*) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_BOOKEDGE_END,
                             (WPARAM)g_clipGen.load(), (LPARAM)g_clipSeg.load());
}

} // namespace

void BookEdgeSetFallbackCallback(BookEdgeFallbackFn fn) { g_fallbackFn = fn; }

// Synthesized MP3 is specific to the voice AND the rate, so changing either
// invalidates the whole cache (and any queued/pending text). We also bump the
// audio epoch and arm the cancel token so a synth already in flight for the old
// voice/rate is discarded on completion (it captured the old epoch) instead of
// being cached and replayed — the next BookEdgePlay re-synthesizes afresh.
void BookEdgeSetVoice(const std::string& voiceShortName) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_voice == voiceShortName) return;
    g_voice = voiceShortName;
    g_cancel.store(true);
    g_epoch.fetch_add(1);
    g_cache.clear(); g_text.clear(); g_queue.clear();
    g_fails.clear(); g_failed.clear();
}

void BookEdgeSetRate(int ratePct) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string r = RatePctToSsml(ratePct);
    if (g_rate == r) return;
    g_rate = r;
    g_cancel.store(true);
    g_epoch.fetch_add(1);
    g_cache.clear(); g_text.clear(); g_queue.clear();
    g_fails.clear(); g_failed.clear();
}

// Drop everything for a book change. See header. UI thread; the clip was already
// freed by the preceding BookEdgeStop, but we stop defensively and re-arm.
void BookEdgeReset() {
    ++g_gen;
    g_cancel.store(true);
    StopClipInternal();
    g_pendingPlay = false;
    g_paused = false;
    std::lock_guard<std::mutex> lk(g_mtx);
    g_epoch.fetch_add(1);   // invalidate any in-flight synth from the previous book
    g_curSeg = -1;
    g_cache.clear(); g_text.clear(); g_queue.clear();
    g_fails.clear(); g_failed.clear();
}

void BookEdgePlay(const std::wstring& curText, int curSeg,
                  const std::wstring& nextText, int nextSeg) {
    int gen = ++g_gen;
    StopClipInternal();
    bool playNow = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_cancel.store(false);     // re-arm after any prior stop/cancel
        g_paused = false;
        g_curSeg = curSeg;
        g_queue.clear();           // drop a previous paragraph's pending work
        g_text[curSeg] = curText;
        if (g_cache.count(curSeg)) {
            playNow = true;        // prefetched — play immediately
        } else {
            g_failed.erase(curSeg); g_fails.erase(curSeg);   // retry afresh
            g_queue.push_back(curSeg);
        }
        if (nextSeg >= 0 && !g_cache.count(nextSeg)) {
            g_text[nextSeg] = nextText;
            g_queue.push_back(nextSeg);   // prefetch one ahead
        }
        EvictFar(curSeg, nextSeg);   // preserve the active prefetch even if far (content-skipping)
        g_pendingPlay = !playNow;
    }
    if (!playNow) { EnsureWorker(); g_cv.notify_all(); }
    // On UI thread already — play synchronously rather than round-tripping a message.
    if (playNow) BookEdgeOnReadyToPlay(gen, curSeg);
}

void BookEdgeOnReadyToPlay(int gen, int segIdx) {
    if (gen != g_gen.load()) return;          // superseded by a newer request
    bool failed = false;
    std::vector<unsigned char> mp3;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (segIdx != g_curSeg) return;       // no longer the wanted paragraph
        failed = g_failed.count(segIdx) > 0;
        auto it = g_cache.find(segIdx);
        if (it != g_cache.end()) mp3 = it->second;
    }
    g_pendingPlay = false;
    if (failed || mp3.empty()) {
        if (g_fallbackFn) g_fallbackFn(segIdx);   // let the player fall back to SAPI
        return;
    }

    StopClipInternal();
    HSTREAM st = BASS_StreamCreateFile(BASS_FILE_MEMCOPY, mp3.data(), 0, mp3.size(), BASS_SAMPLE_FLOAT);
    if (!st) {
        LogF("booktts", "BASS clip create failed code=%d", BASS_ErrorGetCode());
        if (g_fallbackFn) g_fallbackFn(segIdx);
        return;
    }
    BASS_ChannelSetAttribute(st, BASS_ATTRIB_VOL, g_vol);
    g_clip = st; g_clipSeg = segIdx; g_clipGen = gen;
    BASS_ChannelSetSync(st, BASS_SYNC_END | BASS_SYNC_MIXTIME, 0, ClipEndProc, nullptr);
    if (!g_paused) BASS_ChannelPlay(st, FALSE);
}

void BookEdgeStop() {
    ++g_gen;
    StopClipInternal();
    g_pendingPlay = false;
    g_paused = false;
    g_cancel.store(true);     // abort any in-flight synth (worker stays alive for reuse)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_curSeg = -1;
        g_queue.clear();
    }
    g_cv.notify_all();
}

void BookEdgePause() {
    if (g_clip) BASS_ChannelPause(g_clip);
    g_paused = true;
}

void BookEdgeResume() {
    g_paused = false;
    if (g_clip) BASS_ChannelPlay(g_clip, FALSE);
}

bool BookEdgeIsSpeaking() {
    if (g_clip && BASS_ChannelIsActive(g_clip) == BASS_ACTIVE_PLAYING) return true;
    return g_pendingPlay && !g_paused;   // synthesizing the current paragraph
}

bool BookEdgeIsPaused() { return g_paused; }

void BookEdgeApplyVolume(float perceptualVol) {
    g_vol = perceptualVol;
    if (g_clip) BASS_ChannelSetAttribute(g_clip, BASS_ATTRIB_VOL, perceptualVol);
}

void BookEdgeShutdown() {
    ++g_gen;
    StopClipInternal();
    g_pendingPlay = false;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (!g_running && !g_worker.joinable()) {
            g_cache.clear(); g_text.clear(); g_queue.clear();
            g_fails.clear(); g_failed.clear();
            return;
        }
        g_running = false;
    }
    g_cancel.store(true);
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lk(g_mtx);
    g_cache.clear(); g_text.clear(); g_queue.clear();
    g_fails.clear(); g_failed.clear();
    g_curSeg = -1;
}

} // namespace mediaaccess
