#pragma once
#ifndef MEDIAACCESS_DAISY_PLAYER_H
#define MEDIAACCESS_DAISY_PLAYER_H

// =============================================================================
// daisy_player.h — playback engine for DAISY/EPUB books
//
// Wraps BASS to play the linear sequence of audio clips parsed by daisy_book.h,
// preserves structural navigation (heading levels and pages), persists the
// last-read position per book, and exposes a small API for keyboard handlers.
//
// Phase 1: audio playback + navigation. Text display and TTS for text-only
// books come in Phase 2.
// =============================================================================

#include "daisy_book.h"

#include <memory>
#include <string>

namespace mediaaccess {

// Load a book and start playback from its saved position (if any).
// Takes ownership of the book. The bookId is the database row id used to
// persist last-read position and bookmarks. Returns false if the book has
// no playable clips (caller should fall back to TTS in Phase 2).
bool DaisyLoadAndPlay(std::unique_ptr<DaisyBook> book, int bookId);

// True if a DAISY book is currently loaded (regardless of play/pause state).
bool DaisyIsActive();

// Tear down the current book — stops audio, frees streams, saves position.
// Safe to call when nothing is loaded.
void DaisyClose();

// Standard transport. No-op when no book loaded.
void DaisyPlayPause();
void DaisyPlay();
void DaisyPause();
void DaisyStop();           // Pause + rewind to clip start
void DaisySeekRelative(double deltaSeconds);

// Returns playback position in seconds since the start of the book
// (i.e. the time the user has been listening to the book content,
// summed across all completed clips). 0 when no book loaded.
double DaisyGetBookPosition();

// Returns the total length of the book in seconds.
double DaisyGetBookDuration();

// Returns true while a book is playing.
bool DaisyIsPlaying();
bool DaisyIsPaused();

// Apply the current g_volume to the active book stream. Called by SetVolume()
// in player.cpp whenever the user changes volume (Ctrl+Up / Ctrl+Down) so the
// DAISY stream tracks the same volume control as regular audio playback.
// Also called internally on every clip transition so new streams inherit it.
void DaisyApplyVolume();

// v2.48 — the Rate effect changed (Options/keys). For an Edge-narrated book the
// neural voice bakes speed at synthesis time, so this re-synthesizes the current
// paragraph at the new rate. `multiplier` is MediaAccess's speed convention
// (1.0 = normal). No-op unless an Edge book is currently being read.
void DaisyOnSpeedChanged(double multiplier);

// =============================================================================
// Structural navigation
// =============================================================================
//
// The book has a list of nav points (headings h1..h6 and pages). The user
// picks a "current navigation level" with Shift+Up/Down, then jumps to the
// previous/next nav point at that level with Shift+Left/Right.
//
// Levels:  1..6 = heading levels   0 = page   -1 = phrase (clip granularity)

int  DaisyGetNavLevel();
void DaisySetNavLevel(int level);         // Clamps to available levels in book
void DaisyCycleNavLevel(int direction);   // +1 = next available level, -1 = prev

// Jump to the next/previous nav point matching the current nav level. If
// level is -1 (phrase), advances/backs up by one clip. Speaks the new label
// via the standard MediaAccess Speak() infrastructure.
void DaisyNavigateForward();
void DaisyNavigateBackward();

// Announce where we are in the book ("Chapter 3, page 47") via Speak().
// Called automatically after navigation; expose for bound-to-key access too.
void DaisyAnnounceCurrentLocation();

// Phase 4 — announce reading progress: "47 percent, Chapter 3, page 47,
// about 2 hours 15 minutes remaining". Bound to Shift+P by default.
// Works for both audio (uses Duration / effective speed) and text-only
// (uses segment count and a SAPI characters-per-second estimate).
void DaisyAnnounceProgress();

// =============================================================================
// Bookmarks (with optional note)
// =============================================================================

// Add a bookmark at the current position. If 'note' is empty, no prompt is
// shown; otherwise the caller has already collected the note text from the
// user via a dialog. Returns the bookmark id or 0 on failure.
int DaisyAddBookmarkHere(const std::wstring& note);

// Jump to the bookmark's saved position.
void DaisyJumpToBookmark(int clipIndex, double offsetSeconds);

// Build a human-readable location label ("Chapter 3, page 47") for any
// clip index in the current book. Walks navPoints to find the most recent
// heading and page at or before the clip. Returns an empty string if no
// book is loaded. Used by the bookmarks list dialog.
std::wstring DaisyLocationLabelForClip(int clipIndex);

// Returns the database book id for the currently loaded book, or 0 if none.
// Exposed so the bookmarks dialog can query GetBookBookmarks(bookId).
int DaisyCurrentBookId();

// Jump to a specific text segment in TTS mode. No-op for audio books.
// Used by F3 search hits and by the navigation in text-only mode.
void DaisyJumpToSegment(int segmentIndex);

// Returns true if the current book has text content (clip.textContent set
// or non-empty textSegments). Used by the search dialog and other UI.
bool DaisyHasText();

// Read-only accessor for the currently loaded book (for search, etc.).
// Returns nullptr if no book is loaded.
const DaisyBook* DaisyCurrentBook();

} // namespace mediaaccess

#endif // MEDIAACCESS_DAISY_PLAYER_H
