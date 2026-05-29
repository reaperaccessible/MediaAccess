// =============================================================================
// daisy_player.cpp — DAISY/EPUB book playback engine on top of BASS
//
// Each book is a linear vector<DaisyClip> from daisy_book. We play one clip
// at a time as a separate BASS stream. When BASS reaches the clip's end
// position (set via BASS_SYNC_POS) it posts a message to the main thread,
// which advances to the next clip.
//
// Structural navigation (Shift+arrows) walks the nav-point table to find
// the previous/next heading at the user's chosen "navigation level".
// =============================================================================

#include "mediaaccess/daisy_player.h"
#include "mediaaccess/database.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/tts_player.h"          // Phase 2 — TTS for text-only books
#include "mediaaccess/book_text_window.h"    // Phase 2 — text display window
#include "bass.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

extern HWND  g_hwnd;
extern float g_volume;     // MediaAccess global volume (0.0..maxVol)
extern bool  g_muted;      // Muted state

// Custom message sent from BASS sync (worker thread) to main thread when a
// clip reaches its end position. Wparam = clip index that just ended.
#define WM_DAISY_NEXT_CLIP (WM_USER + 50)

// Optional: keep this in sync with main.cpp's WM_COMMAND dispatch so the
// rest of the file doesn't depend on a header. main.cpp routes the message
// to DaisyOnClipEnded().
extern "C" void DaisyOnClipEnded(int endedClipIndex);

namespace mediaaccess {

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

struct DaisyState {
    std::unique_ptr<DaisyBook> book;
    int      bookId          = 0;
    int      currentClip     = 0;     // Index into book->clips
    double   currentOffset   = 0.0;   // Seconds into current clip (cached on pause)
    HSTREAM  stream          = 0;
    HSYNC    endSync         = 0;
    int      navLevel        = 1;     // 1..6 = heading, 0 = page, -1 = phrase

    // Cumulative duration of clips 0..currentClip-1, so DaisyGetBookPosition
    // can compute "global" book time without re-summing every call.
    double   clipsBeforeDuration = 0.0;

    // Phase 2 — TTS path for text-only books / EPUB without Media Overlays.
    // When textOnlyMode is true, currentClip / stream / endSync are unused;
    // currentSegment indexes into book->textSegments and SAPI handles
    // playback via the tts_player module. WM_TTS_END_OF_STREAM advances
    // to the next segment.
    bool     textOnlyMode    = false;
    int      currentSegment  = 0;
    bool     ttsPaused       = false;
};

static DaisyState g_d;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static double ClipDuration(const DaisyClip& c) {
    if (c.clipEnd > c.clipBegin) return c.clipEnd - c.clipBegin;
    return 0.0;  // unknown
}

static void RecomputeClipsBeforeDuration() {
    g_d.clipsBeforeDuration = 0.0;
    if (!g_d.book) return;
    for (int i = 0; i < g_d.currentClip && i < (int)g_d.book->clips.size(); ++i) {
        g_d.clipsBeforeDuration += ClipDuration(g_d.book->clips[i]);
    }
}

static void FreeStream() {
    if (g_d.stream) {
        if (g_d.endSync) {
            BASS_ChannelRemoveSync(g_d.stream, g_d.endSync);
            g_d.endSync = 0;
        }
        BASS_StreamFree(g_d.stream);
        g_d.stream = 0;
    }
}

// BASS sync callback — runs on a BASS worker thread. Post message to main
// thread to actually advance the clip (we can't call back into BASS safely
// from inside a sync callback when freeing the stream that fired it).
static void CALLBACK ClipEndSyncProc(HSYNC, DWORD, DWORD, void* user) {
    int clipIdx = (int)(intptr_t)user;
    if (g_hwnd) PostMessageW(g_hwnd, WM_DAISY_NEXT_CLIP, (WPARAM)clipIdx, 0);
}

// Open clip's audio file as a BASS stream, position to clipBegin, set a sync
// at clipEnd to advance. Does NOT call BASS_ChannelPlay — caller decides.
static bool StartClipPaused(int clipIdx) {
    FreeStream();
    if (!g_d.book) return false;
    if (clipIdx < 0 || clipIdx >= (int)g_d.book->clips.size()) return false;

    const DaisyClip& c = g_d.book->clips[clipIdx];
    if (c.audioFile.empty()) return false;

    g_d.stream = BASS_StreamCreateFile(FALSE, c.audioFile.c_str(), 0, 0,
                                       BASS_UNICODE | BASS_SAMPLE_FLOAT);
    if (!g_d.stream) {
        LogF("DAISY", "BASS_StreamCreateFile failed code=%d", BASS_ErrorGetCode());
        return false;
    }

    // Seek to clipBegin
    QWORD bytePos = BASS_ChannelSeconds2Bytes(g_d.stream, c.clipBegin);
    BASS_ChannelSetPosition(g_d.stream, bytePos, BASS_POS_BYTE);

    // Set end-of-clip sync (only if clipEnd is specified)
    if (c.clipEnd > c.clipBegin) {
        QWORD endByte = BASS_ChannelSeconds2Bytes(g_d.stream, c.clipEnd);
        g_d.endSync = BASS_ChannelSetSync(g_d.stream, BASS_SYNC_POS | BASS_SYNC_MIXTIME,
                                          endByte, ClipEndSyncProc,
                                          (void*)(intptr_t)clipIdx);
    } else {
        // No clipEnd → use end-of-stream
        g_d.endSync = BASS_ChannelSetSync(g_d.stream, BASS_SYNC_END | BASS_SYNC_MIXTIME,
                                          0, ClipEndSyncProc,
                                          (void*)(intptr_t)clipIdx);
    }

    g_d.currentClip   = clipIdx;
    g_d.currentOffset = 0.0;
    RecomputeClipsBeforeDuration();
    DaisyApplyVolume();   // New stream inherits current global volume / mute

    // Push the clip's text to the text window for audio+text books.
    // (Empty string clears the display for audio-only clips.)
    BookTextWindowSetText(c.textContent);
    return true;
}

// -----------------------------------------------------------------------------
// TTS path — used for text-only DAISY books and EPUB without Media Overlays
// -----------------------------------------------------------------------------

static bool StartTtsSegment(int segIdx) {
    if (!g_d.book) return false;
    if (segIdx < 0 || segIdx >= (int)g_d.book->textSegments.size()) return false;
    const DaisyTextSegment& seg = g_d.book->textSegments[segIdx];
    g_d.currentSegment = segIdx;
    BookTextWindowSetText(seg.text);
    if (!TtsSpeak(seg.text)) {
        LogF("daisy", "TTS Speak failed for segment %d", segIdx);
        return false;
    }
    g_d.ttsPaused = false;
    return true;
}

static void SaveTtsPosition() {
    if (g_d.bookId <= 0) return;
    // Reuse position_clip column to store segment index; offset always 0
    // for TTS since SAPI doesn't expose intra-utterance position.
    UpdateBookPosition(g_d.bookId, g_d.currentSegment, 0.0);
}

static void SaveCurrentPosition() {
    if (g_d.bookId <= 0) return;
    double offsetInClip = 0.0;
    if (g_d.stream && g_d.book && g_d.currentClip < (int)g_d.book->clips.size()) {
        QWORD pos = BASS_ChannelGetPosition(g_d.stream, BASS_POS_BYTE);
        double sec = BASS_ChannelBytes2Seconds(g_d.stream, pos);
        offsetInClip = sec - g_d.book->clips[g_d.currentClip].clipBegin;
        if (offsetInClip < 0) offsetInClip = 0;
    }
    UpdateBookPosition(g_d.bookId, g_d.currentClip, offsetInClip);
}

// Apply MediaAccess's global g_volume + g_muted to the current book stream.
// Uses the same perceptual curve (volume²) as legacy-volume mode in player.cpp
// so volume changes feel identical between regular audio and book playback.
void DaisyApplyVolume() {
    if (!g_d.stream) return;
    float v = g_muted ? 0.0f : (::g_volume * ::g_volume);
    BASS_ChannelSetAttribute(g_d.stream, BASS_ATTRIB_VOL, v);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool DaisyIsActive() { return g_d.book != nullptr; }

void DaisyClose() {
    if (g_d.textOnlyMode) {
        SaveTtsPosition();
        TtsStop();
    } else {
        SaveCurrentPosition();
        FreeStream();
    }
    g_d.book.reset();
    g_d.bookId = 0;
    g_d.currentClip = 0;
    g_d.currentOffset = 0.0;
    g_d.clipsBeforeDuration = 0.0;
    g_d.textOnlyMode = false;
    g_d.currentSegment = 0;
    g_d.ttsPaused = false;
}

bool DaisyLoadAndPlay(std::unique_ptr<DaisyBook> book, int bookId) {
    if (!book) return false;

    // Text-only path (text-only DAISY or EPUB without Media Overlays):
    // use SAPI to speak paragraphs in sequence.
    bool isTextOnly = book->clips.empty() && !book->textSegments.empty();

    if (!isTextOnly && book->clips.empty()) {
        // No audio AND no extracted text — nothing we can play.
        return false;
    }

    // Tear down anything currently playing.
    DaisyClose();

    g_d.book          = std::move(book);
    g_d.bookId        = bookId;
    g_d.textOnlyMode  = isTextOnly;

    if (isTextOnly) {
        // Make sure SAPI is initialized (init is idempotent).
        TtsInit();

        // Resume from saved segment.
        BookEntry e = GetBookById(bookId);
        int startSeg = 0;
        if (e.id != 0 && e.positionClip >= 0 &&
            e.positionClip < (int)g_d.book->textSegments.size()) {
            startSeg = e.positionClip;
        }
        if (!StartTtsSegment(startSeg)) return false;
        MarkBookOpened(bookId);
        return true;
    }

    // Audio + (optional) text path — original logic.
    BookEntry e = GetBookById(bookId);
    int startClip = 0;
    double startOffset = 0.0;
    if (e.id != 0 && e.positionClip >= 0 && e.positionClip < (int)g_d.book->clips.size()) {
        startClip   = e.positionClip;
        startOffset = e.positionOffset;
    }

    if (!StartClipPaused(startClip)) return false;
    // Apply intra-clip offset
    if (startOffset > 0) {
        const DaisyClip& c = g_d.book->clips[startClip];
        double targetSec = c.clipBegin + startOffset;
        if (c.clipEnd > 0 && targetSec > c.clipEnd) targetSec = c.clipBegin;
        QWORD b = BASS_ChannelSeconds2Bytes(g_d.stream, targetSec);
        BASS_ChannelSetPosition(g_d.stream, b, BASS_POS_BYTE);
    }

    BASS_ChannelPlay(g_d.stream, FALSE);
    MarkBookOpened(bookId);
    return true;
}

void DaisyPlay() {
    if (g_d.textOnlyMode) {
        if (g_d.ttsPaused) {
            TtsResume();
            g_d.ttsPaused = false;
        } else if (!TtsIsSpeaking()) {
            StartTtsSegment(g_d.currentSegment);
        }
        return;
    }
    if (g_d.stream) BASS_ChannelPlay(g_d.stream, FALSE);
}

void DaisyPause() {
    if (g_d.textOnlyMode) {
        TtsPause();
        g_d.ttsPaused = true;
        SaveTtsPosition();
        return;
    }
    if (g_d.stream) {
        BASS_ChannelPause(g_d.stream);
        SaveCurrentPosition();
    }
}

void DaisyPlayPause() {
    if (g_d.textOnlyMode) {
        if (TtsIsSpeaking()) DaisyPause();
        else                 DaisyPlay();
        return;
    }
    if (!g_d.stream) return;
    DWORD st = BASS_ChannelIsActive(g_d.stream);
    if (st == BASS_ACTIVE_PLAYING) DaisyPause();
    else                           DaisyPlay();
}

void DaisyStop() {
    if (g_d.textOnlyMode) {
        TtsStop();
        g_d.ttsPaused = false;
        SaveTtsPosition();
        return;
    }
    if (!g_d.stream || !g_d.book) return;
    BASS_ChannelPause(g_d.stream);
    const DaisyClip& c = g_d.book->clips[g_d.currentClip];
    QWORD b = BASS_ChannelSeconds2Bytes(g_d.stream, c.clipBegin);
    BASS_ChannelSetPosition(g_d.stream, b, BASS_POS_BYTE);
    SaveCurrentPosition();
}

void DaisySeekRelative(double delta) {
    if (!g_d.stream || !g_d.book) return;
    double bookPos = DaisyGetBookPosition();
    double target  = bookPos + delta;
    if (target < 0) target = 0;
    double total = DaisyGetBookDuration();
    if (total > 0 && target > total) target = total;

    // Find the clip that contains target time.
    double accum = 0.0;
    int targetClip = 0;
    double offsetInClip = 0.0;
    for (size_t i = 0; i < g_d.book->clips.size(); ++i) {
        double dur = ClipDuration(g_d.book->clips[i]);
        if (target <= accum + dur || i == g_d.book->clips.size() - 1) {
            targetClip   = (int)i;
            offsetInClip = target - accum;
            if (offsetInClip < 0) offsetInClip = 0;
            break;
        }
        accum += dur;
    }

    bool wasPlaying = BASS_ChannelIsActive(g_d.stream) == BASS_ACTIVE_PLAYING;
    if (!StartClipPaused(targetClip)) return;
    if (offsetInClip > 0) {
        const DaisyClip& c = g_d.book->clips[targetClip];
        QWORD b = BASS_ChannelSeconds2Bytes(g_d.stream, c.clipBegin + offsetInClip);
        BASS_ChannelSetPosition(g_d.stream, b, BASS_POS_BYTE);
    }
    if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
}

double DaisyGetBookPosition() {
    if (!g_d.stream || !g_d.book) return 0.0;
    QWORD pos = BASS_ChannelGetPosition(g_d.stream, BASS_POS_BYTE);
    double sec = BASS_ChannelBytes2Seconds(g_d.stream, pos);
    double offsetInClip = sec - g_d.book->clips[g_d.currentClip].clipBegin;
    if (offsetInClip < 0) offsetInClip = 0;
    return g_d.clipsBeforeDuration + offsetInClip;
}

double DaisyGetBookDuration() {
    return g_d.book ? g_d.book->totalDuration : 0.0;
}

bool DaisyIsPlaying() {
    if (g_d.textOnlyMode) return TtsIsSpeaking();
    return g_d.stream && BASS_ChannelIsActive(g_d.stream) == BASS_ACTIVE_PLAYING;
}

bool DaisyIsPaused() {
    if (g_d.textOnlyMode) return g_d.ttsPaused;
    return g_d.stream && BASS_ChannelIsActive(g_d.stream) == BASS_ACTIVE_PAUSED;
}

// -----------------------------------------------------------------------------
// Navigation
// -----------------------------------------------------------------------------

static std::string NavLevelName(int level) {
    switch (level) {
        case 1: return Ts("Heading level 1");
        case 2: return Ts("Heading level 2");
        case 3: return Ts("Heading level 3");
        case 4: return Ts("Heading level 4");
        case 5: return Ts("Heading level 5");
        case 6: return Ts("Heading level 6");
        case 0: return Ts("Page");
        case -1: return Ts("Phrase");
    }
    return "";
}

static bool LevelMatches(int navPointLevel, int kindIsPage, int wantLevel) {
    if (wantLevel == 0) return kindIsPage != 0;     // Page mode
    if (wantLevel == -1) return false;              // Phrase = clip granularity, not navPoints
    if (kindIsPage) return false;                   // We only want headings
    return navPointLevel == wantLevel;
}

static int FindNavPointForClip(int clipIndex, int wantLevel) {
    // Returns the index of the latest nav point at wantLevel with
    // clipIndex <= given. -1 if none.
    if (!g_d.book) return -1;
    int best = -1;
    for (size_t i = 0; i < g_d.book->navPoints.size(); ++i) {
        const auto& np = g_d.book->navPoints[i];
        if (np.clipIndex > clipIndex) break;
        if (LevelMatches(np.level, np.kind == DaisyNavPoint::Page ? 1 : 0, wantLevel))
            best = (int)i;
    }
    return best;
}

int  DaisyGetNavLevel() { return g_d.navLevel; }

void DaisySetNavLevel(int level) {
    // Clamp to a sensible range; the announce hint will tell the user what
    // they ended up on, so we don't over-validate.
    if (level < -1) level = -1;
    if (level > 6) level = 6;
    g_d.navLevel = level;

    // Announce "Navigation level: <name>, N items" so the user knows what
    // each heading level represents in this specific book (e.g. 24 chapters
    // at level 2, 200 pages at the page level). Matches Victor Reader's
    // announcement style. The clip count is used for phrase level since
    // there are no nav points there.
    int count = 0;
    if (g_d.book) {
        if (level == -1) {
            count = (int)g_d.book->clips.size();
        } else {
            for (const auto& np : g_d.book->navPoints) {
                if (level == 0) { if (np.kind == DaisyNavPoint::Page) ++count; }
                else            { if (np.kind != DaisyNavPoint::Page && np.level == level) ++count; }
            }
        }
    }
    char numBuf[16];
    std::snprintf(numBuf, sizeof(numBuf), "%d", count);
    std::string msg = Ts("Navigation level") + ": " + NavLevelName(level)
                    + ", " + numBuf + " " +
                      Ts(count > 1 ? "items" : "item");
    Speak(msg);
}

void DaisyCycleNavLevel(int direction) {
    // Cycle through levels that actually exist in the current book.
    if (!g_d.book) return;
    std::vector<int> available;
    bool hasPage = false;
    int maxHeading = 0;
    for (const auto& np : g_d.book->navPoints) {
        if (np.kind == DaisyNavPoint::Page) hasPage = true;
        else if (np.level >= 1 && np.level <= 6 && np.level > maxHeading) maxHeading = np.level;
    }
    for (int h = 1; h <= maxHeading; ++h) available.push_back(h);
    if (hasPage) available.push_back(0);
    available.push_back(-1);  // Phrase always available

    if (available.empty()) return;
    int curIdx = -1;
    for (size_t i = 0; i < available.size(); ++i)
        if (available[i] == g_d.navLevel) { curIdx = (int)i; break; }
    if (curIdx < 0) curIdx = 0;
    int newIdx = (curIdx + direction + (int)available.size()) % (int)available.size();
    DaisySetNavLevel(available[newIdx]);
}

void DaisyNavigateForward() {
    if (!g_d.book) return;
    // Text-only path — Shift+Right = next paragraph.
    if (g_d.textOnlyMode) {
        int next = g_d.currentSegment + 1;
        if (next >= (int)g_d.book->textSegments.size()) {
            Speak(Ts("End of book"));
            return;
        }
        StartTtsSegment(next);
        return;
    }
    if (g_d.navLevel == -1) {
        // Phrase mode = next clip
        int next = g_d.currentClip + 1;
        if (next >= (int)g_d.book->clips.size()) {
            Speak(Ts("End of book"));
            return;
        }
        bool wasPlaying = DaisyIsPlaying();
        StartClipPaused(next);
        if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
        DaisyAnnounceCurrentLocation();
        return;
    }
    // Find next navPoint at wantLevel with clipIndex > current.
    for (size_t i = 0; i < g_d.book->navPoints.size(); ++i) {
        const auto& np = g_d.book->navPoints[i];
        if (np.clipIndex <= g_d.currentClip) continue;
        if (!LevelMatches(np.level, np.kind == DaisyNavPoint::Page ? 1 : 0, g_d.navLevel))
            continue;
        bool wasPlaying = DaisyIsPlaying();
        StartClipPaused(np.clipIndex);
        if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
        SpeakW((LPCWSTR)np.label.c_str());
        return;
    }
    Speak(Ts("End of book"));
}

void DaisyNavigateBackward() {
    if (!g_d.book) return;
    if (g_d.textOnlyMode) {
        if (g_d.currentSegment <= 0) { Speak(Ts("Beginning of book")); return; }
        StartTtsSegment(g_d.currentSegment - 1);
        return;
    }
    if (g_d.navLevel == -1) {
        if (g_d.currentClip <= 0) { Speak(Ts("Beginning of book")); return; }
        bool wasPlaying = DaisyIsPlaying();
        StartClipPaused(g_d.currentClip - 1);
        if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
        DaisyAnnounceCurrentLocation();
        return;
    }
    // Walk backward.
    int found = -1;
    for (int i = (int)g_d.book->navPoints.size() - 1; i >= 0; --i) {
        const auto& np = g_d.book->navPoints[i];
        if (np.clipIndex >= g_d.currentClip) continue;
        if (!LevelMatches(np.level, np.kind == DaisyNavPoint::Page ? 1 : 0, g_d.navLevel))
            continue;
        found = i; break;
    }
    if (found < 0) { Speak(Ts("Beginning of book")); return; }
    bool wasPlaying = DaisyIsPlaying();
    StartClipPaused(g_d.book->navPoints[found].clipIndex);
    if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
    SpeakW((LPCWSTR)g_d.book->navPoints[found].label.c_str());
}

void DaisyAnnounceCurrentLocation() {
    if (!g_d.book) return;
    // Walk nav points and find the last heading and last page before/at current clip.
    std::wstring heading, page;
    for (const auto& np : g_d.book->navPoints) {
        if (np.clipIndex > g_d.currentClip) break;
        if (np.kind == DaisyNavPoint::Page)        page    = np.label;
        else if (np.level >= 1 && np.level <= 6)   heading = np.label;
    }
    std::wstring msg;
    if (!heading.empty()) msg = heading;
    if (!page.empty()) {
        if (!msg.empty()) msg += L", ";
        msg += page;
    }
    if (msg.empty()) msg = g_d.book->title;
    SpeakW(msg);
}

// -----------------------------------------------------------------------------
// Bookmarks
// -----------------------------------------------------------------------------

int DaisyAddBookmarkHere(const std::wstring& note) {
    if (!g_d.book || g_d.bookId <= 0) return 0;
    double offsetInClip = 0.0;
    if (g_d.stream && g_d.currentClip < (int)g_d.book->clips.size()) {
        QWORD pos = BASS_ChannelGetPosition(g_d.stream, BASS_POS_BYTE);
        double sec = BASS_ChannelBytes2Seconds(g_d.stream, pos);
        offsetInClip = sec - g_d.book->clips[g_d.currentClip].clipBegin;
        if (offsetInClip < 0) offsetInClip = 0;
    }
    int id = AddBookBookmark(g_d.bookId, g_d.currentClip, offsetInClip, note);
    if (id > 0) Speak(Ts("Bookmark added"));
    return id;
}

void DaisyJumpToSegment(int segmentIndex) {
    if (!g_d.book || !g_d.textOnlyMode) return;
    if (segmentIndex < 0 || segmentIndex >= (int)g_d.book->textSegments.size()) return;
    StartTtsSegment(segmentIndex);
}

bool DaisyHasText() {
    if (!g_d.book) return false;
    if (!g_d.book->textSegments.empty()) return true;
    for (const auto& c : g_d.book->clips) {
        if (!c.textContent.empty()) return true;
    }
    return false;
}

const DaisyBook* DaisyCurrentBook() {
    return g_d.book.get();
}

void DaisyJumpToBookmark(int clipIndex, double offsetSeconds) {
    if (!g_d.book) return;
    if (clipIndex < 0 || clipIndex >= (int)g_d.book->clips.size()) return;
    bool wasPlaying = DaisyIsPlaying();
    if (!StartClipPaused(clipIndex)) return;
    if (offsetSeconds > 0) {
        const DaisyClip& c = g_d.book->clips[clipIndex];
        double tgt = c.clipBegin + offsetSeconds;
        if (c.clipEnd > 0 && tgt > c.clipEnd) tgt = c.clipBegin;
        QWORD b = BASS_ChannelSeconds2Bytes(g_d.stream, tgt);
        BASS_ChannelSetPosition(g_d.stream, b, BASS_POS_BYTE);
    }
    if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
    DaisyAnnounceCurrentLocation();
}

} // namespace mediaaccess

// -----------------------------------------------------------------------------
// Page-by-label lookup, exposed for books_dialog.cpp's "Go to page" prompt.
// Walks the book's nav points of kind=Page and matches the user's input
// against the label (exact match first, then substring). Returns true if
// a matching page was found and we jumped there.
// -----------------------------------------------------------------------------
namespace mediaaccess {
bool DaisyJumpToPageLabel(const std::wstring& q) {
    if (!g_d.book) return false;
    if (q.empty()) return false;

    // Exact-label match first.
    for (const auto& np : g_d.book->navPoints) {
        if (np.kind != DaisyNavPoint::Page) continue;
        if (np.label == q) {
            bool wasPlaying = DaisyIsPlaying();
            StartClipPaused(np.clipIndex);
            if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
            SpeakW(np.label);
            return true;
        }
    }
    // Substring match (case-sensitive).
    for (const auto& np : g_d.book->navPoints) {
        if (np.kind != DaisyNavPoint::Page) continue;
        if (np.label.find(q) != std::wstring::npos) {
            bool wasPlaying = DaisyIsPlaying();
            StartClipPaused(np.clipIndex);
            if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
            SpeakW(np.label);
            return true;
        }
    }
    return false;
}
} // namespace mediaaccess (DaisyJumpToPageLabel)

// -----------------------------------------------------------------------------
// C-linkage hook for the WM_DAISY_NEXT_CLIP handler in main.cpp.
// -----------------------------------------------------------------------------
extern "C" void DaisyOnClipEnded(int endedClipIndex) {
    using namespace mediaaccess;
    if (!g_d.book) return;
    // Defensive: only advance if the sync that fired matches the clip we
    // currently think is playing (avoids races on user-initiated skips).
    if (endedClipIndex != g_d.currentClip) return;
    int next = g_d.currentClip + 1;
    if (next >= (int)g_d.book->clips.size()) {
        // End of book — pause and announce.
        FreeStream();
        SaveCurrentPosition();
        Speak(Ts("End of book"));
        return;
    }
    if (StartClipPaused(next)) {
        BASS_ChannelPlay(g_d.stream, FALSE);
    }
}

// Called from main.cpp WndProc when SAPI signals end of utterance via
// WM_TTS_END_OF_STREAM. Advance to the next paragraph in text-only mode.
extern "C" void DaisyOnTtsEndOfStream() {
    using namespace mediaaccess;
    if (!g_d.book) return;
    if (!g_d.textOnlyMode) return;
    if (g_d.ttsPaused) return;  // User paused — don't auto-advance
    int next = g_d.currentSegment + 1;
    if (next >= (int)g_d.book->textSegments.size()) {
        SaveTtsPosition();
        Speak(Ts("End of book"));
        return;
    }
    StartTtsSegment(next);
}
