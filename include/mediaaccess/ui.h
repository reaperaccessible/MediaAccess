#pragma once
#ifndef MEDIAACCESS_UI_H
#define MEDIAACCESS_UI_H

#include <windows.h>
#include <string>
#include <vector>
#include "database.h"

// Playlist file handling
bool IsPlaylistFile(const std::wstring& path);
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath);

// Status bar
void CreateStatusBar(HWND hwnd, HINSTANCE hInstance);
void UpdateStatusBar();
void UpdateWindowTitle();

// v1.60 — Now-playing display infrastructure.
//
// The window title and the screen-reader announcement compose a two-level
// label "<source> - <item>" using these globals:
//   - source = container name (radio station, podcast, channel, book...)
//   - item   = currently-playing piece (song, episode, video, chapter...)
//
// Each playback entry-point (PlayTrack for local files, the radio dialog,
// the podcast dialog, the YouTube engine, the DAISY player, etc.) is
// responsible for calling SetNowPlaying() at start, and SetNowPlayingItem()
// whenever the item changes (new ICY song, next chapter, etc.). Stop() and
// the unload paths call ClearNowPlaying().
//
// The helpers also call UpdateWindowTitle() so callers never have to
// remember to refresh the title.
enum class SourceType {
    None,
    Local,           // a file on disk — source label is localized "(Local)"
    RadioFavorite,   // station from the favorites list
    RadioUrl,        // ad-hoc URL the user pasted
    Podcast,         // podcast episode
    YouTube,         // YouTube video (audio or video)
    Book,            // DAISY / EPUB
    Video            // local video file via libmpv (not YouTube)
};

void SetNowPlaying(SourceType type,
                   const std::wstring& source,
                   const std::wstring& item);
void SetNowPlayingItem(const std::wstring& item);
void ClearNowPlaying();

// Build the speech-friendly "<source> - <item>" string used by both the
// Speak("now playing") action and the WM_ACTIVATEAPP auto-announce.
// Returns an empty string if nothing is currently loaded.
std::wstring BuildNowPlayingSpeech();

// File associations
void RegisterAllFileTypes();
void UnregisterAllFileTypes();

// Dialogs
void ShowOpenDialog();
void ShowAddFolderDialog();
void ShowPlaylistDialog();
void ShowOpenURLDialog();
void ShowTestYouTubePlayback();  // Help menu diagnostic — verifies yt-dlp is wired correctly
void ShowJumpToTimeDialog();
void ShowEffectPresetsMenu(HWND hwnd);
void ShowSaveEffectPresetDialog();
void ShowOptionsDialog();
void AuditOptionsLayout();  // Help menu — finds truncated controls in current language
void ShowBookmarksDialog();
void ShowSongHistoryDialog();
void ShowRadioDialog();
void AddCurrentStreamToFavorites();
void ShowSchedulerDialog();
void ShowPodcastDialog();
void CheckScheduledEvents();
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat);
void HandleScheduledDurationEnd();
INT_PTR CALLBACK OptionsDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ShowTabControls(HWND hwnd, int tab);
void ShowTagDialog(const wchar_t* title, const std::wstring& text);
void NotifyPlaylistTrackChanged();

#endif // MEDIAACCESS_UI_H
