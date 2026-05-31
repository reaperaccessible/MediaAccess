#pragma once
#ifndef MEDIAACCESS_PLAYER_H
#define MEDIAACCESS_PLAYER_H

#include <windows.h>
#include <string>
#include <vector>
#include "bass.h"

struct Chapter;  // forward decl, full struct in types.h

// BASS initialization
bool InitBass(HWND hwnd);
void FreeBass();
void LoadBassPlugins();
std::wstring GetLoadedPluginsInfo();

// Playback control
bool LoadFile(const wchar_t* path);
// LoadURL — load and play a stream URL.
//   silentOnFail = true → suppresses the BASS error MessageBox so the caller
//   can attempt a fallback (used by the YouTube path to retry via libmpv).
bool LoadURL(const wchar_t* url, bool silentOnFail = false);
bool IsURL(const wchar_t* path);
void PlayPause();
void Play();
void Pause();
void Stop();
void FreeCurrentStream();

// Seeking
void Seek(double seconds);
void SeekTracks(int tracks);
void SeekToPosition(double seconds);

// v1.79 — Granular seek. unitIdx in [0..12]: 0..8 map 1:1 to the time entries
// of g_seekAmounts (1 s, 5 s, 10 s, 30 s, 1 m, 5 m, 10 m, 30 m, 1 h); 9..11
// are 1/5/10 tracks; 12 is "1 chapter". direction is -1 (back) or +1 (forward).
// Fallbacks mirror the legacy IDM_PLAY_SEEKBACK/FWD handler: track jumps fall
// back to the first enabled time unit when the playlist has 0/1 item, and
// chapter jumps no-op silently when the current file has no chapters.
void PerformGranularSeek(int unitIdx, int direction);
double GetCurrentPosition();

// Chapter support
void ParseChapters(HSTREAM stream);
bool SeekToNextChapter();
bool SeekToPrevChapter();
int GetCurrentChapterIndex();

// Volume
void SetVolume(float vol);
void ToggleMute();

// Track navigation
void NextTrack(bool autoPlay = true);
void PrevTrack();
void PlayTrack(int index, bool autoPlay = true);
void ToggleRepeatMode();

// Track end callback
void CALLBACK OnTrackEnd(HSYNC handle, DWORD channel, DWORD data, void* user);

// Stream metadata change callback (for internet radio)
void CALLBACK OnMetaChange(HSYNC handle, DWORD channel, DWORD data, void* user);
void AnnounceStreamMetadata();

// Device management
bool ReinitBass(int device);
int FindDeviceByName(const std::wstring& name);
std::wstring GetDeviceName(int device);
void ShowAudioDeviceMenu(HWND hwnd);
void SelectAudioDevice(int deviceIndex);

// Speak functions
void SpeakElapsed();
void SpeakRemaining();
void SpeakTotal();

// Tag reading functions (speak ID3/metadata tags)
void SpeakTagTitle();
void SpeakTagArtist();
void SpeakTagAlbum();
void SpeakTagYear();
void SpeakTagTrack();
void SpeakTagGenre();
void SpeakTagComment();
void SpeakTagBitrate();
int GetCurrentBitrate();  // Returns current stream bitrate in kbps, or 0 if unavailable
void SpeakTagDuration();
void SpeakTagFilename();

// Tag retrieval functions (return tag text for display)
std::wstring GetTagTitle();
std::wstring GetTagArtist();
std::wstring GetTagAlbum();
std::wstring GetTagYear();
std::wstring GetTagTrack();
std::wstring GetTagGenre();
std::wstring GetTagComment();
std::wstring GetTagBitrate();
std::wstring GetTagDuration();
std::wstring GetTagFilename();

// Recording functions
void ToggleRecording();
void StopRecording();

// Expand friendly date tokens ({année}/{year}/etc.) to strftime codes (%Y/%m/etc.)
std::wstring ExpandFilenameTokens(const std::wstring& tmpl);

// Externally provided chapter list (e.g. Podcast 2.0 RSS chapter JSON).
// Call BEFORE PlayTrack/LoadFile/LoadURL so the URL parser preserves them
// instead of overwriting with whatever it finds in the audio file.
// Pass an empty vector to clear.
void SetExternalChapters(const std::vector<Chapter>& chapters);

double GetTrackLength();
bool IsCurrentlyPlaying();
bool IsCurrentlyPaused();
bool IsCurrentlyStopped();

// Effective playback-speed multiplier combining tempo and rate effects.
// Returns 1.0 when both are at default. Used by time displays to convert
// nominal source-content seconds into real wall-clock seconds — a 33-min
// file played at 3.0 reports 11 min total / position / remaining.
// Pitch alone does not affect duration and is ignored. Always >= 0.01 to
// avoid divide-by-zero when the user inches tempo toward -100%.
double GetEffectivePlaybackSpeed();

#endif // MEDIAACCESS_PLAYER_H
