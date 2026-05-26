#pragma once
#ifndef MEDIAACCESS_YOUTUBE_H
#define MEDIAACCESS_YOUTUBE_H

#include <windows.h>
#include <string>
#include <vector>

// YouTube search result
struct YouTubeResult {
    std::wstring videoId;
    std::wstring title;
    std::wstring channel;
    std::wstring duration;    // Human-readable duration
    std::wstring uploadDate;  // Human-readable upload date
    bool isPlaylist = false;
    bool isChannel = false;
};

// Search YouTube using API or yt-dlp fallback
// Returns results, nextPageToken is set if more results available
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken = L"");

// Get contents of a playlist or channel
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken = L"");

// Get audio stream URL for a video using yt-dlp
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl);

// Get raw YouTube URL for video playback (libmpv handles yt-dlp internally)
bool YouTubeGetVideoURL(const std::wstring& videoId, std::wstring& url);

// Video mode toggle (when true, YouTube results play via libmpv with video)
void SetYouTubeVideoMode(bool mode);
bool GetYouTubeVideoMode();

// Download (or fetch from cache) YouTube audio to a permanent location
// under %LOCALAPPDATA%\MediaAccess\YouTubeCache. Subsequent plays of the
// same videoId are instant.
bool YouTubeDownloadAudio(const std::wstring& videoId, std::wstring& filePath);

// Returns true if the video is already in the persistent cache.
bool YouTubeIsAudioCached(const std::wstring& videoId);

// Permanently download a video to the user's Downloads folder
// (Downloads\MediaAccess\YouTube\<title>.m4a). The title is used as the
// filename so users can browse the folder. Returns the absolute path on
// success.
bool YouTubeDownloadPermanent(const std::wstring& videoId,
                              const std::wstring& title,
                              std::wstring& outFilePath);

// Wipe every file from the YouTube audio cache. Returns the count removed.
int ClearYouTubeCache();

// Total bytes currently used by the YouTube audio cache.
unsigned long long GetYouTubeCacheSize();

// Removes the legacy %TEMP%\MediaAccess directory (pre-1.0.8). Kept for
// backwards compatibility / housekeeping at startup and shutdown.
void CleanupYouTubeTempFiles();

// Unified YouTube playback entry point. Hybrid strategy:
//   * cached       -> instant BASS playback, full effects
//   * uncached+mpv -> libmpv streams immediately + background download
//                     swaps to BASS at the same position once ready
//   * fallback     -> blocking download, then BASS
bool YouTubePlayById(const std::wstring& videoId);

// Called from WM_YT_HYBRID_READY: the background download has finished.
// Swap from the streaming libmpv engine to BASS at the current position.
void YouTubeOnHybridDownloadReady(const std::wstring& videoId);

// Start streaming - downloads and returns path when complete (blocking)
bool YouTubeStartStream(const std::wstring& videoId, std::wstring& filePath);

// Async download functions
bool YouTubeStartDownload(const std::wstring& videoId);  // Start download, returns immediately
bool YouTubeIsDownloadComplete();                         // Check if download finished
bool YouTubeGetDownloadResult(std::wstring& filePath);   // Get result after completion

// Async search functions
bool YouTubeStartSearch(const std::wstring& query, bool isPlaylist, const std::wstring& playlistId,
                        const std::wstring& pageToken, bool isLoadMore);  // Start search, returns immediately
bool YouTubeIsSearchComplete();                                            // Check if search finished
bool YouTubeGetSearchResult(std::vector<YouTubeResult>& results, std::wstring& nextPageToken);  // Get results
bool YouTubeWasLoadMore();                                                 // Check if last search was "load more"

// Clean up temp files (call on startup and exit)
void YouTubeCleanup();

// Check if input looks like a YouTube URL
bool IsYouTubeURL(const std::wstring& input);

// Parse YouTube URL to extract video/playlist/channel ID
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel);

// Show YouTube search dialog (modeless)
void ShowYouTubeDialog(HWND parent);

// Get YouTube dialog handle (for message loop)
HWND GetYouTubeDialog();

// YouTube dialog procedure
INT_PTR CALLBACK YouTubeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#endif // MEDIAACCESS_YOUTUBE_H
