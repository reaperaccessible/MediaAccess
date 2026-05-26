#pragma once
#ifndef MEDIAACCESS_VIDEO_ENGINE_H
#define MEDIAACCESS_VIDEO_ENGINE_H

#include <windows.h>
#include <string>

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

// ===== Seek / Position =====
void MPVSeek(double seconds);
void MPVSeekToPosition(double seconds);
double MPVGetPosition();
double MPVGetLength();

// ===== Volume =====
void MPVSetVolume(float vol);
void MPVSetMute(bool mute);

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

// ===== Metadata =====
std::wstring MPVGetMediaTitle();

#endif // MEDIAACCESS_VIDEO_ENGINE_H
