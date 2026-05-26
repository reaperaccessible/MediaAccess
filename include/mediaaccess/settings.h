#pragma once
#ifndef MEDIAACCESS_SETTINGS_H
#define MEDIAACCESS_SETTINGS_H

#include <windows.h>
#include <string>

// Config path initialization
void InitConfigPath();

// Settings load/save
void LoadSettings();
void SaveSettings();
void LoadDSPSettings();  // Call after InitEffects()

// Playback state persistence
void SavePlaybackState();
void LoadPlaybackState();

// Per-file position tracking
void SaveFilePosition(const std::wstring& filePath);
double LoadFilePosition(const std::wstring& filePath);

// Seek amount cycling
void CycleSeekAmount(int direction);
double GetCurrentSeekAmount();
bool IsSeekAmountAvailable(int index);
void SpeakSeekAmount();

// Recent files
void AddToRecentFiles(const std::wstring& filePath);
void UpdateRecentFilesMenu(HMENU hMenu);

// Video settings getters
bool GetHwdecEnabled();
const std::wstring& GetVideoOutput();

#endif // MEDIAACCESS_SETTINGS_H
