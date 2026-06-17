#pragma once
#ifndef MEDIAACCESS_EDGE_TTS_CLIENT_H
#define MEDIAACCESS_EDGE_TTS_CLIENT_H

// =============================================================================
// edge_tts_client.h — native Microsoft Edge "Read Aloud" online TTS client.
//
// Talks DIRECTLY to the Edge online speech endpoint over a TLS websocket
// (WinHTTP), so MediaAccess can use Edge neural voices WITHOUT requiring the
// user to install any third-party SAPI bridge. The Sec-MS-GEC security token
// is computed locally with CNG/SHA-256. No external dependencies — WinHTTP +
// bcrypt + rpcrt4 are all shipped with Windows.
//
// Protocol reference (validated): tts_latency/EDGE_PROTOCOL.md, ported from
// the public edge-tts project. We do NOT vendor edge-tts/Sonarpad code; this
// is a clean reimplementation of the documented wire protocol.
//
// Used by the subtitle prefetch scheduler: each subtitle cue is synthesized to
// an MP3 buffer ahead of time, then played via BASS at the cue's start time.
//
// Threading: EdgeSynthesize() is SYNCHRONOUS and blocking (one network round
// trip, ~0.4-0.6 s warm). Call it from a worker thread, never the UI thread.
// The clock-skew state it keeps is guarded internally, so concurrent calls
// from multiple workers are safe.
//
// Caveats (see EDGE_PROTOCOL.md): the endpoint is an undocumented Edge service
// (gray-area ToS, non-commercial), and the Sec-MS-GEC algorithm can change on
// Microsoft's side. Callers MUST handle failure gracefully (fall back to the
// screen-reader path) rather than assuming synthesis always succeeds.
// =============================================================================

#include <atomic>
#include <string>
#include <vector>

namespace mediaaccess {

struct EdgeVoice {
    std::string  shortName;    // "fr-FR-DeniseNeural" — the id used in the SSML request
    std::wstring displayName;  // friendly label for the UI (FriendlyName, fallback ShortName)
    std::string  locale;       // "fr-FR"
    std::string  gender;       // "Female" / "Male" ("" if unknown)
};

// Synthesize `text` with the Edge voice `voiceShortName` (e.g. "fr-FR-DeniseNeural")
// to MP3 bytes (format audio-24khz-48kbitrate-mono-mp3). `rate`/`pitch` are SSML
// prosody strings, e.g. "+0%" / "-10%" and "+0Hz".
//
// Returns true and fills `outMp3` on success. On failure returns false, leaves
// `outMp3` empty, and (if `err` non-null) sets a short human-readable reason.
// BLOCKING — run on a worker thread.
//
// `cancel` is an OPTIONAL per-operation cancellation token (v2.44): pass the
// address of an atomic your subsystem owns; when it becomes true the synth
// aborts at the next receive iteration (combined with the finite WinHTTP
// timeouts, within a few seconds). Pass nullptr for no cancellation. Each
// caller MUST own its own token — there is intentionally no shared/global
// cancel state, so one subsystem (e.g. SubStop) can't abort another's synth
// (e.g. a voice preview).
bool EdgeSynthesize(const std::string& voiceShortName,
                    const std::wstring& text,
                    std::vector<unsigned char>& outMp3,
                    const std::string& rate = "+0%",
                    const std::string& pitch = "+0Hz",
                    std::string* err = nullptr,
                    const std::atomic<bool>* cancel = nullptr);

// Catalog of available Edge online voices, fetched once from the service and
// cached for the process lifetime. If the fetch fails (offline / token broken)
// returns a small built-in fallback list so the UI is never empty. BLOCKING.
std::vector<EdgeVoice> EdgeListVoices();

// True once the voice catalog has been fetched (the startup prewarm or a prior
// EdgeListVoices() call populated the cache). Lets the UI decide whether it can
// fill the voice combo without a blocking network call.
bool EdgeVoicesReady();

// Non-blocking voice list for the UI thread: returns the cached catalog if it
// has been fetched, otherwise a small built-in fallback list. NEVER touches the
// network, so it is safe to call from WM_INITDIALOG. Pair with EdgeVoicesReady()
// to know whether a background refresh (then a re-populate) is still needed.
std::vector<EdgeVoice> EdgeListVoicesCached();

} // namespace mediaaccess

#endif // MEDIAACCESS_EDGE_TTS_CLIENT_H
