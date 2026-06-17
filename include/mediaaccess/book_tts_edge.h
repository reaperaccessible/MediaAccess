#pragma once
#ifndef MEDIAACCESS_BOOK_TTS_EDGE_H
#define MEDIAACCESS_BOOK_TTS_EDGE_H

// =============================================================================
// book_tts_edge.h — sequential Edge neural-voice narrator for text-only books.
//
// This is the book-reading counterpart of the subtitle prefetch scheduler. The
// subtitle scheduler is driven by the video clock (cues fire at timestamps); a
// book is purely SEQUENTIAL — we read one paragraph and, when it finishes,
// advance to the next. So this module is much simpler than subtitle_scheduler:
// no cue timing, no ducking, no overlap policy.
//
// It mirrors the small slice of the SAPI tts_player API that daisy_player.cpp
// drives (speak / stop / pause / resume / is-speaking), so the player can pick
// SAPI or Edge per book without changing its control flow. Reuses EdgeSynthesize
// (edge_tts_client.h) for text -> MP3 and BASS for playback.
//
// Threading: a background worker synthesizes paragraphs (current + a one-ahead
// prefetch so playback is gapless despite ~0.5 s network latency). When the
// CURRENT paragraph's MP3 is ready, the worker PostMessages WM_BOOKEDGE_READY to
// the main window; the UI thread then creates the BASS stream and plays it. A
// BASS end-of-stream sync posts WM_BOOKEDGE_END, which the player turns into
// "advance to the next paragraph" — the BASS analogue of SAPI's end-of-stream.
//
// All public functions below are UI-thread only (they touch the BASS stream),
// EXCEPT that the worker owns its own cache/cancel internally. The cache is
// keyed by segment index and a book's text is immutable, so synthesized MP3 is
// reused across navigation (re-reading a paragraph is instant).
//
// Like the subtitle path, callers MUST handle synthesis failure gracefully: on
// a hard failure the registered fallback fires so the player can fall back to
// the SAPI voice rather than leaving the reader in silence.
// =============================================================================

#include <string>

namespace mediaaccess {

// Fallback notification: invoked (on the UI thread) with the segment index when
// Edge synthesis of the current paragraph has failed for good. The player uses
// this to switch the book to the SAPI voice and keep reading.
using BookEdgeFallbackFn = void (*)(int segIdx);
void BookEdgeSetFallbackCallback(BookEdgeFallbackFn fn);

// Voice / rate used for synthesis. Set before BookEdgePlay; changing them takes
// effect on the next BookEdgePlay (or after a re-speak). `ratePct` is a percent
// offset (-50..+100; 0 = normal) mapped to the Edge SSML prosody "+N%"/"-N%".
void BookEdgeSetVoice(const std::string& voiceShortName);
void BookEdgeSetRate(int ratePct);

// Begin (or restart) narration at paragraph `curSeg` with text `curText`.
// `nextSeg`/`nextText` describe the immediately-following paragraph to prefetch
// (pass nextSeg < 0 if there is none — e.g. last paragraph). Bumps the internal
// generation so any in-flight synth/playback for a previous paragraph is
// superseded. Returns immediately; playback starts asynchronously once the MP3
// is ready (or right away if it was prefetched). Resets paused state to playing.
void BookEdgePlay(const std::wstring& curText, int curSeg,
                  const std::wstring& nextText, int nextSeg);

// Stop narration: bump the generation (cancels pending synth), stop+free the
// BASS stream. The MP3 cache is kept (cheap re-read), use BookEdgeShutdown to
// drop everything at app exit.
void BookEdgeStop();

// Pause / resume the current paragraph clip.
void BookEdgePause();
void BookEdgeResume();

// True while a paragraph is playing OR its synth is in flight (so the player
// reports "playing" during the brief synth gap). IsPaused reflects BookEdgePause.
bool BookEdgeIsSpeaking();
bool BookEdgeIsPaused();

// Apply the app's perceptual volume (g_muted ? 0 : g_volume*g_volume) to the
// current clip. Called from DaisyApplyVolume so book narration tracks the
// global volume / mute like regular BASS playback.
void BookEdgeApplyVolume(float perceptualVol);

// UI-thread entry points invoked from the WndProc message handlers (resource.h).
// WM_BOOKEDGE_READY -> BookEdgeOnReadyToPlay(gen, segIdx): create+play the clip.
// WM_BOOKEDGE_END   -> handled in daisy_player (DaisyOnBookEdgeEnd) for advance.
void BookEdgeOnReadyToPlay(int gen, int segIdx);

// Stop everything and join the worker; free the cache. Call once at app exit
// (WM_DESTROY) so a frozen network connection can't orphan the worker thread.
void BookEdgeShutdown();

} // namespace mediaaccess

#endif // MEDIAACCESS_BOOK_TTS_EDGE_H
