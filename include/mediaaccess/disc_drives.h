#pragma once
// disc_drives — optical drives and the audio CD in them, through BASSCD.
// Shared with PLAN_CD_DVD.md (CD/DVD playback, later).

#include <windows.h>
#include <string>
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

} // namespace mediaaccess
