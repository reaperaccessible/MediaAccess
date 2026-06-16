#pragma once
#ifndef MEDIAACCESS_CUE_SHEET_H
#define MEDIAACCESS_CUE_SHEET_H

#include <string>
#include <vector>

#include "globals.h"   // Chapter

// =============================================================================
// cue_sheet.h — .cue sheet support (tester Séb).
//
// Two scenarios, handled with EXISTING infrastructure (no new plumbing):
//
//   Single-FILE cue  — one audio file, N tracks. We load the one file and
//                      inject the tracks as CHAPTERS via SetExternalChapters().
//                      Track-to-track navigation then reuses the existing
//                      chapter Prev/Next actions (SEEK_BACK/FWD_CHAPTER).
//
//   Multi-FILE cue   — each FILE becomes one g_playlist entry (file-level
//                      split). Multi-track-per-FILE inside a multi-FILE cue is
//                      a documented limitation: only the FILE split is honored.
//
// Charset: the .cue is read as RAW BYTES and decoded via MultiByteToWideChar
// (UTF-8 / UTF-16 LE / UTF-16 BE BOM sniff, CP_UTF8 then CP_ACP fallback) so
// French accents survive and UTF-16 NUL bytes don't break a line loop.
// =============================================================================

struct CueTrack {
    int          number = 0;     // TRACK nn (1-based as written in the file)
    std::wstring title;          // TITLE "..."  (empty -> "Track N" fallback)
    std::wstring performer;      // PERFORMER "..." (track-level, may be empty)
    double       startSec = 0.0; // INDEX 01 mm:ss:ff converted to seconds
    int          fileIndex = 0;  // which entry of CueSheet::files this belongs to
};

struct CueSheet {
    std::wstring albumTitle;        // disc-level TITLE (before first TRACK)
    std::wstring albumPerformer;    // disc-level PERFORMER
    std::vector<std::wstring> files; // resolved absolute audio paths, in order
    std::vector<CueTrack> tracks;    // all AUDIO tracks, in file order

    bool multiFile() const { return files.size() > 1; }
};

// True when the path's extension is ".cue" (case-insensitive).
bool IsCueFile(const std::wstring& path);

// Parse a .cue file. Returns false on unreadable file or zero AUDIO tracks.
// Does NOT touch playback state — pure parse.
bool ParseCueSheet(const std::wstring& cuePath, CueSheet& out);

// Convert the tracks belonging to files[fileIndex] into Chapters, sorted by
// start time. The performer is folded into the name when it differs from the
// album performer:  "NN. Title - Performer".
std::vector<Chapter> CueToChapters(const CueSheet& sheet, int fileIndex = 0);

// Orchestrator: parse + load + play. Single-FILE -> inject chapters + load the
// one audio file. Multi-FILE -> populate g_playlist from the files and play the
// first. Announces and aborts cleanly on missing audio / unreadable cue / no
// tracks. Returns true on success.
// restoreMode (v2.34): true when called from startup state-restore — loads the
// audio PAUSED (like the normal restore path, not auto-playing) and stays SILENT
// on errors (a missing cue/audio at launch must not speak; the caller falls back
// to the generic restore). Interactive opens use the default (auto-play + spoken
// errors).
bool OpenCueSheet(const std::wstring& cuePath, bool restoreMode = false);

// Announce the current track ("Track N of M: name") for the active cue. Bound
// to the user-assignable CUE_ANNOUNCE_TRACK action.
void AnnounceCurrentCueTrack();

// Show the accessible track-list dialog (Enter on a track jumps to it).
void ShowTrackListDialog();

#endif // MEDIAACCESS_CUE_SHEET_H
