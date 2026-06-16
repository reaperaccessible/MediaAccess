#pragma once
#ifndef MEDIAACCESS_VIDEO_ENGINE_H
#define MEDIAACCESS_VIDEO_ENGINE_H

#include <windows.h>
#include <string>

// Populated by LoadMPVLibrary on failure: human-readable reason such as
// "LoadLibraryExW(...) failed: <error string> (0x80070002)". Empty on
// success. Read by player.cpp to surface in the "please reinstall" dialog
// so users can paste it back to us for diagnosis.
extern std::wstring g_lastMpvLoadError;

// ===== Initialization =====
bool IsMPVAvailable();
bool InitMPV(HWND parentHwnd);
void FreeMPV();
bool IsMPVInitialized();

// ===== Detection =====
bool IsVideoFile(const std::wstring& path);

// ===== Playback =====
bool MPVLoadFile(const wchar_t* path);
bool MPVLoadURL(const wchar_t* url);
void MPVPlay();
void MPVPause();
void MPVStop();
void MPVPlayPause();

// Toggle mpv's video output. Pass true before loading a URL when you only
// want the audio decoded (e.g. YouTube hybrid streaming) so no video
// window is shown and no video decoder is spun up.
void MPVSetAudioOnly(bool audioOnly);

// ===== Seek / Position =====
void MPVSeek(double seconds);
void MPVSeekToPosition(double seconds);
double MPVGetPosition();
double MPVGetLength();

// ===== Volume =====
void MPVSetVolume(float vol);
void MPVSetMute(bool mute);

// Single owner of the mpv video volume (v2.44). Every write to the mpv volume
// must go through ApplyVideoVolume() — it pushes g_volume scaled by the current
// subtitle-duck multiplier, so a user volume change during a spoken subtitle
// keeps the duck, and a duck change keeps the user's volume. Never call
// MPVSetVolume(g_volume) directly from outside this owner.
void ApplyVideoVolume();

// Set the subtitle-duck multiplier [0..1] applied on top of g_volume and re-apply
// immediately. 1.0 = no ducking (full volume). Driven by the subtitle reader's
// fade ramp.
void MPVSetDuck(float mul);

// ===== State =====
bool MPVIsPlaying();
bool MPVIsPaused();
bool MPVIsStopped();

// ===== Fullscreen =====
void MPVToggleFullscreen(HWND mainHwnd);
bool MPVIsFullscreen();

// ===== Subtitles =====
int MPVGetSubtitleTrackCount();
std::wstring MPVGetSubtitleTrackName(int index);
void MPVSetSubtitleTrack(int index);
void MPVCycleSubtitles();
bool MPVLoadExternalSubtitle(const wchar_t* path);
// ffmpeg stream index (container-wide, 0-based) of the active subtitle track,
// or -1 if none is selected. Used to extract exactly the track being watched.
long MPVGetActiveSubtitleFfIndex();
// Codec name of the active subtitle track (e.g. "subrip", "ass", "hdmv_pgs_subtitle"),
// or "" if none. Used to detect image-based tracks that cannot be read aloud.
std::wstring MPVGetActiveSubtitleCodec();
// Current playback speed (1.0 = normal). Used to widen the subtitle lookahead.
double MPVGetSpeed();

// ===== Audio Tracks =====
int MPVGetAudioTrackCount();
std::wstring MPVGetAudioTrackName(int index);
void MPVSetAudioTrack(int index);
void MPVCycleAudioTracks();

// ===== Aspect Ratio =====
void MPVCycleAspectRatio();
std::wstring MPVGetCurrentAspectRatio();

// ===== Speed / OSD / Screenshot =====
void MPVSetSpeed(double speed);
void MPVShowOSD(const wchar_t* text, int durationMs = 2000);
void MPVTakeScreenshot();

// ===== Chapters =====
int MPVGetChapterCount();
std::wstring MPVGetChapterTitle(int index);
void MPVSeekToChapter(int index);

// ===== Subtitle navigation (v1.83) =====
// Jump to the previous (delta < 0) or next (delta > 0) subtitle event in the
// currently active sub track. No-op when MPV isn't loaded or no sub track is
// active. The new sub-text line is spoken automatically via WM_SPEAK_SUBTITLE
// when g_speakSubtitles is on (v1.81 mechanism).
void MPVSubSeek(int delta);

// ===== Metadata =====
std::wstring MPVGetMediaTitle();

// v2.13 — technical description of the current video (resolution, codec,
// bitrate) for the "bitrate" announcement shortcut. Empty if unavailable.
std::wstring MPVGetVideoInfo();

#endif // MEDIAACCESS_VIDEO_ENGINE_H
