#pragma once
#ifndef MEDIAACCESS_SUBTITLE_SCHEDULER_H
#define MEDIAACCESS_SUBTITLE_SCHEDULER_H

// =============================================================================
// subtitle_scheduler.h — prefetch + schedule TTS for video subtitles.
//
// Drives the "speak subtitles with an Edge neural voice" feature. Because the
// online voices have ~0.5 s latency (see tts_latency/), we can't synthesize
// reactively when a line appears — we'd lag behind the picture. Instead we
// parse the whole subtitle track up front, pre-synthesize each cue a few
// seconds ahead of its on-screen time (on a worker), and play the ready MP3
// clip via BASS exactly when playback reaches the cue.
//
// libmpv exposes only the currently-visible subtitle line, never the full cue
// list, so the cues come from a subtitle file: an external .srt/.vtt directly,
// or an embedded track extracted to a temp .srt via the bundled ffmpeg.
//
// Synthesis backend: edge_tts_client (native Edge). Audio path: BASS clip
// (mirrors the DAISY book clip pattern), ducking the mpv video volume while a
// line is spoken. Falls back to the live screen-reader path when prefetch
// can't run (image subtitles, no text track, network/token failure).
// =============================================================================

#include <string>
#include <vector>

namespace mediaaccess {

struct SubCue {
    double       startSec = 0.0;  // cue start, seconds
    double       endSec   = 0.0;  // cue end, seconds
    std::wstring text;            // display text, tags stripped, lines joined
};

// Parse SubRip (.srt) or WebVTT (.vtt) content (UTF-8) into time-ordered cues.
// Tolerant of both ',' and '.' millisecond separators and optional hours
// (MM:SS.mmm). Strips markup tags (<i>…</i>, {\an8}, etc.). Cues are returned
// sorted by start time; malformed entries are skipped. Empty on total failure.
std::vector<SubCue> ParseSubtitles(const std::string& utf8Data);

// =============================================================================
// Scheduler — prefetch + timed playback
//
// The scheduler is DRIVEN BY THE APP, not by mpv directly: the caller feeds it
// the current playback position via SubOnTimePos() (from the time-pos observer)
// and notifies seek/pause. This keeps the scheduler unit-testable with a fake
// clock and decouples it from libmpv. It owns a worker thread that pre-renders
// upcoming cues to MP3 via edge_tts_client, and plays each ready clip via BASS
// when playback reaches it. Assumes BASS_Init() has already been called.
// =============================================================================

// Ducking hook: called with 1.0 (restore) when no subtitle is speaking and with
// `duckLevel` (e.g. 0.3) while a clip plays. The app wires this to lower the mpv
// video volume; a test can pass a stub. Optional (null = no ducking).
using SubDuckFn = void (*)(double level);
void SubSetDuckCallback(SubDuckFn fn);

// v2.52 — set the volume of the spoken subtitle clip itself (1.0 = normal).
// Applies to the next clip and the currently-playing one. Lets the user balance
// the voice against the background independently of the ducking setting.
void SubSetVoiceVolume(float vol);

// Per-cue fallback: called (on the app thread, from SubOnTimePos) with a cue's
// text when its synthesis has permanently failed, so the app can read that one
// line via the screen reader instead of leaving it silent. Optional.
using SubFallbackFn = void (*)(const std::wstring& text);
void SubSetFallbackCallback(SubFallbackFn fn);

// v2.52 — per-cue "speak now" hook used in LIVE-ONLY mode (no Edge prefetch):
// fired on the app thread as each cue becomes current, so a plain screen-reader
// user gets the caption text in audio mode. Optional (null = not live-only).
using SubCueSpeakFn = void (*)(const std::wstring& text);
void SubSetCueSpeakCallback(SubCueSpeakFn fn);

// v2.52 — load + parse a subtitle file into cues. When isAutoCaption is true,
// runs CollapseRollingCues to remove YouTube auto-caption rolling duplication.
// BLOCKING (file read) — call on a worker thread.
std::vector<SubCue> SubLoadCuesFromFile(const std::wstring& path, bool isAutoCaption);

// v2.52 — collapse YouTube auto-caption rolling/duplicate cues in place.
void CollapseRollingCues(std::vector<SubCue>& cues);

// Tell the scheduler the current video playback speed so the lookahead window
// (in video-seconds) is widened at fast speeds — synthesis latency is wall-clock,
// so faster playback needs more video lookahead to stay ahead. 1.0 = normal.
void SubSetSpeed(double speed);

// Begin prefetching/scheduling `cues` with the given Edge voice short name.
// `lookaheadSec` is how far ahead of playback to pre-synthesize. Replaces any
// previous session. Starts the worker thread.
// v2.52 — liveOnly: skip the Edge prefetch worker entirely and instead fire the
// cue-speak callback as each cue becomes current (live screen-reader mode).
void SubStart(const std::vector<SubCue>& cues, const std::string& edgeVoice,
              double lookaheadSec = 2.5, double duckLevel = 0.3,
              const std::string& rate = "+0%", bool liveOnly = false);

// Source subtitle cues for `mediaPath` — first a sidecar .srt/.vtt next to the
// file, else the embedded subtitle track `subFfIndex` extracted via the bundled
// ffmpeg (or the first text track if -1). Returns the parsed cues (empty on
// failure). BLOCKING (runs ffmpeg + file reads) — call on a worker thread, never
// the UI thread; the result is then handed to SubStart.
std::vector<SubCue> SubExtractCues(const std::wstring& mediaPath, int subFfIndex = -1);

// High-level start: source subtitles for `mediaPath` — first a sidecar
// .srt/.vtt next to the file, else the embedded subtitle track extracted via
// the bundled ffmpeg — parse them, and begin prefetch scheduling with `voice`.
// Returns false (and sets *err) when no usable subtitles are found. BLOCKING
// (runs ffmpeg synchronously); call off the time-critical path.
// `subFfIndex` is the ffmpeg stream index of the subtitle track to extract
// (from MPVGetActiveSubtitleFfIndex); pass -1 to fall back to the first text
// subtitle stream.
bool SubStartForMedia(const std::wstring& mediaPath, const std::string& edgeVoice,
                      double lookaheadSec = 2.5, double duckLevel = 0.3,
                      int subFfIndex = -1, const std::string& rate = "+0%",
                      std::wstring* err = nullptr);

// Stop scheduling, halt any clip, free everything, join the worker.
void SubStop();

// Terminate any in-flight ffmpeg subtitle-extraction child processes and block
// further extractions. Call once on app shutdown (WM_DESTROY) so a detached
// extraction worker can't leave an orphaned ffmpeg running past exit. v2.44.
void SubShutdownExtraction();

// Feed the current playback position (seconds). Triggers prefetch of upcoming
// cues and plays a ready clip when its cue becomes current. Call frequently
// (e.g. on every time-pos change). No-op if not started.
void SubOnTimePos(double posSec);

// Notify a seek to `posSec`: stops the current clip and re-arms cues around the
// new position (kept buffers are reused; passed cues are marked done).
void SubOnSeek(double posSec);

// Notify pause state change. On pause the current clip is stopped.
void SubOnPause(bool paused);

} // namespace mediaaccess

#endif // MEDIAACCESS_SUBTITLE_SCHEDULER_H
