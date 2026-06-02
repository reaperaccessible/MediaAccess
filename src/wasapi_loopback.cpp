// =============================================================================
// wasapi_loopback.cpp — system-audio capture engine (Windows WASAPI loopback).
//
// v1.94. Implements the contract in include\mediaaccess\wasapi_loopback.h.
// This is a fully self-contained, parallel recording path. It does NOT touch
// the legacy "MediaAccess output" recording (player.cpp ToggleRecording /
// StopRecording / g_isRecording / g_encoder). IsSystemCapturing() is the sole
// source of truth for system-capture state.
//
// Chain (validated by spike\spike_bridge.cpp Test C):
//   BASS_WASAPI_Init(loopDevice, ..., WasapiProc)   // loopback = a device with
//                                                     // BASS_DEVICE_LOOPBACK
//   WasapiProc(buffer,len) -> BASS_StreamPutData(g_pushStream, buffer, len)
//   g_pushStream is a BASS_STREAM_DECODE push stream; a BASS encoder is attached
//   to it. A dedicated drain thread pulls via BASS_ChannelGetData so the encoder
//   is fed without any audible playback.
//
// COM: CoInitializeEx(MULTITHREADED) is done ONLY on the drain thread, never on
// the UI thread (BASS/BASSWASAPI calls used here are issued on the same drain
// thread to keep COM apartment consistency simple and safe).
// =============================================================================

#include <windows.h>
#include <avrt.h>          // AvSetMmThreadCharacteristics ("Pro Audio")
#include <atomic>
#include <thread>

#include "bass.h"
#include "bassenc.h"
#include "bassenc_mp3.h"
#include "bassenc_ogg.h"
#include "bassenc_flac.h"
#include "basswasapi.h"

#include "mediaaccess/wasapi_loopback.h"
#include "mediaaccess/globals.h"   // g_selectedDevice, g_selectedDeviceName

namespace mediaaccess {

// ----------------------------------------------------------------------------
// Internal state. All file-static; mutated only as described below.
// ----------------------------------------------------------------------------
static HSTREAM            g_pushStream = 0;    // decode push stream feeding the encoder
static HENCODE            g_systemEncoder = 0; // attached BASS encoder
static std::atomic<bool>  g_isSystemCapturing{false};
static std::atomic<bool>  g_drainRun{false};   // drives the drain-thread loop
static std::atomic<bool>  g_deviceLost{false}; // set if capture self-stops on device loss
static std::thread        g_drainThread;       // dedicated drain (pull) thread

// Start parameters handed to the drain thread (the thread owns the BASS/WASAPI
// init so COM lives entirely on that thread). Written by StartSystemCapture
// before the thread is launched; read by the thread only.
static int          s_startBwaIndex = -1;
static std::wstring s_startOutputPath;
static int          s_startFormat = 0;
static int          s_startBitrate = 192;

// Result handshake from the thread back to StartSystemCapture.
static std::atomic<int> s_startResult{0};      // 0 = pending, 1 = ok, -1 = failed
static std::wstring     s_capturedName;        // filled by the thread on success

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static std::wstring WasapiNameToWide(const char* name) {
    if (!name) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
    if (len <= 0) {
        // BASSWASAPI device names are UTF-8; fall back to ANSI just in case.
        len = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
        if (len <= 0) return L"";
        std::wstring w(len, 0);
        MultiByteToWideChar(CP_ACP, 0, name, -1, &w[0], len);
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    }
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, &w[0], len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// True if either name contains the other (case-insensitive). BASSWASAPI may
// suffix/decorate loopback names slightly relative to BASS's output names, so a
// substring match in either direction is the robust correlation.
static bool NameMatches(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    std::wstring la = ToLower(a), lb = ToLower(b);
    return la.find(lb) != std::wstring::npos || lb.find(la) != std::wstring::npos;
}

// Resolve the human-readable name of the output device BASS is currently using.
static std::wstring CurrentBassOutputName() {
    // g_selectedDeviceName is the persisted name; prefer it when set.
    if (!g_selectedDeviceName.empty()) return g_selectedDeviceName;
    // Otherwise resolve via the active BASS device index.
    int dev = BASS_GetDevice();
    BASS_DEVICEINFO info;
    if (dev != (DWORD)-1 && BASS_GetDeviceInfo(dev, &info) && info.name) {
        int len = MultiByteToWideChar(CP_UTF8, 0, info.name, -1, nullptr, 0);
        if (len > 0) {
            std::wstring w(len, 0);
            MultiByteToWideChar(CP_UTF8, 0, info.name, -1, &w[0], len);
            if (!w.empty() && w.back() == L'\0') w.pop_back();
            return w;
        }
    }
    return L"";
}

// ----------------------------------------------------------------------------
// Public: enumeration
// ----------------------------------------------------------------------------
std::vector<LoopbackDevice> EnumerateLoopbackDevices() {
    std::vector<LoopbackDevice> out;
    BASS_WASAPI_DEVICEINFO di;
    for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
        const bool loop    = (di.flags & BASS_DEVICE_LOOPBACK) != 0;
        const bool enabled = (di.flags & BASS_DEVICE_ENABLED)  != 0;
        if (loop && enabled) {
            LoopbackDevice d;
            d.bwaIndex  = i;
            d.name      = WasapiNameToWide(di.name);
            d.isDefault = (di.flags & BASS_DEVICE_DEFAULT) != 0;
            out.push_back(std::move(d));
        }
    }
    return out;
}

int FindLoopbackForCurrentBassDevice() {
    const std::wstring target = CurrentBassOutputName();
    if (target.empty()) return -1;

    BASS_WASAPI_DEVICEINFO di;
    for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
        const bool loop    = (di.flags & BASS_DEVICE_LOOPBACK) != 0;
        const bool enabled = (di.flags & BASS_DEVICE_ENABLED)  != 0;
        if (loop && enabled && NameMatches(WasapiNameToWide(di.name), target)) {
            return i;
        }
    }
    return -1;
}

// ----------------------------------------------------------------------------
// WASAPI capture callback. Loopback delivers float PCM; just hand it to the
// push stream. Returns 1 to continue (input-style device).
// ----------------------------------------------------------------------------
static DWORD CALLBACK LoopWasapiProc(void* buffer, DWORD length, void* user) {
    (void)user;
    if (g_pushStream && length) {
        BASS_StreamPutData(g_pushStream, buffer, length);
    }
    return 1; // continue capturing
}

// ----------------------------------------------------------------------------
// Encoder start — mirrors player.cpp ToggleRecording() exactly (flags, options,
// WAV fallback for MP3/OGG/FLAC). Operates on g_pushStream. On WAV fallback the
// output path's extension is rewritten to .wav. Returns true if an encoder is
// running (g_systemEncoder set).
// ----------------------------------------------------------------------------
static std::wstring ReplaceExtWav(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot   = path.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return path.substr(0, dot) + L".wav";
    return path + L".wav";
}

static bool StartEncoderOnPush(const std::wstring& outputPath, int format,
                               int bitrate, std::wstring& finalPath) {
    const DWORD wavFlags = BASS_ENCODE_AUTOFREE | BASS_ENCODE_FP_16BIT;
    finalPath = outputPath;

    auto fallbackToWav = [&]() {
        finalPath = ReplaceExtWav(outputPath);
        g_systemEncoder = BASS_Encode_StartPCMFile(g_pushStream, wavFlags, finalPath.c_str());
    };

    switch (format) {
        case 0: {
            g_systemEncoder = BASS_Encode_StartPCMFile(g_pushStream, wavFlags, finalPath.c_str());
            break;
        }
        case 1: {
            wchar_t options[64];
            swprintf(options, 64, L"--preset cbr %d", bitrate);
            g_systemEncoder = BASS_Encode_MP3_StartFile(g_pushStream, options,
                                                        BASS_ENCODE_AUTOFREE, finalPath.c_str());
            if (!g_systemEncoder) fallbackToWav();
            break;
        }
        case 2: {
            wchar_t options[64];
            swprintf(options, 64, L"--bitrate %d", bitrate);
            g_systemEncoder = BASS_Encode_OGG_StartFile(g_pushStream, options,
                                                        BASS_ENCODE_AUTOFREE, finalPath.c_str());
            if (!g_systemEncoder) fallbackToWav();
            break;
        }
        case 3: {
            g_systemEncoder = BASS_Encode_FLAC_StartFile(g_pushStream, nullptr,
                                                         wavFlags, finalPath.c_str());
            if (!g_systemEncoder) fallbackToWav();
            break;
        }
        default:
            g_systemEncoder = BASS_Encode_StartPCMFile(g_pushStream, wavFlags, finalPath.c_str());
            break;
    }
    return g_systemEncoder != 0;
}

// ----------------------------------------------------------------------------
// Full teardown of engine resources. Safe to call from the drain thread after
// the loop ends. Leaves all handles zeroed.
// ----------------------------------------------------------------------------
static void TeardownEngine() {
    BASS_WASAPI_Stop(TRUE);    // stop + reset the loopback capture
    if (g_systemEncoder) {
        BASS_Encode_Stop(g_systemEncoder);
        g_systemEncoder = 0;
    }
    BASS_WASAPI_Free();
    if (g_pushStream) {
        BASS_StreamFree(g_pushStream);
        g_pushStream = 0;
    }
}

// ----------------------------------------------------------------------------
// Drain thread: owns init, the pull loop, and teardown. COM lives here only.
// ----------------------------------------------------------------------------
static void DrainThreadMain() {
    HRESULT comHr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // Raise scheduling priority for glitch-free capture (best-effort).
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    bool ok = false;
    std::wstring capturedName;

    do {
        // --- Init WASAPI loopback on the chosen device. flags=0 (no separate
        // loopback flag exists; the device itself is a LOOPBACK device). ---
        if (!BASS_WASAPI_Init(s_startBwaIndex, 0, 0, 0, 0.5f, 0,
                              LoopWasapiProc, NULL)) {
            break;
        }

        BASS_WASAPI_INFO wi;
        if (!BASS_WASAPI_GetInfo(&wi)) {
            BASS_WASAPI_Free();
            break;
        }

        // Capture the real device name for announcement.
        BASS_WASAPI_DEVICEINFO di;
        if (BASS_WASAPI_GetDeviceInfo(s_startBwaIndex, &di)) {
            capturedName = WasapiNameToWide(di.name);
        }

        // --- Push stream matching the loopback format (WASAPI loopback = float). ---
        g_pushStream = BASS_StreamCreate(wi.freq, wi.chans,
                                         BASS_SAMPLE_FLOAT | BASS_STREAM_DECODE,
                                         STREAMPROC_PUSH, NULL);
        if (!g_pushStream) {
            BASS_WASAPI_Free();
            break;
        }

        // --- Attach encoder. ---
        std::wstring finalPath;
        if (!StartEncoderOnPush(s_startOutputPath, s_startFormat, s_startBitrate, finalPath)) {
            BASS_StreamFree(g_pushStream);
            g_pushStream = 0;
            BASS_WASAPI_Free();
            break;
        }

        // --- Start the capture. ---
        if (!BASS_WASAPI_Start()) {
            BASS_Encode_Stop(g_systemEncoder);
            g_systemEncoder = 0;
            BASS_StreamFree(g_pushStream);
            g_pushStream = 0;
            BASS_WASAPI_Free();
            break;
        }

        ok = true;
    } while (false);

    // Hand the result back to StartSystemCapture.
    if (ok) {
        s_capturedName = capturedName;
        g_isSystemCapturing.store(true);
        s_startResult.store(1);
    } else {
        s_startResult.store(-1);
        // Cleanup of any partial COM/MMCSS happens below.
    }

    // --- Pull loop: drain the decode stream so the encoder is fed. ---
    bool deviceLost = false;
    if (ok) {
        char drain[65536];
        // Device-loss watchdog. If the WASAPI device is invalidated mid-capture
        // (unplugged / disabled / format change), BASS_WASAPI_IsStarted() stops
        // reporting BASS_ACTIVE_PLAYING. We sample it periodically (cheap) and,
        // after a short run of consecutive "not started" readings, conclude the
        // device is gone, stop cleanly, and flag it for the UI. The grace count
        // avoids false positives during transient stalls.
        int notStartedStreak = 0;
        const int kLostThreshold = 40;   // ~ 40 * 5ms = 200ms of no playback
        while (g_drainRun.load()) {
            DWORD got = BASS_ChannelGetData(g_pushStream, drain, sizeof(drain));

            // Watchdog sample (only meaningful while we still think we run).
            if (BASS_WASAPI_IsStarted() != BASS_ACTIVE_PLAYING) {
                if (++notStartedStreak >= kLostThreshold) {
                    deviceLost = true;
                    break;   // self-stop: finalize file below via TeardownEngine
                }
            } else {
                notStartedStreak = 0;
            }

            if (got == (DWORD)-1) {
                // Nothing buffered yet — brief wait, then retry.
                Sleep(5);
            } else if (got < sizeof(drain)) {
                // Drained what was available; let more accumulate.
                Sleep(5);
            }
            // else: a full buffer came back — loop immediately to keep up.
        }
        // Final flush so trailing buffered audio reaches the encoder.
        DWORD got;
        do {
            got = BASS_ChannelGetData(g_pushStream, drain, sizeof(drain));
        } while (got != (DWORD)-1 && got == sizeof(drain));

        TeardownEngine();
        g_isSystemCapturing.store(false);
        if (deviceLost) g_deviceLost.store(true);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(comHr)) CoUninitialize();
}

// ----------------------------------------------------------------------------
// Public: start / stop / query
// ----------------------------------------------------------------------------
bool StartSystemCapture(int bwaIndex, const std::wstring& outputPath,
                        int format, int bitrate,
                        std::wstring& capturedDeviceName) {
    // If a previous capture self-stopped (device lost) the thread has exited but
    // is still joinable; reap it before starting a fresh one. A genuinely active
    // capture (g_isSystemCapturing still true) is left alone and rejected below.
    if (!g_isSystemCapturing.load() && g_drainThread.joinable()) {
        g_drainRun.store(false);
        g_drainThread.join();
    }
    if (g_isSystemCapturing.load() || g_drainThread.joinable()) {
        return false; // already capturing
    }
    if (bwaIndex < 0) return false;

    g_deviceLost.store(false);   // fresh start clears any stale loss flag

    // Stage parameters for the drain thread.
    s_startBwaIndex   = bwaIndex;
    s_startOutputPath = outputPath;
    s_startFormat     = format;
    s_startBitrate    = bitrate;
    s_startResult.store(0);
    s_capturedName.clear();

    g_drainRun.store(true);
    g_drainThread = std::thread(DrainThreadMain);

    // Wait (bounded) for the thread to report init success/failure.
    for (int i = 0; i < 1000 && s_startResult.load() == 0; ++i) {
        Sleep(2);
    }

    if (s_startResult.load() != 1) {
        // Init failed (or timed out). Tear the thread down cleanly.
        g_drainRun.store(false);
        if (g_drainThread.joinable()) g_drainThread.join();
        return false;
    }

    capturedDeviceName = s_capturedName;
    return true;
}

void StopSystemCapture() {
    if (!g_drainThread.joinable()) {
        // Not running (or already stopped). Ensure flags are sane.
        g_isSystemCapturing.store(false);
        return;
    }
    g_drainRun.store(false);
    g_drainThread.join();      // thread performs final flush + TeardownEngine
    g_isSystemCapturing.store(false);
}

bool IsSystemCapturing() {
    return g_isSystemCapturing.load();
}

bool ConsumeSystemCaptureLost() {
    return g_deviceLost.exchange(false);
}

} // namespace mediaaccess
