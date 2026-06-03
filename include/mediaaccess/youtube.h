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

// ============================================================
// Format-aware download engine (v1.94+)
// ============================================================
//
// These functions power the "Download with format choice" feature: the user
// gets a dynamic list of every stream YouTube offers for a video (resolutions,
// codecs, audio-only, video-only, sizes) and picks one. The engine then
// downloads exactly that format_id, merging audio+video with ffmpeg when the
// chosen stream is video-only.
//
// One parsed YouTube stream/format, as returned by ParseFormatsArray().
struct YtFormat {
    std::wstring formatId;     // yt-dlp format_id, e.g. "137" or "251"
    std::wstring ext;          // container extension, e.g. "mp4", "webm", "m4a"
    std::wstring resolution;   // human resolution: "1080p", "audio only", or format_note
    std::wstring vcodec;       // video codec, "none" for audio-only streams
    std::wstring acodec;       // audio codec, "none" for video-only streams
    std::wstring sizeStr;      // human-readable size: "12.3 MB" or "" if unknown
    std::wstring note;         // yt-dlp format_note (e.g. "720p60", "DRC")
    int height = 0;            // pixel height (0 = audio-only / unknown), used for sorting
    long long filesize = 0;    // size in bytes (0 = unknown)
};

// Resolve the ffmpeg.exe to use for merging/remuxing. Resolution order:
//   1. INI [YouTube] FfmpegPath, if set and the file exists.
//   2. <app>\lib\ffmpeg.exe (bundled, same layout as yt-dlp).
//   3. ffmpeg.exe found on the system PATH.
//   4. Empty string -> caller omits --ffmpeg-location.
std::wstring GetFfmpegLocation();

// Query yt-dlp for every available format of a video and parse them into a
// sorted vector (best first: height desc, then bitrate/size). Returns an empty
// vector if the video has no formats or yt-dlp output couldn't be parsed; the
// caller should then show "no formats available". The videoId is sanitized
// internally before being passed to yt-dlp.
std::vector<YtFormat> ParseFormatsArray(const std::wstring& videoId);

// Download a specific format_id to the user's configured Downloads folder
// (same destination + filename rules as YouTubeDownloadPermanent). The
// formatId is validated against a strict whitelist before use; an invalid id
// makes the function return false so the caller can fall back. On success
// outFilePath holds the absolute path of the produced file.
//
// When videoOnly is true the chosen format carries no audio (acodec == "none"),
// so the selector is expanded to "<id>+bestaudio[ext=m4a]/<id>+bestaudio/<id>"
// to pull and merge the best available audio track. For audio-only or combined
// formats pass videoOnly = false and the raw "<id>" selector is used.
bool YouTubeDownloadFormat(const std::wstring& videoId,
                           const std::wstring& title,
                           const std::wstring& formatId,
                           bool videoOnly,
                           std::wstring& outFilePath);

// Wipe every file from the YouTube audio cache. Returns the count removed.
int ClearYouTubeCache();

// Total bytes currently used by the YouTube audio cache.
unsigned long long GetYouTubeCacheSize();

// Enforce a cache size cap by deleting oldest files first until the total
// is below limitMB. Pass 0 or negative to disable. Returns count removed.
int EnforceYouTubeCacheLimit(int limitMB);

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

// Cancel any pending hybrid swap. Call before loading non-YouTube media so a
// late-arriving download from a previously-started hybrid playback does not
// clobber the new track the user just opened.
void YouTubeCancelHybrid();

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
