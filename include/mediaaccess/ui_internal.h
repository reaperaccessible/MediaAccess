#pragma once

// Internal header shared between ui.cpp and ui_*.cpp split modules.
// Not part of the public interface — only #include from UI implementation files.

#include "ui.h"
#include "globals.h"
#include "utils.h"
#include "player.h"
#include "settings.h"
#include "hotkeys.h"
#include "tray.h"
#include "accessibility.h"
#include "effects.h"
#include "tempo_processor.h"
#include "convolution.h"
#include "database.h"
#include "download_manager.h"
#include "translations.h"
#include "resource.h"
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wininet.h>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <set>

// External globals (defined in globals.cpp) — duplicated here for modules
// that don't include globals.h transitively through every path.
extern HWND g_hwnd;
extern HWND g_statusBar;
extern HSTREAM g_fxStream;
extern std::vector<std::wstring> g_playlist;
extern int g_currentTrack;
extern float g_volume;
extern bool g_isLoading;
extern bool g_isBusy;
extern int g_selectedDevice;
extern bool g_allowAmplify;
extern bool g_rememberState;
extern int g_rememberPosMinutes;
extern bool g_bringToFront;
extern bool g_loadFolder;
extern const int g_posThresholds[];
extern const int g_posThresholdCount;
extern bool g_seekEnabled[];
extern int g_currentSeekIndex;
extern const SeekAmount g_seekAmounts[];
extern const int g_seekAmountCount;
extern const FileAssoc g_fileAssocs[];
extern const int g_fileAssocCount;
extern bool g_registerFileTypes;
extern const HotkeyAction g_hotkeyActions[];
extern const int g_hotkeyActionCount;
extern std::vector<GlobalHotkey> g_hotkeys;
extern int g_nextHotkeyId;
extern bool g_hotkeysEnabled;
extern std::wstring g_configPath;
extern bool g_effectEnabled[];
extern int g_currentEffectIndex;
extern int g_bufferSize;
extern int g_updatePeriod;
extern const int g_bufferSizes[];
extern const int g_bufferSizeCount;
extern const int g_updatePeriods[];
extern const int g_updatePeriodCount;
extern int g_tempoAlgorithm;
extern std::wstring g_ytdlpPath;
extern std::wstring g_ytApiKey;
extern bool g_isRecording;

// External functions (defined in other modules)
extern std::wstring GetFileName(const std::wstring& path);
extern std::wstring FormatTime(double seconds);
extern void SaveSettings();
extern void SaveHotkeys();
extern bool ReinitBass(int device);
extern void SetVolume(float vol);
extern void RegisterGlobalHotkeys();
extern void UnregisterGlobalHotkeys();
// PlayTrack is already declared in player.h (included above)
extern void SaveFilePosition(const std::wstring& filePath);
extern double LoadFilePosition(const std::wstring& filePath);
extern INT_PTR CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern std::wstring FormatHotkey(UINT modifiers, UINT vk);

// Constants shared across UI modules
extern const wchar_t* const APP_NAME_INTERNAL;

// Shared helpers (defined in ui.cpp, used by multiple UI modules)
bool IsSupportedAudioExt(const std::wstring& ext);
void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files);
std::wstring GetExePath();
