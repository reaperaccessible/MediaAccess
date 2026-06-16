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

} // namespace mediaaccess

#endif // MEDIAACCESS_SUBTITLE_SCHEDULER_H
