#pragma once
// cd_ripper — extracting audio CD tracks to files, on a worker thread.
// Phase 2 of docs/PLAN_EXTRACTION_CD.md: Fast mode (one read per track),
// WAV and FLAC. Each file is written as "<name>.part" and renamed at the end,
// so a cancelled or failed track never leaves a truncated file under its
// real name.

#include <windows.h>
#include <string>
#include <vector>
#include "disc_drives.h"

namespace mediaaccess {

enum class RipFormat { WAV = 0, FLAC = 1 };

struct RipTrack {
    int          index;      // 0-based BASSCD track number
    int          number;     // number on the disc
    double       seconds;
    std::wstring fileName;   // without folder or extension
};

struct RipJob {
    DiscDrive             drive;
    std::vector<RipTrack> tracks;
    RipFormat             format = RipFormat::FLAC;
    std::wstring          folder;  // created if missing
};

struct RipProgress {
    int    trackPos = 0;        // 1-based position in the job
    int    trackCount = 0;
    double trackFraction = 0;   // 0..1
    double totalFraction = 0;   // 0..1, by duration
    double secondsLeft = -1;    // estimate, -1 until known
};

// Messages posted to the window given to StartRip (after IsWindow).
#define WM_CDRIP_TRACK_START  (WM_APP + 0x300)   // wParam: 1-based position, lParam: MAKELPARAM(count, disc number)
#define WM_CDRIP_DONE         (WM_APP + 0x301)   // wParam: tracks written
#define WM_CDRIP_CANCELLED    (WM_APP + 0x302)   // wParam: tracks written before the stop
#define WM_CDRIP_ERROR        (WM_APP + 0x303)   // lParam: std::wstring* (receiver deletes)

bool StartRip(const RipJob& job, HWND notify);   // false if one is already running
void CancelRip();                                // asynchronous; CANCELLED follows
void CancelRipAndWait();                         // for the application exit
bool IsRipping();
RipProgress GetRipProgress();

} // namespace mediaaccess
