#pragma once
// disc_drives — optical drives and the audio CD in them, through BASSCD.
// Shared with PLAN_CD_DVD.md (CD/DVD playback, later).

#include <windows.h>
#include <string>
#include "bass.h"
#include <vector>

namespace mediaaccess {

enum class DriveMediaType { None, AudioCD, DataCD, Unknown };

struct DiscDrive {
    wchar_t      letter = L'?';   // L'?' when BASSCD does not know it
    DWORD        bassIndex = 0;   // drive number for the BASSCD API
    std::wstring vendorModel;     // vendor + product: key of the remembered read offset
    bool         canOpen = false; // the tray can be opened by software
};

struct DiscTrack {
    int    index = 0;             // 0-based BASSCD track number
    int    number = 1;            // number printed on the disc (TOC)
    double seconds = 0.0;
    bool   isData = false;
};

struct DiscContents {
    DriveMediaType            type = DriveMediaType::None;
    std::vector<DiscTrack>    tracks;        // audio and data tracks, TOC order
    int    AudioTrackCount() const;
    double AudioSeconds() const;
};

// Every optical drive BASSCD sees. Empty when there is none or when
// basscd.dll cannot be loaded (basscd::LastError() says which).
std::vector<DiscDrive> EnumerateOpticalDrives();

// What is in the drive right now (no disc, audio CD, data disc).
DiscContents ReadDiscContents(const DiscDrive& drive);

bool IsDriveTrayOpen(const DiscDrive& drive);
// Opens the tray, or closes it when it is already open.
bool ToggleDriveTray(const DiscDrive& drive, bool* nowOpen);

// Lets other programs use the drive again (BASS keeps a handle open).
void ReleaseDrive(const DiscDrive& drive);

// ---- Playing a CD track opened as a file ("E:\Track01.cda") ----
// Windows shows each audio track as a 44-byte .cda file: it holds no sound,
// only where the track is. BASSCD reads the track itself from it.
bool IsCdaFile(const wchar_t* path);
// A BASS stream for that track (0 on failure, BASS_ErrorGetCode says why).
// Opening a second stream on the same drive frees the first one
// (BASS_CONFIG_CD_FREEOLD): never call it just to measure a track.
HSTREAM OpenCdaStream(const wchar_t* path, DWORD flags);
// Length of that track from the disc's table of contents, without opening a
// stream (safe while the track is playing). 0 when unknown.
double CdaTrackSeconds(const wchar_t* path);

} // namespace mediaaccess
