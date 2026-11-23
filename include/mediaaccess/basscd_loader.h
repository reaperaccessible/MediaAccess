#pragma once
// basscd_loader — dynamic loading of basscd.dll (audio CD access).
//
// basscd.dll ships in lib\ but has no import library, and MediaAccess must
// start even when it is missing. It is loaded on first use by its full path
// (LoadLibraryExW on {exeDir}\lib\basscd.dll, the video_engine.cpp pattern:
// SetDllDirectoryW is ignored since SetDefaultDllDirectories), then every
// function is fetched with GetProcAddress.
//
// Track numbers are 0-based everywhere in the BASSCD API.

#include <windows.h>
#include <string>
#include "bass.h"
#include "basscd.h"

namespace mediaaccess {
namespace basscd {

struct Api {
    BOOL        (WINAPI *GetInfo)(DWORD drive, BASS_CD_INFO* info);
    BOOL        (WINAPI *Door)(DWORD drive, DWORD action);
    BOOL        (WINAPI *DoorIsOpen)(DWORD drive);
    BOOL        (WINAPI *IsReady)(DWORD drive);
    DWORD       (WINAPI *GetTracks)(DWORD drive);
    DWORD       (WINAPI *GetTrackLength)(DWORD drive, DWORD track);
    BOOL        (WINAPI *GetTOC)(DWORD drive, DWORD mode, BASS_CD_TOC* toc);
    const char* (WINAPI *GetID)(DWORD drive, DWORD id);
    BOOL        (WINAPI *Release)(DWORD drive);
    // file: a "TrackNN.cda" path, wide when flags has BASS_UNICODE.
    HSTREAM     (WINAPI *StreamCreateFile)(const char* file, DWORD flags);
    HSTREAM     (WINAPI *StreamCreate)(DWORD drive, DWORD track, DWORD flags);
    BOOL        (WINAPI *SetSpeed)(DWORD drive, DWORD speed);
    BOOL        (WINAPI *SetOffset)(DWORD drive, int offset);   // read offset, in samples
    // Crossfade, phase 4: CD tracks one after another on the same stream.
    DWORD       (WINAPI *StreamGetTrack)(HSTREAM handle);            // LOWORD track, HIWORD drive
    BOOL        (WINAPI *StreamSetTrack)(HSTREAM handle, DWORD track);
};

// Loads basscd.dll once. Returns nullptr when it cannot be loaded; LastError()
// then says why (for the log and the message box).
const Api* Get();
std::wstring LastError();

} // namespace basscd
} // namespace mediaaccess
