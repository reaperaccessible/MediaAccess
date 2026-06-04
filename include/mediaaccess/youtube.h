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
// Returns results, nextPageToken is set if more results available.
//
// seenIdsSnapshot (v1.98, M4 thread-safety): for the yt-dlp paginated fallback,
// the "load more" path must deduplicate new results against the ids already
// shown to the user. Historically that dedup read the live g_ytResults vector,
// which the UI thread can clear/modify concurrently (use-after-free / data
// race). Pass a SNAPSHOT of the already-seen video ids here and the dedup uses
// it instead of the live vector. Pass nullptr (the default) for the synchronous
// UI-thread callers (first-page search, playlist load) where no background
// thread is involved.
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken = L"",
                   const std::vector<std::wstring>* seenIdsSnapshot = nullptr);

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
    std::wstring language;     // ISO 639-1 code from JSON "language" ("fr", "en-US"),
                               // empty when YouTube exposes no per-track language
                               // (the common single-language case). Used to label
                               // audio tracks on genuine multi-language videos.
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

// Download SEVERAL streams of one video and mux them into a single file (v2.10,
// redlaf's request: e.g. one video track + two audio tracks for a multilingual
// video). The formatIds are each validated against the same strict whitelist as
// YouTubeDownloadFormat; any invalid id makes the function return false. The ids
// are joined with '+' into one yt-dlp selector and downloaded with
// --audio-multistreams / --video-multistreams so yt-dlp keeps EVERY selected
// stream instead of collapsing to one of each kind. The output container is
// Matroska (.mkv) because it carries any number of audio tracks and any codec
// combination (e.g. Opus audio that mp4 cannot hold). On success outFilePath
// holds the absolute path of the produced file. Use YouTubeDownloadFormat for
// the single-stream case (it keeps the smart video-only +bestaudio fallback).
bool YouTubeDownloadMultiFormat(const std::wstring& videoId,
                                const std::wstring& title,
                                const std::vector<std::wstring>& formatIds,
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

// ============================================================
// Async UI message handlers (v1.98) — called from the MAIN window proc
// ============================================================
//
// Every YouTube network operation now runs on a worker thread and posts its
// result to the always-alive main window (g_hwnd). The main wndproc forwards
// the lParam straight to these handlers. Each handler:
//   * takes ownership of the heap payload pointed to by lParam and frees it,
//   * is robust if the modeless YouTube dialog was closed mid-fetch (it checks
//     GetYouTubeDialog() and silently drops the UI update when null),
//   * never dereferences a dead HWND.
// Routing through the main window (rather than the dialog HWND) guarantees the
// heap payload is always freed even when the dialog closed before the worker
// finished — a PostMessage to a destroyed HWND is silently discarded by
// Windows, which would otherwise leak the payload.
void YouTubeOnLoadMoreDone(LPARAM lParam);   // WM_YT_LOAD_MORE_DONE
void YouTubeOnFormatsReady(LPARAM lParam);   // WM_YT_FORMATS_READY
void YouTubeOnDownloadDone(LPARAM lParam);   // WM_YT_DOWNLOAD_DONE
void YouTubeOnSearchDone(LPARAM lParam);     // WM_YT_SEARCH_DONE (v1.99)
void YouTubeOnBatchProgress(WPARAM wParam, LPARAM lParam); // WM_YT_BATCH_PROGRESS (v2.12)
void YouTubeOnBatchDone(LPARAM lParam);      // WM_YT_BATCH_DONE (v2.12)

// Cancel any pending hybrid swap. Call before loading non-YouTube media so a
// late-arriving download from a previously-started hybrid playback does not
// clobber the new track the user just opened.
void YouTubeCancelHybrid();

// ============================================================
// yt-dlp process runner (foundation, v1.97)
// ============================================================
//
// Distinct timeouts for the two classes of yt-dlp invocation:
//   * Queries (search, format list, get-url) are expected to finish in a
//     few seconds; a 30 s ceiling protects the UI from a hung process.
//   * Downloads can legitimately run for minutes (large video, slow link),
//     so they MUST NOT be capped — a timeout here would kill a perfectly
//     healthy download mid-transfer and leave a partial file.
constexpr int YTDLP_QUERY_TIMEOUT_MS    = 30000;  // 30 s — interactive queries
constexpr int YTDLP_DOWNLOAD_TIMEOUT_MS = 0;      // 0 = unlimited — EXPLICIT user downloads
// A (v2.00): bounded timeout for the PLAYBACK download path (hybrid cache fill,
// cache-refresh, last-resort blocking download). Unlike an explicit "download
// with options" transfer — which the user deliberately started and may
// legitimately let run for minutes — a playback download exists only to feed
// BASS for instant listening. A YouTube LIVE stream never terminates, so an
// unlimited timeout there leaks the HybridDownloadThread forever and (on the
// last-resort path) freezes the calling thread. 60 s is comfortably longer than
// any normal audio fetch on a working link, but guarantees a live/pathological
// download is killed and reported instead of hanging.
constexpr int YTDLP_PLAYBACK_TIMEOUT_MS = 60000;  // 60 s — playback (cache/hybrid)

// Run yt-dlp with the given argument string and capture its stdout (UTF-8 →
// wide). stderr is captured separately so the returned stdout stays clean
// JSON/URL text. On a timeout (> 0) the child process is terminated and every
// handle is closed — no orphan yt-dlp/ffmpeg processes are left behind.
//
//   timeoutMs  YTDLP_QUERY_TIMEOUT_MS for queries, YTDLP_DOWNLOAD_TIMEOUT_MS
//              (0 = wait forever) for downloads.
//   exitCode   optional out-param; receives yt-dlp's process exit code (0 on
//              success). Set to a negative sentinel when the process timed out
//              or could not be launched. Pass nullptr if not needed.
//   stderrOut  optional out-param; receives the captured stderr text so the
//              caller can craft a specific error message. Pass nullptr to
//              discard stderr.
//
// Returns the captured stdout, or an empty string when yt-dlp is unavailable
// or the process could not be created. Thread-safe: holds no shared state.
std::wstring RunYtdlp(const std::wstring& args,
                      int timeoutMs = YTDLP_QUERY_TIMEOUT_MS,
                      int* exitCode = nullptr,
                      std::wstring* stderrOut = nullptr);

// Process exit-code sentinels used when no real exit code is available.
constexpr int YTDLP_EXIT_LAUNCH_FAILED = -1;  // CreateProcess / pipe failed
constexpr int YTDLP_EXIT_TIMED_OUT     = -2;  // killed after timeoutMs elapsed

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
