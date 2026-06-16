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
#include "mediaaccess/tts_player.h"          // TTS for text-only books
#include "mediaaccess/book_text_window.h"    // synced text-display window
#include "mediaaccess/player.h"              // GetEffectivePlaybackSpeed
#include "mediaaccess/globals.h"             // g_bookSkipMask / g_bookSkipBypass
#include "mediaaccess/ui.h"                  // SetNowPlaying et al.
#include "mediaaccess/utils.h"               // FormatTime for seek announcements
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

// Upper bound on consecutive clips/segments to skip in one auto-advance step
// when the user has opted out of categories (page numbers, footnotes, ...).
// Bounded to prevent infinite loops on pathological books where every entry
// is in a skip category.
static constexpr int kClipSkipSafetyBound = 5000;

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

    // TTS path for text-only books / EPUB without Media Overlays.
    // When textOnlyMode is true, currentClip / stream / endSync are unused;
    // currentSegment indexes into book->textSegments and SAPI handles
    // playback via the tts_player module. WM_TTS_END_OF_STREAM advances
    // to the next segment.
    bool     textOnlyMode    = false;
    int      currentSegment  = 0;
    bool     ttsPaused       = false;
    bool     ttsReading      = false;  // v2.35 — continuous-narration intent; gates auto-advance
};

static DaisyState g_d;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// Forward declaration — BuildLocationLabel is defined further down with
// the rest of the navigation helpers but used by StartClipPaused /
// StartTtsSegment for the now-playing item refresh.
static std::wstring BuildLocationLabel();

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
    // Refresh the title's "item" to the latest chapter/page label.
    // Cheap; the label only changes at chapter boundaries inside a long
    // book, and the window title comparison short-circuits no-op writes.
    if (g_d.book) SetNowPlayingItem(BuildLocationLabel());
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
    g_d.ttsReading = true;   // v2.35 — a new utterance is the active continuous read
    // Refresh title with the segment's chapter / page label.
    SetNowPlayingItem(BuildLocationLabel());
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

// Recursively delete a directory tree. Best-effort — we don't propagate
// errors because temp-folder cleanup must never break book unload.
static void RemoveDirectoryRecursive(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    std::wstring pat = dir + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryRecursive(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

void DaisyClose() {
    if (g_d.textOnlyMode) {
        SaveTtsPosition();
        TtsStop();
    } else {
        SaveCurrentPosition();
        FreeStream();
    }
    // Clean up the per-book temp folder if EPUB Media Overlays extracted
    // audio there. Done after FreeStream() so no files are held open.
    if (g_d.book && !g_d.book->tempAudioDir.empty()) {
        RemoveDirectoryRecursive(g_d.book->tempAudioDir);
    }
    g_d.book.reset();
    g_d.bookId = 0;
    g_d.currentClip = 0;
    g_d.currentOffset = 0.0;
    g_d.clipsBeforeDuration = 0.0;
    g_d.textOnlyMode = false;
    g_d.currentSegment = 0;
    g_d.ttsPaused = false;
    g_d.ttsReading = false;   // v2.35
    // v1.78 — Only clear the now-playing state if we were actually
    // showing a book. The comment below assumed every caller would
    // call SetNowPlaying right after (and many do), but YouTube cache
    // hit and similar paths set SetNowPlaying BEFORE calling LoadFile,
    // which calls DaisyClose first. Unconditionally clearing here wiped
    // out the caller's SetNowPlaying (e.g. the YouTube video title) and
    // left the main window blank — reported by user testing v1.78.
    // Now: if the current state isn't a book, leave it alone — the caller
    // knows better. If it IS a book, we still clear because the book is
    // genuinely closing.
    if (g_nowPlayingType == SourceType::Book) {
        ClearNowPlaying();
    }
}

bool DaisyLoadAndPlay(std::unique_ptr<DaisyBook> book, int bookId) {
    if (!book) return false;
    g_bookSkipBypass = false;  // Reset runtime skip-bypass on book change.

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
        // Show the book title + initial location in the window title
        // and announce path.
        SetNowPlaying(SourceType::Book, g_d.book->title, BuildLocationLabel());
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
    // Book title + initial location.
    SetNowPlaying(SourceType::Book, g_d.book->title, BuildLocationLabel());
    return true;
}

void DaisyPlay() {
    if (g_d.textOnlyMode) {
        if (g_d.ttsPaused) {
            TtsResume();
            g_d.ttsPaused = false;
            g_d.ttsReading = true;   // v2.35 — resuming continuous narration
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
        // v2.35 — clear the reading intent BEFORE purging. TtsStop() purges via
        // SPF_PURGEBEFORESPEAK, which emits an SPEI_END_INPUT_STREAM for the
        // stopped stream; without clearing intent first, DaisyOnTtsEndOfStream
        // would treat that as a natural end and auto-advance (the "cannot stop"
        // bug). The purged stream still carries the current stream number, so the
        // stream-number check alone would NOT catch it — intent is what stops it.
        g_d.ttsReading = false;
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
    // Announce the new position so the user knows where the seek
    // landed without having to query the status bar. Gated by
    // Options > Speech > "Announce position after seek".
    if (g_speechSeekPosition) SpeakW(FormatTime(target));
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
            count = g_d.textOnlyMode
                    ? (int)g_d.book->textSegments.size()
                    : (int)g_d.book->clips.size();
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
    // Text-only path — dispatch by navLevel just like the audio path,
    // so Shift+Right at heading-1 jumps to the next chapter, navLevel=0
    // moves page-by-page, and navLevel=-1 (phrase) moves segment-by-segment.
    if (g_d.textOnlyMode) {
        if (g_d.navLevel == -1) {
            // Phrase mode = next paragraph
            int next = g_d.currentSegment + 1;
            if (next >= (int)g_d.book->textSegments.size()) {
                Speak(Ts("End of book")); return;
            }
            StartTtsSegment(next);
            return;
        }
        // Find next navPoint at wantLevel with clipIndex > currentSegment.
        for (size_t i = 0; i < g_d.book->navPoints.size(); ++i) {
            const auto& np = g_d.book->navPoints[i];
            if (np.clipIndex <= g_d.currentSegment) continue;
            if (!LevelMatches(np.level,
                              np.kind == DaisyNavPoint::Page ? 1 : 0,
                              g_d.navLevel)) continue;
            StartTtsSegment(np.clipIndex);
            SpeakW(np.label);
            return;
        }
        Speak(Ts("End of book"));
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
        if (g_d.navLevel == -1) {
            if (g_d.currentSegment <= 0) { Speak(Ts("Beginning of book")); return; }
            StartTtsSegment(g_d.currentSegment - 1);
            return;
        }
        int found = -1;
        for (int i = (int)g_d.book->navPoints.size() - 1; i >= 0; --i) {
            const auto& np = g_d.book->navPoints[i];
            if (np.clipIndex >= g_d.currentSegment) continue;
            if (!LevelMatches(np.level,
                              np.kind == DaisyNavPoint::Page ? 1 : 0,
                              g_d.navLevel)) continue;
            found = i; break;
        }
        if (found < 0) { Speak(Ts("Beginning of book")); return; }
        StartTtsSegment(g_d.book->navPoints[found].clipIndex);
        SpeakW(g_d.book->navPoints[found].label);
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

// Compose a location label ("Chapter 3, page 47") for an arbitrary navPoint
// index (clip index in audio mode, segment index in text-only mode — the
// navPoints vector reuses clipIndex for both, see daisy_book.h). Returns an
// empty string if no book is loaded or the index has no preceding nav point.
static std::wstring BuildLocationLabelForIndex(int idx) {
    if (!g_d.book) return L"";
    if (idx < 0) return L"";
    std::wstring heading, page;
    for (const auto& np : g_d.book->navPoints) {
        if (np.clipIndex > idx) break;
        if (np.kind == DaisyNavPoint::Page)        page    = np.label;
        else if (np.level >= 1 && np.level <= 6)   heading = np.label;
    }
    std::wstring msg;
    if (!heading.empty()) msg = heading;
    if (!page.empty()) {
        if (!msg.empty()) msg += L", ";
        msg += page;
    }
    return msg;
}

// Current playback location — text-only segment or audio clip as appropriate.
static std::wstring BuildLocationLabel() {
    if (!g_d.book) return L"";
    int cur = g_d.textOnlyMode ? g_d.currentSegment : g_d.currentClip;
    return BuildLocationLabelForIndex(cur);
}

void DaisyAnnounceCurrentLocation() {
    if (!g_d.book) return;
    std::wstring msg = BuildLocationLabel();
    if (msg.empty()) msg = g_d.book->title;
    SpeakW(msg);
    // Keep the now-playing item synced with the spoken location.
    SetNowPlayingItem(msg);
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

// Bookmarks management helpers — used by ui_bookmarks to render labels
// for the bookmark list dialog.
std::wstring DaisyLocationLabelForClip(int clipIndex) {
    return BuildLocationLabelForIndex(clipIndex);
}

int DaisyCurrentBookId() {
    return g_d.bookId;
}

// Reading progress announcement (Ctrl+P from main menu).
//
// Format: "<P> percent, <location label>, about <H> hours <M> minutes remaining"
// (localized via T()/Ts()). Falls back gracefully when duration is unknown
// (text-only mode uses segment count and a characters-per-second estimate).

// Friendly duration formatter — "2 hours 15 minutes", "45 minutes",
// "less than a minute". Returns localized text.
static std::wstring FormatRemainingFriendly(double seconds) {
    if (seconds < 60.0) return T("less than a minute");
    int totalMin = (int)(seconds / 60.0);
    int h = totalMin / 60;
    int m = totalMin % 60;
    wchar_t buf[128];
    if (h > 0) {
        if (m > 0) {
            swprintf(buf, 128, L"%d %s %d %s", h,
                     (h > 1) ? T("hours") : T("hour"),
                     m, (m > 1) ? T("minutes") : T("minute"));
        } else {
            swprintf(buf, 128, L"%d %s", h,
                     (h > 1) ? T("hours") : T("hour"));
        }
    } else {
        swprintf(buf, 128, L"%d %s", m,
                 (m > 1) ? T("minutes") : T("minute"));
    }
    return buf;
}

void DaisyAnnounceProgress() {
    if (!g_d.book) { Speak(Ts("No book loaded")); return; }

    int    percent     = 0;
    double remainSec   = 0.0;
    bool   haveRemain  = false;
    std::wstring loc;

    if (g_d.textOnlyMode) {
        int n = (int)g_d.book->textSegments.size();
        if (n > 0) {
            percent = (int)(100.0 * (g_d.currentSegment + 1) / n);
            if (percent > 100) percent = 100;
            loc = BuildLocationLabelForIndex(g_d.currentSegment);

            // Estimate remaining: sum chars of remaining segments / chars-per-sec.
            // Base SAPI ~17 chars/sec at speed multiplier 1.0; scale.
            double remainChars = 0.0;
            for (int i = g_d.currentSegment + 1; i < n; ++i) {
                remainChars += (double)g_d.book->textSegments[i].text.size();
            }
            double mult = TtsGetSpeedMultiplier();
            if (mult < 0.1) mult = 0.1;
            double cps = 17.0 * mult;
            if (cps > 0.1) { remainSec = remainChars / cps; haveRemain = true; }
        }
    } else {
        double dur = DaisyGetBookDuration();
        double pos = DaisyGetBookPosition();
        if (dur > 0.5) {
            percent = (int)(100.0 * pos / dur);
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            double speed = GetEffectivePlaybackSpeed();
            if (speed < 0.01) speed = 0.01;
            remainSec = (dur - pos) / speed;
            if (remainSec < 0) remainSec = 0;
            haveRemain = true;
        }
        loc = DaisyLocationLabelForClip(g_d.currentClip);
    }

    // Build the announcement.
    wchar_t pctBuf[32];
    swprintf(pctBuf, 32, L"%d", percent);
    std::wstring msg = std::wstring(pctBuf) + L" " + T("percent");
    if (!loc.empty()) { msg += L", "; msg += loc; }
    if (haveRemain) {
        msg += L", "; msg += T("about");
        msg += L" "; msg += FormatRemainingFriendly(remainSec);
        msg += L" "; msg += T("remaining");
    }
    SpeakW(msg);
}

} // namespace mediaaccess

// -----------------------------------------------------------------------------
// Page-by-label lookup, exposed for books_dialog.cpp's "Go to page" prompt.
//
// Accepts a variety of user inputs and falls back to the nearest numeric
// match when no exact label exists:
//   - "47"                 → numeric, exact or nearest
//   - "p. 47" / "page 47"  → strip prefix, numeric
//   - "iii"                → roman numeral, numeric
//   - "Foreword"           → substring match on labels
//
// Many DAISY books label front-matter with roman numerals ("iii") and body
// pages with arabic ("47"). We treat the two number spaces separately so
// asking for "3" doesn't jump to "iii" — we prefer same-kind matches first.
// -----------------------------------------------------------------------------
namespace mediaaccess {

// Trim leading/trailing whitespace.
static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Lowercase ASCII fold for prefix matching.
static std::wstring LowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Roman numeral parser (1..3999). Returns -1 if input is not a valid roman.
// Accepts upper or lower case. Strict-ish: requires standard subtraction rules.
static int ParseRoman(const std::wstring& in) {
    if (in.empty()) return -1;
    auto val = [](wchar_t c) -> int {
        switch (towlower(c)) {
            case L'i': return 1;
            case L'v': return 5;
            case L'x': return 10;
            case L'l': return 50;
            case L'c': return 100;
            case L'd': return 500;
            case L'm': return 1000;
        }
        return 0;
    };
    int total = 0;
    int prev  = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        int v = val(in[i]);
        if (v == 0) return -1;
        if (prev && prev < v) total += v - 2 * prev;
        else                  total += v;
        prev = v;
    }
    return total > 0 ? total : -1;
}

// Try to extract a numeric value from a page label. Returns -1 if neither
// arabic nor roman could be parsed. `outIsRoman` tells the caller which
// number space the label lives in so we can prefer same-space matches.
static int ParseLabelAsNumber(const std::wstring& labelIn, bool* outIsRoman) {
    std::wstring s = Trim(labelIn);
    if (s.empty()) { if (outIsRoman) *outIsRoman = false; return -1; }

    // Strip a leading prefix like "Page ", "page ", "p. ", "p ".
    std::wstring lo = LowerW(s);
    static const wchar_t* kPrefixes[] = { L"page ", L"p. ", L"p." , L"p " };
    for (auto* p : kPrefixes) {
        size_t n = wcslen(p);
        if (lo.size() >= n && lo.compare(0, n, p) == 0) {
            s  = Trim(s.substr(n));
            lo = LowerW(s);
            break;
        }
    }

    // Arabic? Allow trailing non-digits (e.g. "47b").
    if (!s.empty() && iswdigit(s[0])) {
        int v = 0;
        size_t i = 0;
        while (i < s.size() && iswdigit(s[i])) {
            v = v * 10 + (s[i] - L'0');
            ++i;
        }
        if (outIsRoman) *outIsRoman = false;
        return v;
    }

    // Roman?
    int r = ParseRoman(s);
    if (r > 0) {
        if (outIsRoman) *outIsRoman = true;
        return r;
    }
    return -1;
}

static void JumpToClipAndAnnounce(int clipIndex, const std::wstring& announce) {
    bool wasPlaying = DaisyIsPlaying();
    StartClipPaused(clipIndex);
    if (wasPlaying) BASS_ChannelPlay(g_d.stream, FALSE);
    SpeakW(announce);
}

bool DaisyJumpToPageLabel(const std::wstring& qIn) {
    if (!g_d.book) return false;
    std::wstring q = Trim(qIn);
    if (q.empty()) return false;

    // 1) Exact label match (case-sensitive — DAISY labels are stable).
    for (const auto& np : g_d.book->navPoints) {
        if (np.kind != DaisyNavPoint::Page) continue;
        if (np.label == q) { JumpToClipAndAnnounce(np.clipIndex, np.label); return true; }
    }

    // 2) Numeric path — parse the query, find best match in same number space.
    bool qIsRoman = false;
    int qNum = ParseLabelAsNumber(q, &qIsRoman);
    if (qNum > 0) {
        const DaisyNavPoint* exact     = nullptr;
        const DaisyNavPoint* nearest   = nullptr;
        int                  nearestDist = 0;
        // Pass 1 — prefer same number space (roman vs arabic).
        for (const auto& np : g_d.book->navPoints) {
            if (np.kind != DaisyNavPoint::Page) continue;
            bool isRoman = false;
            int  n = ParseLabelAsNumber(np.label, &isRoman);
            if (n <= 0) continue;
            if (isRoman != qIsRoman) continue;
            if (n == qNum) { exact = &np; break; }
            int d = (n > qNum) ? (n - qNum) : (qNum - n);
            if (!nearest || d < nearestDist) { nearest = &np; nearestDist = d; }
        }
        // Pass 2 — fall back to opposite number space if nothing matched.
        if (!exact && !nearest) {
            for (const auto& np : g_d.book->navPoints) {
                if (np.kind != DaisyNavPoint::Page) continue;
                bool isRoman = false;
                int  n = ParseLabelAsNumber(np.label, &isRoman);
                if (n <= 0) continue;
                if (n == qNum) { exact = &np; break; }
                int d = (n > qNum) ? (n - qNum) : (qNum - n);
                if (!nearest || d < nearestDist) { nearest = &np; nearestDist = d; }
            }
        }
        if (exact) { JumpToClipAndAnnounce(exact->clipIndex, exact->label); return true; }
        if (nearest) {
            std::wstring announce = std::wstring(T("Nearest page:")) + L" " + nearest->label;
            JumpToClipAndAnnounce(nearest->clipIndex, announce);
            return true;
        }
    }

    // 3) Substring match on the label (case-insensitive).
    std::wstring qLo = LowerW(q);
    for (const auto& np : g_d.book->navPoints) {
        if (np.kind != DaisyNavPoint::Page) continue;
        if (LowerW(np.label).find(qLo) != std::wstring::npos) {
            JumpToClipAndAnnounce(np.clipIndex, np.label);
            return true;
        }
    }
    return false;
}
} // namespace mediaaccess (DaisyJumpToPageLabel)

// -----------------------------------------------------------------------------
// C-linkage hook for the WM_DAISY_NEXT_CLIP handler in main.cpp.
// -----------------------------------------------------------------------------
// True if the user has asked to skip this category (via g_bookSkipMask
// set from Options > Books) and the runtime bypass isn't temporarily
// disabling the skip filter.
static inline bool BookSkipShouldSkip(mediaaccess::SkipKind k) {
    if (k == mediaaccess::SkipKind::None) return false;
    if (g_bookSkipBypass)    return false;
    uint32_t bit = 1u << (uint32_t)((uint8_t)k - 1);  // Page=bit0, Note=bit1...
    return (g_bookSkipMask & bit) != 0;
}

extern "C" void DaisyOnClipEnded(int endedClipIndex) {
    using namespace mediaaccess;
    if (!g_d.book) return;
    // Defensive: only advance if the sync that fired matches the clip we
    // currently think is playing (avoids races on user-initiated skips).
    if (endedClipIndex != g_d.currentClip) return;
    int next = g_d.currentClip + 1;
    // Skip categories the user has opted out of (page numbers, footnotes,
    // etc.). Bounded by kClipSkipSafetyBound to avoid infinite loops on
    // pathological books where every clip is in a skip category.
    int safety = 0;
    while (next < (int)g_d.book->clips.size() && safety < kClipSkipSafetyBound &&
           BookSkipShouldSkip(g_d.book->clips[next].skipKind)) {
        ++next; ++safety;
    }
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
extern "C" void DaisyOnTtsEndOfStream(unsigned long endedStream) {
    using namespace mediaaccess;
    if (!g_d.book) return;
    if (!g_d.textOnlyMode) return;
    // v2.35 — auto-advance ONLY on the natural end of the CURRENT utterance while
    // continuous reading is intended. This rejects the three look-alike cases:
    //  (b) Stop purge -> ttsReading is false;
    //  (a) pause -> ttsPaused;
    //  (c) a stale/purged stream from a navigation jump -> its number no longer
    //      matches the latest utterance (TtsLastStreamNumber), even when the
    //      late end-event is delivered after we've started the new segment.
    if (!g_d.ttsReading) return;
    if (g_d.ttsPaused) return;
    if (endedStream != TtsLastStreamNumber()) return;
    int next = g_d.currentSegment + 1;
    int safety = 0;
    while (next < (int)g_d.book->textSegments.size() && safety < kClipSkipSafetyBound &&
           BookSkipShouldSkip(g_d.book->textSegments[next].skipKind)) {
        ++next; ++safety;
    }
    if (next >= (int)g_d.book->textSegments.size()) {
        g_d.ttsReading = false;   // v2.35 — narration genuinely ended
        SaveTtsPosition();
        Speak(Ts("End of book"));
        return;
    }
    StartTtsSegment(next);
}
