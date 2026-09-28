#pragma once
#ifndef MEDIAACCESS_YTDLP_UPDATER_H
#define MEDIAACCESS_YTDLP_UPDATER_H

// v2.72 — yt-dlp nightly auto-updater (see docs/PLAN_YTDLP_NIGHTLY.md).
//
// The updater keeps %LOCALAPPDATA%\MediaAccess\yt-dlp.exe on the latest
// nightly. A new build is installed only after its size, its SHA-256 (from the
// release's SHA2-256SUMS) and its own --version output have all been checked.
// While an update runs, every YouTube action waits for it (YtdlpWaitIfUpdating).

#include <windows.h>
#include <string>

// Posted to the main window when an update run ends (wParam = 1 when the
// yt-dlp path changed and mpv's ytdl_path must be refreshed).
#define WM_YTDLP_UPDATE_DONE (WM_USER + 24)

// Timer on the main window: hourly tick that rechecks once 12 h have passed.
#define IDT_YTDLP_RECHECK  411
// Timer on the main window: 2-minute cap on a deferred exit.
#define IDT_YTDLP_EXIT_CAP 412

// Call once from wWinMain on the UI thread, before LoadSettings().
void YtdlpInit();

// Called by LoadSettings() before it resolves the yt-dlp path: repairs a
// half-done swap, installs a verified pending build, and seeds the
// %LOCALAPPDATA% copy from the bundled lib\yt-dlp.exe so the path never changes.
void YtdlpPrepareAtStartup(const std::wstring& appDir);

// Thread-safe access to the yt-dlp path (g_ytdlpPath is guarded by a lock).
std::wstring GetYtdlpPath();
void SetYtdlpPath(const std::wstring& path);

// Start a check now (no-op when one is already running).
void LaunchYtdlpUpdateCheck();
// Hourly timer / resume from sleep: start a check if 12 h have passed since
// the last successful one (force = true starts one regardless).
void YtdlpMaybeRecheck(bool force);

// True while a check, download, verification or install is running.
bool YtdlpUpdateBusy();
// True while a download, verification or install is running (not a mere check).
bool YtdlpInstallRunning();
// True once YtdlpAbortAndWait() was called for the current run.
bool YtdlpAbortRequested();

// Gate before any user-driven YouTube action. On the UI thread: waits (silent
// during the API check, then the mandatory wait window) until the update ends
// or 90 s pass. Returns false only when a wait is already on screen (the new
// action is dropped). On another thread: waits silently.
bool YtdlpWaitIfUpdating(HWND owner = nullptr);

// True while YtdlpWaitIfUpdating is waiting on the UI thread. A new YouTube
// action (or PlayTrack) arriving meanwhile must be ignored, not treated as a
// failure.
bool YtdlpWaitShown();
// An exit was requested while a wait is on the stack: end the wait; the gate
// then drops its action and posts WM_CLOSE once its loop has unwound.
void YtdlpEndWaitForExit();

// Background safety net (RunYtdlp): waits silently when called off the UI
// thread and an update is running, unless the UI gate already gave up.
void YtdlpWaitSilentlyIfUpdating();

// Stop a running update and wait up to |ms| for the worker to finish. The
// file swap itself is never interrupted; an unverified download is deleted.
void YtdlpAbortAndWait(DWORD ms);

#endif // MEDIAACCESS_YTDLP_UPDATER_H
