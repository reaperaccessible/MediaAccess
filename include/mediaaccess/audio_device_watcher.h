// =============================================================================
// audio_device_watcher.h — v2.32. Watch for Windows audio-endpoint changes and
// notify the main window so playback can be re-routed automatically when the
// default output device switches (headphones unplugged, device disabled, etc.).
//
// GitHub issue #7. The watcher uses Core Audio's IMMNotificationClient; the
// callbacks arrive on a COM worker thread and do nothing but PostMessageW the
// custom message below to the UI thread, which performs the actual reroute via
// HandleAudioDeviceChange() (implemented in player.cpp). No COM types leak into
// this header.
// =============================================================================
#pragma once
#ifndef MEDIAACCESS_AUDIO_DEVICE_WATCHER_H
#define MEDIAACCESS_AUDIO_DEVICE_WATCHER_H

#include <windows.h>

// Posted to the main window when a relevant audio-endpoint change is detected.
// The handler coalesces the burst of notifications via a short timer before
// performing the reroute.
#define WM_AUDIO_DEVICE_CHANGED (WM_USER + 130)

// Start/stop the endpoint-change watcher. StartAudioDeviceWatch must be called
// after the main window exists (it stores no HWND itself; the callbacks post to
// the global g_hwnd). Returns true on success.
bool StartAudioDeviceWatch(HWND hwnd);
void StopAudioDeviceWatch();

// UI-thread reroute entry point. Implemented in player.cpp. Invoked from the
// coalescing timer once the notification burst has settled.
void HandleAudioDeviceChange();

#endif // MEDIAACCESS_AUDIO_DEVICE_WATCHER_H
