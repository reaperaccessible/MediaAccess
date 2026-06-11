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
#include <shlobj.h>        // SHGetFolderPathW (diagnostic file location)
#include <mmdeviceapi.h>   // IMMDeviceEnumerator — resolve true Windows default endpoint
#include <atomic>
#include <thread>
#include <cstring>         // strcmp/_stricmp (endpoint-id correlation)
#include <cctype>          // tolower (GUID-tail comparison)
#include <cstdio>          // snprintf for the diagnostic dump
#include <fstream>         // diagnostic file writer
#include <string>
#include "mediaaccess/version.h"  // APP_VERSION (diagnostic header)

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
static std::atomic<bool>  g_systemPaused{false};   // v2.24 — encoder paused (punch-in/out)
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

// v2.12 — extract the trailing GUID portion of a WASAPI endpoint id, lowercased.
// Endpoint ids look like "{0.0.0.00000000}.{guid}" for render endpoints and
// "{0.0.1.00000000}.{guid}" for capture endpoints. The {guid} after the last
// '{' uniquely identifies the physical endpoint; the leading "{0.0.x.0...}."
// only encodes the data-flow role (render vs capture). Comparing on this tail
// lets us correlate a render endpoint to its loopback twin even if BASS and
// BASSWASAPI report the role prefix (or letter case) differently.
static std::string EndpointGuidTailLower(const char* id) {
    if (!id) return "";
    std::string s(id);
    size_t pos = s.find_last_of('{');
    std::string tail = (pos == std::string::npos) ? s : s.substr(pos);
    for (auto& c : tail) c = (char)tolower((unsigned char)c);
    return tail;
}

// v2.12 — exact/case-insensitive endpoint-id match (the documented twin). Used
// as tiers 1+2; tier 3 (GUID-tail) is handled separately in the caller with an
// ambiguity guard so it can never pick the wrong device. Reported by Sèb.
static bool EndpointIdExactOrCase(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    if (strcmp(a, b) == 0)   return true;  // tier 1: exact
    if (_stricmp(a, b) == 0)  return true; // tier 2: case-insensitive
    return false;
}

// v2.14 — narrow (UTF-8) copy of a wide string. Local to keep the helpers below
// independent of declaration order (Utf8FromWide lives further down).
static std::string WideToNarrowUtf8(const wchar_t* w) {
    if (!w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// v2.14 — return the index of the SINGLE enabled loopback whose endpoint GUID
// tail matches `tail`, or -1 if none / more than one (ambiguity guard preserves
// the "never the wrong device" invariant).
static int FindUniqueEnabledLoopbackByTail(const std::string& tail) {
    if (tail.empty()) return -1;
    BASS_WASAPI_DEVICEINFO di;
    int found = -1, count = 0;
    for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
        const DWORD f = di.flags;
        if ((f & BASS_DEVICE_LOOPBACK) && (f & BASS_DEVICE_ENABLED) &&
            di.id && di.id[0] && EndpointGuidTailLower(di.id) == tail) {
            ++count;
            found = i;
        }
    }
    return (count == 1) ? found : -1;
}

// v2.14 — Sèb's machine exposed the root cause: BASS was bound to its pseudo
// "Default" device (index 1, driver=""), so there was NO endpoint id to
// correlate, and the name "Default" matched no loopback -> the recorder could
// not identify the device. Ask Windows itself for the real default RENDER
// endpoint id (the FxSound endpoint, in his case) via Core Audio, then match its
// unique enabled loopback by GUID tail. Returns the GUID tail (lowercased) or ""
// on any failure. `role` is eConsole (what the user hears) or eMultimedia.
static std::string CoreAudioDefaultRenderTail(ERole role) {
    std::string tail;
    // The recorder toggle runs on the UI thread (STA). MMDeviceEnumerator is a
    // free-threaded object, so it works from STA. Init defensively: tolerate
    // "already initialised" and a different apartment model; only uninit if WE
    // initialised it.
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool weInit = (co == S_OK || co == S_FALSE);

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) && en) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, role, &dev)) && dev) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id)) && id) {
                tail = EndpointGuidTailLower(WideToNarrowUtf8(id).c_str());
                CoTaskMemFree(id);
            }
            dev->Release();
        }
        en->Release();
    }
    if (weInit) CoUninitialize();
    return tail;
}

// v2.14 — no-COM fallback: the BASSWASAPI RENDER endpoint flagged DEFAULT. Only
// accept it when exactly one render endpoint is the default (else "" -> ambiguous).
static std::string BasswasapiDefaultRenderTail() {
    BASS_WASAPI_DEVICEINFO di;
    std::string tail;
    int count = 0;
    for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
        const DWORD f = di.flags;
        if (!(f & BASS_DEVICE_LOOPBACK) && !(f & BASS_DEVICE_INPUT) &&
            (f & BASS_DEVICE_DEFAULT) && (f & BASS_DEVICE_ENABLED) &&
            di.id && di.id[0]) {
            ++count;
            tail = EndpointGuidTailLower(di.id);
        }
    }
    return (count == 1) ? tail : "";
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
    // Resolve the BASS output device we're bound to.
    int dev = BASS_GetDevice();
    BASS_DEVICEINFO bi{};
    const bool haveBi = (dev != static_cast<int>(static_cast<DWORD>(-1)) &&
                         BASS_GetDeviceInfo(static_cast<DWORD>(dev), &bi));

    // Are we tracking the Windows DEFAULT output? v2.14 — Sèb's diagnostic showed
    // this is NOT just the (g_selectedDevice == -1) case: BASS resolves the
    // default to its pseudo "Default" device (index 1, name "Default") whose
    // BASS_DEVICEINFO.driver is EMPTY — so there is no endpoint id to correlate
    // and the name "Default" matches no loopback. The robust, self-validating
    // signal is an EMPTY driver on the resolved device (only "No sound" [0] and
    // "Default" [1] have one); we also accept the index/name hints as corroboration.
    const bool driverEmpty = !(haveBi && bi.driver && bi.driver[0]);
    const bool isWindowsDefault =
        g_selectedDevice == -1 || g_selectedDevice == 1 ||
        g_selectedDeviceName.empty() ||
        _wcsicmp(g_selectedDeviceName.c_str(), L"Default") == 0 ||
        driverEmpty;

    if (isWindowsDefault) {
        // (1) Authoritative: ask Windows for the real default RENDER endpoint and
        //     match its unique enabled loopback. eConsole = what the user hears;
        //     eMultimedia as a fallback (they almost always coincide). NEVER
        //     eCommunications — BASS_Init binds to the console/multimedia default.
        for (ERole role : { eConsole, eMultimedia }) {
            int idx = FindUniqueEnabledLoopbackByTail(CoreAudioDefaultRenderTail(role));
            if (idx >= 0) return idx;
        }
        // (2) No-COM fallback: the BASSWASAPI render endpoint flagged DEFAULT.
        {
            int idx = FindUniqueEnabledLoopbackByTail(BasswasapiDefaultRenderTail());
            if (idx >= 0) return idx;
        }
        // (3) If BASS did give us a concrete endpoint id (non-default machines),
        //     correlate on it directly (exact / case / unique GUID-tail).
        if (haveBi && bi.driver && bi.driver[0]) {
            BASS_WASAPI_DEVICEINFO di;
            {
                // Tiers 1+2: exact / case-insensitive full id match. These are
                // unambiguous endpoint identities — return on first hit.
                for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
                    const DWORD f = di.flags;
                    if ((f & BASS_DEVICE_LOOPBACK) && (f & BASS_DEVICE_ENABLED) &&
                        EndpointIdExactOrCase(di.id, bi.driver)) {
                        return i;
                    }
                }
                // Tier 3: GUID-tail match (survives render-vs-capture "{0.0.x}."
                // prefix differences). To preserve the "never the wrong device"
                // invariant on machines with virtual cables (VB-Audio etc.) where
                // tails could collide, accept this ONLY when EXACTLY ONE enabled
                // loopback shares the tail. Otherwise fall through to name match.
                std::string targetTail = EndpointGuidTailLower(bi.driver);
                if (!targetTail.empty()) {
                    int found = -1, count = 0;
                    for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
                        const DWORD f = di.flags;
                        if ((f & BASS_DEVICE_LOOPBACK) && (f & BASS_DEVICE_ENABLED) &&
                            di.id && di.id[0] &&
                            EndpointGuidTailLower(di.id) == targetTail) {
                            ++count;
                            found = i;
                        }
                    }
                    if (count == 1) return found;
                }
            }
            // Secondary: match the loopback by the BASS device's name.
            if (bi.name && bi.name[0]) {
                std::wstring target = WasapiNameToWide(bi.name);
                for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
                    const DWORD f = di.flags;
                    if ((f & BASS_DEVICE_LOOPBACK) && (f & BASS_DEVICE_ENABLED) &&
                        NameMatches(WasapiNameToWide(di.name), target)) {
                        return i;
                    }
                }
            }
        }
        return -1;   // caller MUST abort with a clear message — never front()
    }

    // Named device: name-based correlation (existing behaviour, unchanged).
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
        BASS_Encode_Stop(g_systemEncoder);  // finalizes the file with captured spans
        g_systemEncoder = 0;
    }
    g_systemPaused.store(false);  // v2.24 — clear paused state on any teardown

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
    g_systemPaused.store(false); // v2.24 — never inherit a stale paused state

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

// v2.24 — pause/resume the encoder of an in-progress system capture. We pause
// ONLY the encoder, not WASAPI: BASS_WASAPI keeps running so the device-loss
// watchdog stays satisfied, the drain loop keeps emptying the push stream (no
// buildup), and the paused encoder simply drops the pulled bytes — so the file
// just skips the paused span. BASS encode functions are thread-safe.
// We snapshot g_systemEncoder into a local: the guard makes it live in the common
// case, but a device-loss self-stop on the drain thread can zero it right after
// the guard — the local + null-check closes that narrow window cleanly.
bool ToggleSystemCapturePaused() {
    if (!g_isSystemCapturing.load()) return false;
    HENCODE enc = g_systemEncoder;
    if (!enc) return false;
    bool np = !g_systemPaused.load();
    BASS_Encode_SetPaused(enc, np ? TRUE : FALSE);
    g_systemPaused.store(np);
    return np;
}

bool IsSystemCapturePaused() {
    return g_systemPaused.load();
}

bool ConsumeSystemCaptureLost() {
    return g_deviceLost.exchange(false);
}

// ----------------------------------------------------------------------------
// v2.12 — Audio diagnostic dump (Help -> Audio diagnostic).
//
// Writes BOTH device tables VERBATIM so we can see, on a tester's real machine,
// exactly why Automatic loopback detection fails (Sèb's case). The strings that
// FindLoopbackForCurrentBassDevice() compares — BASS_DEVICEINFO.driver and
// BASS_WASAPI_DEVICEINFO.id — are printed unmodified (no trim, no lowercase),
// because the whole point is to reveal the literal bytes (case, "{0.0.x.0...}"
// prefix, empty string, etc.). Read-only: enumerates, never inits/captures.
// Returns the full path written, or L"" on failure.
// ----------------------------------------------------------------------------
static std::string Utf8FromWide(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::wstring WriteAudioDiagnostic() {
    std::string r;
    char line[1024];

    snprintf(line, sizeof(line),
             "=== MediaAccess audio diagnostic (v%s) ===\r\n", APP_VERSION);
    r += line;
    r += "Purpose: diagnose Automatic system-recording device detection.\r\n";
    r += "Strings below are VERBATIM (case and prefixes preserved).\r\n\r\n";

    snprintf(line, sizeof(line), "g_selectedDevice = %d\r\n", g_selectedDevice);
    r += line;
    r += "g_selectedDeviceName = \"" + Utf8FromWide(g_selectedDeviceName) + "\"\r\n";

    int cur = static_cast<int>(BASS_GetDevice());
    snprintf(line, sizeof(line), "BASS_GetDevice() = %d\r\n\r\n", cur);
    r += line;

    // --- BASS output devices ---
    r += "-- BASS output devices (BASS_GetDeviceInfo) --\r\n";
    {
        BASS_DEVICEINFO bi;
        for (int i = 0; BASS_GetDeviceInfo(static_cast<DWORD>(i), &bi); ++i) {
            const DWORD f = bi.flags;
            snprintf(line, sizeof(line),
                     "[%d] flags=0x%08lX ENABLED=%d DEFAULT=%d INIT=%d\r\n",
                     i, (unsigned long)f,
                     (f & BASS_DEVICE_ENABLED) ? 1 : 0,
                     (f & BASS_DEVICE_DEFAULT) ? 1 : 0,
                     (f & BASS_DEVICE_INIT)    ? 1 : 0);
            r += line;
            r += "      name=\"";
            r += (bi.name ? bi.name : "");
            r += "\"\r\n      driver=\"";
            r += (bi.driver ? bi.driver : "");
            r += "\"\r\n";
        }
    }

    // --- BASSWASAPI devices ---
    r += "\r\n-- BASSWASAPI devices (BASS_WASAPI_GetDeviceInfo) --\r\n";
    {
        BASS_WASAPI_DEVICEINFO di;
        for (int i = 0; BASS_WASAPI_GetDeviceInfo(i, &di); ++i) {
            const DWORD f = di.flags;
            snprintf(line, sizeof(line),
                     "[%d] flags=0x%08lX LOOPBACK=%d INPUT=%d ENABLED=%d DEFAULT=%d\r\n",
                     i, (unsigned long)f,
                     (f & BASS_DEVICE_LOOPBACK) ? 1 : 0,
                     (f & BASS_DEVICE_INPUT)    ? 1 : 0,
                     (f & BASS_DEVICE_ENABLED)  ? 1 : 0,
                     (f & BASS_DEVICE_DEFAULT)  ? 1 : 0);
            r += line;
            r += "      name=\"";
            r += (di.name ? di.name : "");
            r += "\"\r\n      id=\"";
            r += (di.id ? di.id : "");
            r += "\"\r\n";
        }
    }

    int match = FindLoopbackForCurrentBassDevice();
    snprintf(line, sizeof(line),
             "\r\n-- Correlation --\r\nFindLoopbackForCurrentBassDevice() = %d"
             "  (-1 = no match -> \"Unable to identify\")\r\n", match);
    r += line;

    // Resolve a writable path: Music folder (same as recordings), else cwd.
    wchar_t dir[MAX_PATH] = {0};
    std::wstring path;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, dir))) {
        path = dir;
    } else if (GetCurrentDirectoryW(MAX_PATH, dir) > 0) {
        path = dir;
    } else {
        return L"";
    }
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"MediaAccess_audio_diagnostic.txt";

    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return L"";
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};  // UTF-8 BOM for Notepad
    f.write(reinterpret_cast<const char*>(bom), 3);
    f.write(r.data(), static_cast<std::streamsize>(r.size()));
    if (!f) return L"";
    return path;
}

} // namespace mediaaccess
