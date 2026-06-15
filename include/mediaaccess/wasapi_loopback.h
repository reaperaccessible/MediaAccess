// =============================================================================
// wasapi_loopback.h — system-audio capture engine (Windows WASAPI loopback).
//
// v1.94. This is the SEPARATE, parallel recording path. It captures whatever
// Windows is playing through an output device (loopback) and encodes it to a
// file, independently of MediaAccess's own playback/recording pipeline. The
// legacy "MediaAccess output" recording in player.cpp is untouched.
//
// Engine design (validated by spike\spike_bridge.cpp, Phase 0 Test C):
//   WASAPI loopback device  --WASAPIPROC-->  BASS_StreamPutData(pushStream)
//   pushStream (BASS_STREAM_DECODE)  --BASS_ChannelGetData (drain thread)-->
//   BASS encoder  -->  output file
//
// Loopback is selected by choosing a WASAPI output device that carries the
// BASS_DEVICE_LOOPBACK flag; there is no separate "loopback" init flag.
// =============================================================================
#pragma once
#ifndef MEDIAACCESS_WASAPI_LOOPBACK_H
#define MEDIAACCESS_WASAPI_LOOPBACK_H

#include <string>
#include <vector>

namespace mediaaccess {

struct LoopbackDevice {
    int          bwaIndex;   // index passed to BASSWASAPI (BASS_WASAPI_* device arg)
    std::wstring name;       // human-readable device name
    bool         isDefault;  // is this the Windows default output device?
};

// Enumerate the available loopback devices (output devices that expose the
// BASS_DEVICE_LOOPBACK flag and are enabled). Empty if none / no WASAPI.
std::vector<LoopbackDevice> EnumerateLoopbackDevices();

// Find the BASSWASAPI loopback index whose underlying output endpoint matches
// the output device MediaAccess (BASS) is currently using. Returns -1 if no
// match is found — the caller should then fall back to the default loopback
// device and warn the user.
int FindLoopbackForCurrentBassDevice();

// v2.33 — lowercased GUID-tail identity of the current Windows default RENDER
// endpoint (resolved via eConsole, then eMultimedia, then a no-COM BASSWASAPI
// fallback). Returns "" if no device / unresolvable. UI-thread only. Used by
// HandleAudioDeviceChange to detect whether the default endpoint TRULY changed,
// so spurious endpoint notifications don't trigger a reroute/announcement.
std::string CurrentDefaultRenderEndpointTail();

// Start loopback capture of device `bwaIndex`, encoding to `outputPath`.
//   format : 0=WAV, 1=MP3, 2=OGG, 3=FLAC (same numbering as g_recordFormat).
//   bitrate: kbps for MP3/OGG (ignored for WAV/FLAC).
// On success returns true and fills `capturedDeviceName` with the real name of
// the device being captured (for NVDA announcement). On any failure returns
// false, releases all engine resources, and never throws/crashes. Lossy/lossless
// encoder start failures fall back to WAV (with a .wav output path), mirroring
// the legacy recording behavior. Safe to call while BASS playback is running.
bool StartSystemCapture(int bwaIndex, const std::wstring& outputPath,
                        int format, int bitrate,
                        std::wstring& capturedDeviceName);

// Stop the capture and finalize the file. No-op if not capturing.
void StopSystemCapture();

// True while a system capture is in progress.
bool IsSystemCapturing();

// v2.24 — pause/resume the ENCODER of an in-progress system capture WITHOUT
// stopping WASAPI (the device-loss watchdog stays satisfied; the file simply
// skips the paused span). Returns the resulting paused state. No-op returning
// false if not capturing. Safe to call from the UI thread.
bool ToggleSystemCapturePaused();
bool IsSystemCapturePaused();

// One-shot edge flag: returns true exactly once if the capture self-stopped
// because the audio device was lost/invalidated (e.g. unplugged or disabled
// mid-recording). The caller (UI timer) polls this to announce the event and
// refresh UI. Resets to false on read. A user-initiated StopSystemCapture()
// does NOT set this flag.
bool ConsumeSystemCaptureLost();

// v2.12 — write a verbatim dump of the BASS and BASSWASAPI device tables to a
// text file (in the Music folder) for diagnosing Automatic loopback detection
// on a tester's machine. Read-only enumeration; never inits or captures.
// Returns the full path written, or L"" on failure.
std::wstring WriteAudioDiagnostic();

} // namespace mediaaccess

#endif // MEDIAACCESS_WASAPI_LOOPBACK_H
