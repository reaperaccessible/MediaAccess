#pragma once
// cd_ripper — extracting audio CD tracks to files, on a worker thread
// (docs/PLAN_EXTRACTION_CD.md). Fast mode: one read per track. Formats: WAV,
// FLAC, MP3, Ogg Vorbis, Opus (Opus only when lib\bassenc_opus.dll is there).
// Tags and the front cover go into each file (BASSenc options, like the flac,
// lame, oggenc and opusenc tools); a CUE sheet and an M3U8 playlist can be
// written next to them. Each file is written as "<name>.part" and renamed at
// the end, so a cancelled or failed track never leaves a truncated file under
// its real name.

#include <windows.h>
#include <string>
#include <vector>
#include "disc_drives.h"
#include "cd_metadata.h"

namespace mediaaccess {

// Values stored in [CdRip] Format: keep them stable.
enum class RipFormat { WAV = 0, FLAC = 1, MP3 = 2, OGG = 3, OPUS = 4 };

bool IsOpusAvailable();                          // bassenc_opus.dll can be loaded
const wchar_t* RipFormatExtension(RipFormat f);  // L".flac"...

// Quality choices of a format (index into a fixed list; WAV has one).
int  RipQualityCount(RipFormat f);
int  RipDefaultQuality(RipFormat f);
std::wstring RipQualityLabel(RipFormat f, int index);     // translated
std::wstring RipQualityLabelEn(RipFormat f, int index);   // for the log (English)
std::wstring RipFormatNameEn(RipFormat f);

struct RipTrack {
    int          index;      // 0-based BASSCD track number
    int          number;     // number on the disc
    double       seconds;
    int          audioPos;   // 0-based position among the disc's audio tracks (AccurateRip)
    std::wstring fileName;   // relative to the album folder, without extension (may hold "\")
    std::wstring title, artist, isrc, recordingMbid;
};

struct RipJob {
    DiscDrive             drive;
    std::vector<RipTrack> tracks;
    RipFormat             format = RipFormat::FLAC;
    int                   quality = 0;      // index, see RipQualityLabel
    std::wstring          folder;           // album folder, created if missing
    CdReleaseMeta         meta;             // album tags and the cover
    int                   trackTotal = 0;   // audio tracks on the disc (TRACKTOTAL)
    bool embedCover = true, saveFolderJpg = true, writeCue = true, writeM3u = true;
    std::wstring          listName;         // CUE / M3U8 / log file name, without extension
    bool secure = false;                    // two reads compared, blocks re-read (plan 4.3)
    bool verify = true;                     // AccurateRip
    bool writeLog = true;
    bool offsetKnown = false;               // readOffset given; else saved / AccurateRip list / 0
    int  readOffset = 0;                    // samples, for BASS_CD_SetOffset
    std::wstring discId;                    // MusicBrainz disc ID, for the log
};

// What the window needs at the end (WM_CDRIP_DONE / WM_CDRIP_SUMMARY).
struct RipSummary {
    int written = 0, accurate = 0;
    bool verified = false, notInDb = false;  // AccurateRip found the disc / does not know it
    std::vector<int> notAccurate;            // disc numbers of the tracks AccurateRip did not confirm
    std::vector<int> suspicious;             // disc numbers with suspicious blocks (Secure)
    std::wstring folder, logPath;
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
#define WM_CDRIP_DONE         (WM_APP + 0x301)   // wParam: tracks written, lParam: RipSummary* (receiver deletes)
#define WM_CDRIP_CANCELLED    (WM_APP + 0x302)   // wParam: tracks written before the stop
#define WM_CDRIP_ERROR        (WM_APP + 0x303)   // lParam: std::wstring* (receiver deletes)
#define WM_CDRIP_TRACK_DONE   (WM_APP + 0x304)   // wParam: disc number, lParam: suspicious blocks (> 0 only)
#define WM_CDRIP_SUMMARY      (WM_APP + 0x305)   // after a stop or an error: lParam RipSummary* (receiver deletes)

bool StartRip(const RipJob& job, HWND notify);   // false if one is already running
void CancelRip();                                // asynchronous; CANCELLED follows
void CancelRipAndWait();                         // for the application exit
bool IsRipping();
RipProgress GetRipProgress();

} // namespace mediaaccess
