#include "player.h"
#include "globals.h"
#include "utils.h"
#include "accessibility.h"
#include "settings.h"
#include "ui.h"
#include "effects.h"
#include "database.h"
#include "resource.h"
#include "mediaaccess/video_engine.h"
#include "mediaaccess/youtube.h"  // YouTubeCancelHybrid()
#include "mediaaccess/logger.h"   // LogF
#include "mediaaccess/daisy_player.h"  // DaisyClose() when loading other media
#include "mediaaccess/wasapi_loopback.h"  // v1.94 — system-audio (loopback) recording
#include "mediaaccess/audio_device_watcher.h"  // v2.32 — auto-follow default output device

// Forward declaration so LoadURL (earlier in the file) can call it.
static void TeardownMpvBeforeLoad();
#include "mediaaccess/translations.h"
#include "bass_fx.h"
#include "bass_aac.h"
#include "bassmidi.h"
#include "bassenc.h"
#include "bassenc_mp3.h"
#include "bassenc_ogg.h"
#include "bassenc_flac.h"
#include "tempo_processor.h"
#include <ctime>
#include <shlobj.h>
#include <map>
#include <algorithm>  // std::sort (v2.43 bookmark navigation)
#include <algorithm>
#include <vector>
#include <cstdint>

// Forward declarations for tag reading helpers (defined later in file)
static std::string GetMetadataTag(HSTREAM stream, const char* tagName);
static std::string GetStreamTitle(HSTREAM stream);
static std::string GetTrimmedTag(const char* data, size_t maxLen);
static std::string GetStationName(HSTREAM stream);  // v1.60 — used by AnnounceStreamMetadata

// Global SoundFont handle for MIDI playback
static HSOUNDFONT g_hSoundFont = 0;

// Track loaded plugins for debugging
static std::vector<std::wstring> g_loadedPlugins;
static std::vector<std::wstring> g_failedPlugins;

// Load every optional BASS format plugin shipped in <exe>\lib\.
// Failures are recorded in g_failedPlugins but don't abort startup — the
// app simply won't be able to play formats backed by the missing DLLs.
// Surfaced to the user via Help → Loaded modules using friendly format
// names (autonomy rule: never mention DLL filenames).
void LoadBassPlugins() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // Get directory
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
    }

    std::wstring libPath = exePath;
    libPath += L"lib\\";

    // List of plugins to load (playback-related)
    const wchar_t* plugins[] = {
        L"bassflac.dll",   // FLAC
        L"bassopus.dll",   // Opus
        L"basswma.dll",    // WMA
        L"basswv.dll",     // WavPack
        L"bassape.dll",    // Monkey's Audio (APE)
        L"bassalac.dll",   // Apple Lossless (ALAC)
        L"bassmidi.dll",   // MIDI
        L"basscd.dll",     // CD Audio
        L"bassdsd.dll",    // DSD
        L"basshls.dll",    // HLS streaming
        L"bassmix.dll",    // Mixer (for some stream types)
        L"bass_aac.dll",   // AAC/M4A (if available)
    };

    for (const wchar_t* plugin : plugins) {
        std::wstring fullPath = libPath + plugin;
        HPLUGIN hPlugin = BASS_PluginLoad(reinterpret_cast<const char*>(fullPath.c_str()), BASS_UNICODE);
        // If lib\ path failed, try same directory as exe
        if (!hPlugin) {
            std::wstring altPath = exePath;
            altPath += plugin;
            hPlugin = BASS_PluginLoad(reinterpret_cast<const char*>(altPath.c_str()), BASS_UNICODE);
        }

        if (hPlugin) {
            g_loadedPlugins.push_back(plugin);
        } else {
            g_failedPlugins.push_back(plugin);
        }
    }
}

// Map a bundled plugin filename to the user-friendly audio format it adds.
// Used by the Help → Loaded modules diagnostic so we never surface DLL
// filenames in the UI (autonomy rule: no implementation detail).
static const wchar_t* PluginFriendlyName(const std::wstring& dll) {
    if (dll == L"bassflac.dll")  return L"FLAC";
    if (dll == L"bassopus.dll")  return L"Opus";
    if (dll == L"basswma.dll")   return L"Windows Media Audio";
    if (dll == L"basswv.dll")    return L"WavPack";
    if (dll == L"bassape.dll")   return L"Monkey's Audio (APE)";
    if (dll == L"bassalac.dll")  return L"Apple Lossless (ALAC)";
    if (dll == L"bassmidi.dll")  return L"MIDI";
    if (dll == L"basscd.dll")    return L"CD audio";
    if (dll == L"bassdsd.dll")   return L"DSD";
    if (dll == L"basshls.dll")   return L"HLS streams";
    if (dll == L"bassmix.dll")   return L"mixer";
    if (dll == L"bass_aac.dll")  return L"AAC / M4A";
    return dll.c_str();
}

// Get list of supported audio formats. Friendly names only — no DLL paths
// or filenames. Shown by Help → Loaded modules so users can verify their
// install is complete.
std::wstring GetLoadedPluginsInfo() {
    std::wstring info = T("Audio format support:");
    info += L"\n\n";
    for (const auto& p : g_loadedPlugins) {
        info += L"\xE2\x9C\x93 ";  // checkmark
        info += PluginFriendlyName(p);
        info += L"\n";
    }
    if (!g_failedPlugins.empty()) {
        info += L"\n";
        info += T("Some audio format modules failed to load. Please reinstall MediaAccess.");
    }
    return info;
}

// BASS reports device names in the active code page (CP_ACP). Convert to
// wstring and drop the embedded trailing NUL that MultiByteToWideChar leaves
// in the buffer, so comparisons against std::wstring literals just work.
static std::wstring BassDeviceNameToWide(const char* deviceName) {
    if (!deviceName) return L"";
    int len = MultiByteToWideChar(CP_ACP, 0, deviceName, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wideName(len, 0);
    MultiByteToWideChar(CP_ACP, 0, deviceName, -1, &wideName[0], len);
    if (!wideName.empty() && wideName.back() == L'\0') {
        wideName.pop_back();
    }
    return wideName;
}

// Find device index by name, returns -1 if not found (use default)
int FindDeviceByName(const std::wstring& name) {
    if (name.empty()) return -1;  // Empty name means default device

    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (info.flags & BASS_DEVICE_ENABLED) {
            if (BassDeviceNameToWide(info.name) == name) {
                return i;
            }
        }
    }
    return -1;  // Not found, use default
}

// Get device name by index
std::wstring GetDeviceName(int device) {
    if (device <= 0) return L"";  // Default device

    BASS_DEVICEINFO info;
    if (BASS_GetDeviceInfo(device, &info)) {
        return BassDeviceNameToWide(info.name);
    }
    return L"";
}

// Show popup menu with audio devices
void ShowAudioDeviceMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    BASS_DEVICEINFO info;
    int itemCount = 0;

    for (int i = 1; BASS_GetDeviceInfo(i, &info); i++) {
        if (info.flags & BASS_DEVICE_ENABLED) {
            std::wstring wideName = BassDeviceNameToWide(info.name);

            UINT flags = MF_STRING;
            // Check if this is the current device
            if (i == g_selectedDevice || (g_selectedDevice == -1 && (info.flags & BASS_DEVICE_DEFAULT))) {
                flags |= MF_CHECKED;
            }

            AppendMenuW(hMenu, flags, IDM_AUDIO_DEVICE_BASE + i, wideName.c_str());
            itemCount++;
        }
    }

    if (itemCount == 0) {
        DestroyMenu(hMenu);
        Speak(Ts("No audio devices found"));
        return;
    }

    // Check if window was hidden (for global hotkey support)
    bool wasHidden = !IsWindowVisible(hwnd);
    if (wasHidden) {
        ShowWindow(hwnd, SW_SHOW);
    }

    // Get cursor position for menu
    POINT pt;
    GetCursorPos(&pt);

    // Show the popup menu and get the selected command
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    // Handle device selection
    if (cmd >= IDM_AUDIO_DEVICE_BASE && cmd < IDM_AUDIO_DEVICE_BASE + 100) {
        int deviceIndex = cmd - IDM_AUDIO_DEVICE_BASE;
        SelectAudioDevice(deviceIndex);
    }

    // Re-hide window if it was hidden before
    if (wasHidden) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

// Select and switch to an audio device
void SelectAudioDevice(int deviceIndex) {
    if (deviceIndex <= 0) return;

    // Get device name for announcement
    std::wstring deviceName = GetDeviceName(deviceIndex);

    // Try to reinitialize BASS with the new device
    if (ReinitBass(deviceIndex)) {
        g_selectedDevice = deviceIndex;
        g_selectedDeviceName = deviceName;
        SaveSettings();

        // Announce the change
        std::string msg = Ts("Switched to ") + WideToUtf8(deviceName);
        Speak(msg.c_str());
    } else {
        Speak(Ts("Failed to switch audio device"));
    }
}

// Check if file is a MIDI file by extension
static bool IsMidiFile(const wchar_t* path) {
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return (_wcsicmp(ext, L".mid") == 0 || _wcsicmp(ext, L".midi") == 0 ||
            _wcsicmp(ext, L".kar") == 0 || _wcsicmp(ext, L".rmi") == 0);
}

// Apply MIDI settings (SoundFont, max voices)
//
// SoundFont resolution order:
//   1. User-configured path (g_midiSoundFont, set in Options → MIDI)
//   2. Bundled FluidR3_GM.sf2 at <exe dir>\lib\FluidR3_GM.sf2 (installed
//      by default; downloaded by download-deps.bat for dev builds)
//   3. BASSMIDI built-in synth (last resort — sounds basic but plays)
//
// The fallback runs at apply time (not save time), so a user can clear
// their custom SoundFont in Options and silently fall back to FluidR3.
void ApplyMidiSettings() {
    // Free previous SoundFont if any
    if (g_hSoundFont) {
        BASS_MIDI_FontFree(g_hSoundFont);
        g_hSoundFont = 0;
    }

    // Set max voices for MIDI playback
    BASS_SetConfig(BASS_CONFIG_MIDI_VOICES, g_midiMaxVoices);

    // Decide which SoundFont path to load
    std::wstring sfPath = g_midiSoundFont;
    if (sfPath.empty()) {
        // No user-configured SoundFont — look for bundled FluidR3_GM.sf2
        wchar_t exePath[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            wchar_t* lastSlash = wcsrchr(exePath, L'\\');
            if (lastSlash) {
                *(lastSlash + 1) = L'\0';
                std::wstring bundled = std::wstring(exePath) + L"lib\\FluidR3_GM.sf2";
                // Only use the bundled file if it actually exists; otherwise
                // we leave sfPath empty and BASSMIDI uses its built-in synth.
                DWORD attrs = GetFileAttributesW(bundled.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    sfPath = bundled;
                }
            }
        }
    }

    // Load SoundFont if we have a path
    if (!sfPath.empty()) {
        g_hSoundFont = BASS_MIDI_FontInit(sfPath.c_str(), 0);
        if (g_hSoundFont) {
            // Set as default SoundFont for all MIDI streams
            BASS_MIDI_FONT font;
            font.font = g_hSoundFont;
            font.preset = -1;  // All presets
            font.bank = 0;
            BASS_MIDI_StreamSetFonts(0, &font, 1);  // 0 = set default
        }
        // If BASS_MIDI_FontInit failed (corrupt file etc.), g_hSoundFont stays
        // 0 and BASSMIDI silently uses its built-in synth. No error popup —
        // we never block playback over a SoundFont issue.
    }
}

// Initialize BASS for the user's preferred audio device (or default).
// Returns false (with a message box) only if BOTH the preferred device and
// the system default fail to init — at that point we have no audio output.
//
// Order is important:
//   1. BASS_SetConfig() for buffer/update/curve MUST happen before BASS_Init
//      — these only take effect at init time.
//   2. BASS_Init.
//   3. LoadBassPlugins — needs an init'd BASS to register the plugin.
//   4. ApplyMidiSettings — sets the default soundfont for any future MIDI
//      streams; cheap to call even if no MIDI is ever loaded.
//   5. Network configs for URL streaming.
bool InitBass(HWND hwnd) {
    // Apply buffer settings before init
    BASS_SetConfig(BASS_CONFIG_BUFFER, g_bufferSize);
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, g_updatePeriod);

    // Use logarithmic volume curve (more natural for human perception)
    BASS_SetConfig(BASS_CONFIG_CURVE_VOL, TRUE);

    // Find device by saved name
    int device = FindDeviceByName(g_selectedDeviceName);
    g_selectedDevice = device;

    if (!BASS_Init(device, 44100, 0, hwnd, nullptr)) {
        // Fall back to default device if the saved one is gone (USB DAC
        // unplugged, Bluetooth speaker out of range, etc.). Clear the saved
        // selection so we don't repeat the failure on next startup.
        if (device != -1) {
            if (BASS_Init(-1, 44100, 0, hwnd, nullptr)) {
                g_selectedDevice = -1;
                g_selectedDeviceName.clear();
            } else {
                MessageBoxW(hwnd, T("Failed to initialize BASS audio library."), APP_NAME, MB_ICONERROR);
                return false;
            }
        } else {
            MessageBoxW(hwnd, T("Failed to initialize BASS audio library."), APP_NAME, MB_ICONERROR);
            return false;
        }
    }

    // Load plugins for additional format support
    LoadBassPlugins();

    // Apply MIDI settings (SoundFont, max voices)
    ApplyMidiSettings();

    // Configure network settings for URL streaming (YouTube, etc.)
    BASS_SetConfigPtr(BASS_CONFIG_NET_AGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
    BASS_SetConfig(BASS_CONFIG_NET_TIMEOUT, 30000);  // 30 second timeout
    BASS_SetConfig(BASS_CONFIG_NET_BUFFER, 10000);   // 10 second network buffer (helps with long streams)
    BASS_SetConfig(BASS_CONFIG_NET_PREBUF, 50);      // Start playback when 50% buffered

    return true;
}

// Free all BASS resources at shutdown.
// Always BASS_ChannelStop() before BASS_StreamFree() — without the explicit
// stop, freeing a still-playing stream is racy: BASS may continue producing
// samples from its internal buffer briefly. Belt-and-suspenders.
//
// g_sourceStream is intentionally NOT freed here — for the SoundTouch path
// it's owned by g_fxStream via BASS_FX_FREESOURCE, and for Speedy/Signalsmith
// it's owned by the tempo processor; double-freeing either way crashes.
void FreeBass() {
    if (g_fxStream) {
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        BASS_ChannelStop(g_stream);
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }
    g_sourceStream = 0;
    g_currentBitrate = 0;
    BASS_Free();
}

// Check if a path is a URL
bool IsURL(const wchar_t* path) {
    if (!path) return false;
    return (_wcsnicmp(path, L"http://", 7) == 0 ||
            _wcsnicmp(path, L"https://", 8) == 0 ||
            _wcsnicmp(path, L"ftp://", 6) == 0);
}

// Forward declarations for video engine helpers (defined later in this file)
static bool LoadVideoFile(const wchar_t* path);
static bool LoadVideoURL(const wchar_t* url);

// Load and play an http(s) URL stream — internet radio, podcasts,
// arbitrary HTTP audio, or a YouTube URL (which is forwarded to MPV).
//
// silentOnFail = true is used by the YouTube hybrid path: it tries BASS
// first to grab fast-starting audio while libmpv extracts the higher-
// quality video, and we don't want a BASS failure box if the YouTube
// extraction is going to succeed anyway.
//
// Cleans up any prior stream and any in-flight MPV playback before opening
// the new one. Format detection tries AAC first, then generic, with and
// without BASS_STREAM_BLOCK — BLOCK mode is only useful for live streams
// where seeking isn't expected.
bool LoadURL(const wchar_t* url, bool silentOnFail) {
    // Validate URL scheme — only allow http:// and https://
    if (!url ||
        (_wcsnicmp(url, L"http://", 7) != 0 && _wcsnicmp(url, L"https://", 8) != 0)) {
        MessageBoxW(GetMessageBoxOwner(),
            T("Only http:// and https:// URLs are supported."),
            APP_NAME, MB_ICONWARNING);
        return false;
    }

    // Route YouTube URLs (raw, not extracted stream URLs) to MPV if available
    if (IsMPVAvailable()) {
        std::wstring urlStr(url);
        if (urlStr.find(L"youtube.com/watch") != std::wstring::npos ||
            urlStr.find(L"youtu.be/") != std::wstring::npos ||
            urlStr.find(L"youtube.com/playlist") != std::wstring::npos) {
            return LoadVideoURL(url);
        }
    }

    g_isLoading = true;

    // Same dual-engine safety as LoadFile: if mpv is currently playing a
    // video, stop it before opening a BASS URL stream (radio, podcast,
    // arbitrary HTTP audio). Without this the video keeps running on top
    // of the new audio stream.
    TeardownMpvBeforeLoad();

    // Free existing streams safely
    if (g_fxStream) {
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        RemoveDSPEffects();
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        if (g_metaSync) {
            BASS_ChannelRemoveSync(g_stream, g_metaSync);
            g_metaSync = 0;
        }
        if (g_dlSync) {
            BASS_ChannelRemoveSync(g_stream, g_dlSync);
            g_dlSync = 0;
        }
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }

    // Convert URL to UTF-8 for BASS
    std::string urlUtf8 = WideToUtf8(url);

    // Create URL stream - try without BLOCK first (allows seeking for podcasts)
    // BLOCK mode is only needed for live streams where seeking isn't expected
    DWORD flags = BASS_STREAM_DECODE | BASS_STREAM_STATUS | BASS_SAMPLE_FLOAT;

    // Try BASS_AAC_StreamCreateURL first (handles raw AAC/M4A better)
    g_stream = BASS_AAC_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);

    // If AAC fails, try generic
    if (!g_stream) {
        g_stream = BASS_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    // If non-BLOCK fails, try with BLOCK (for live streams)
    if (!g_stream) {
        flags = BASS_STREAM_DECODE | BASS_STREAM_STATUS | BASS_STREAM_BLOCK | BASS_SAMPLE_FLOAT;
        g_stream = BASS_AAC_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    if (!g_stream) {
        g_stream = BASS_StreamCreateURL(urlUtf8.c_str(), 0, flags, nullptr, nullptr);
    }

    if (!g_stream) {
        g_isLoading = false;
        int error = BASS_ErrorGetCode();
        const wchar_t* errorMsg;
        switch (error) {
            case BASS_ERROR_NONET:    errorMsg = T("No internet connection."); break;
            case BASS_ERROR_FILEOPEN: errorMsg = T("Could not connect to URL."); break;
            case BASS_ERROR_FILEFORM: errorMsg = T("Unsupported stream format."); break;
            case BASS_ERROR_CODEC:    errorMsg = T("Required codec is not available."); break;
            case BASS_ERROR_FORMAT:   errorMsg = T("Unsupported sample format."); break;
            case BASS_ERROR_TIMEOUT:  errorMsg = T("Connection timed out."); break;
            case BASS_ERROR_SSL:      errorMsg = T("SSL/HTTPS not supported."); break;
            default:                  errorMsg = T("Could not open stream."); break;
        }
        // Show truncated URL in error message
        std::wstring displayUrl = url;
        if (displayUrl.length() > 100) {
            displayUrl = displayUrl.substr(0, 100) + L"...";
        }
        std::wstring msg = T("Cannot play URL:");
        msg += L"\n";
        msg += displayUrl;
        msg += L"\n\n";
        msg += errorMsg;
        // Internal BASS error code is logged but never shown to the user
        // (autonomy rule: never surface implementation detail).
        LogF("BASS", "URL load failed: code=%d", error);
        if (!silentOnFail) {
            // v2.52 — speak the reason first so a screen-reader user hears why
            // the stream failed even if the modal box never takes focus; then
            // show the box for sighted users. Gated by !silentOnFail so silent
            // probe/fallback loads stay quiet.
            std::wstring spoken = T("Cannot play URL:");
            spoken += L" ";
            spoken += errorMsg;
            SpeakW(spoken);
            MessageBoxW(GetMessageBoxOwner(), msg.c_str(), APP_NAME, MB_ICONERROR);
        }
        return false;
    }

    // Get original sample frequency for rate control
    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(g_stream, &info);
    g_originalFreq = static_cast<float>(info.freq);

    // Store source stream for bitrate queries
    g_sourceStream = g_stream;

    // Capture initial bitrate (may come from stream headers or BASS attribute)
    float bitrate = 0;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_BITRATE, &bitrate);
    g_currentBitrate = static_cast<int>(bitrate);
    // If no BASS bitrate, ICY headers will be checked by GetCurrentBitrate()

    // Set up metadata sync for stream title changes (internet radio)
    g_metaSync = BASS_ChannelSetSync(g_stream, BASS_SYNC_META, 0, OnMetaChange, nullptr);

    // For internet streams, always use SoundTouch - Speedy and Signalsmith
    // use push-based processing that doesn't work well with network buffering
    SetCurrentAlgorithm(TempoAlgorithm::SoundTouch);

    // Check if this is a live (non-seekable) stream BEFORE setting up tempo
    // Live streams have unknown length (-1 or 0)
    QWORD streamLen = BASS_ChannelGetLength(g_stream, BASS_POS_BYTE);
    g_isLiveStream = (streamLen == (QWORD)-1 || streamLen == 0);

    // Create or reinitialize tempo processor
    FreeTempoProcessor();
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
        g_isLoading = false;
        if (!silentOnFail) MessageBoxW(GetMessageBoxOwner(), T("Failed to create tempo processor."), APP_NAME, MB_ICONERROR);
        return false;
    }

    // Restore pitch settings (always works)
    // Only restore tempo if not a live stream (tempo doesn't work on live streams)
    if (!g_isLiveStream) {
        processor->SetTempo(g_tempo);
    }
    processor->SetPitch(g_pitch);

    // Initialize processor - this creates the output stream
    g_fxStream = processor->Initialize(g_stream, g_originalFreq);
    if (!g_fxStream) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
        g_isLoading = false;
        if (!silentOnFail) MessageBoxW(GetMessageBoxOwner(), T("Failed to create tempo stream for URL."), APP_NAME, MB_ICONERROR);
        return false;
    }

    // For SoundTouch, g_stream is now owned by g_fxStream (BASS_FX_FREESOURCE)
    if (processor->GetAlgorithm() == TempoAlgorithm::SoundTouch) {
        g_stream = 0;  // Prevent double-free
    }

    // Apply rate using native BASS frequency attribute (skip for live streams)
    if (g_rate != 1.0f && !g_isLiveStream) {
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
    }

    // Set larger playback buffer for streams (helps prevent choppiness during long playback)
    BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_BUFFER, 1.0f);  // 1 second buffer

    // Apply DSP effects (including volume DSP which handles g_volume/g_muted)
    ApplyDSPEffects();

    // Set up end sync for auto-advance
    g_endSync = BASS_ChannelSetSync(g_fxStream, BASS_SYNC_END, 0, OnTrackEnd, nullptr);

    // Chapter detection for URL streams (podcasts in particular). Two-step
    // strategy:
    //
    //   1. Immediate parse: for non-BLOCK URL streams BASS reads the file
    //      header during the initial probe, so BASS_TAG_ID3V2 is usually
    //      available right now. Covers the common podcast case.
    //
    //   2. BASS_SYNC_DOWNLOAD fallback: fires once the full file has been
    //      downloaded. If the immediate parse left g_chapters empty (e.g.
    //      ID3v2 block sat at the end of the file, or an ID3v2_2 refresh
    //      arrives mid-stream), we re-parse from the now-complete handle.
    //      Skipped for live streams where the sync would never fire.
    if (!g_isLiveStream && g_sourceStream) {
        ParseChapters(g_sourceStream);
        g_dlSync = BASS_ChannelSetSync(
            g_sourceStream,
            BASS_SYNC_DOWNLOAD | BASS_SYNC_ONETIME,
            0,
            [](HSYNC, DWORD, DWORD, void*) {
                if (g_chapters.empty() && g_sourceStream) {
                    ParseChapters(g_sourceStream);
                }
            },
            nullptr);
    }

    // Start playback
    BASS_ChannelPlay(g_fxStream, FALSE);

    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Parse time string in format HH:MM:SS.mmm or MM:SS.mmm or SS.mmm
static double ParseChapterTime(const char* timeStr) {
    int hours = 0, mins = 0;
    double secs = 0.0;

    // Try HH:MM:SS.mmm format
    if (sscanf(timeStr, "%d:%d:%lf", &hours, &mins, &secs) == 3) {
        return hours * 3600.0 + mins * 60.0 + secs;
    }
    // Try MM:SS.mmm format
    if (sscanf(timeStr, "%d:%lf", &mins, &secs) == 2) {
        return mins * 60.0 + secs;
    }
    // Try SS.mmm format
    if (sscanf(timeStr, "%lf", &secs) == 1) {
        return secs;
    }
    return 0.0;
}

// Parse VorbisComment chapters (used by Ogg, FLAC, Opus)
// Format: CHAPTER001=00:00:00.000 and CHAPTER001NAME=Chapter Name
static void ParseVorbisCommentChapters(HSTREAM stream) {
    const char* tags = BASS_ChannelGetTags(stream, BASS_TAG_OGG);
    if (!tags) return;

    // Build a map of chapter numbers to times and names
    std::map<int, std::pair<double, std::string>> chapterMap;

    // Iterate through all tags
    while (*tags) {
        std::string tag = tags;
        tags += tag.length() + 1;

        // Check for CHAPTERnnn= format
        if (_strnicmp(tag.c_str(), "CHAPTER", 7) == 0) {
            const char* p = tag.c_str() + 7;
            // Parse the chapter number
            int num = 0;
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p - '0');
                p++;
            }
            if (num > 0) {
                if (_strnicmp(p, "NAME=", 5) == 0) {
                    // This is a chapter name
                    chapterMap[num].second = p + 5;
                } else if (*p == '=') {
                    // This is a chapter time
                    chapterMap[num].first = ParseChapterTime(p + 1);
                }
            }
        }
    }

    // Convert map to sorted chapter list
    for (const auto& kv : chapterMap) {
        if (kv.second.first > 0 || kv.first == 1) {  // Allow chapter 1 at 0:00
            Chapter ch;
            ch.position = kv.second.first;
            if (kv.second.second.empty()) {
                // Generate default name
                wchar_t buf[32];
                swprintf(buf, 32, L"Chapter %d", kv.first);
                ch.name = buf;
            } else {
                ch.name = Utf8ToWide(kv.second.second);
            }
            g_chapters.push_back(ch);
        }
    }
}

// Parse ID3v2 CHAP frames (used by MP3) into g_chapters.
//
// CHAP frame layout (per ID3v2 chapter spec):
//   Element ID (null-terminated string)
//   Start time     (4 bytes, big-endian, milliseconds)
//   End time       (4 bytes)
//   Start offset   (4 bytes)
//   End offset     (4 bytes)
//   Optional sub-frames (typically TIT2 for the chapter title)
//
// Tag size is decoded from the ID3v2 syncsafe integer header. Version 2.3
// uses regular big-endian frame sizes, 2.4 uses syncsafe frame sizes —
// we branch on `version` for both. Extended header (flag 0x40) is skipped.
// Sub-frames are walked looking for TIT2; everything else is ignored.
static void ParseID3v2Chapters(HSTREAM stream) {
    const unsigned char* id3v2 = (const unsigned char*)BASS_ChannelGetTags(stream, BASS_TAG_ID3V2);
    if (!id3v2) return;

    // Check ID3v2 header
    if (memcmp(id3v2, "ID3", 3) != 0) return;

    unsigned char version = id3v2[3];
    unsigned char flags = id3v2[5];

    // Calculate tag size (syncsafe integer)
    size_t tagSize = ((id3v2[6] & 0x7F) << 21) |
                     ((id3v2[7] & 0x7F) << 14) |
                     ((id3v2[8] & 0x7F) << 7) |
                     (id3v2[9] & 0x7F);

    size_t pos = 10;  // Header size

    // Skip extended header if present
    if (flags & 0x40) {
        size_t extSize = (version >= 4) ?
            (((id3v2[pos] & 0x7F) << 21) | ((id3v2[pos+1] & 0x7F) << 14) |
             ((id3v2[pos+2] & 0x7F) << 7) | (id3v2[pos+3] & 0x7F)) :
            ((id3v2[pos] << 24) | (id3v2[pos+1] << 16) |
             (id3v2[pos+2] << 8) | id3v2[pos+3]);
        pos += (version >= 4) ? extSize : (extSize + 4);
    }

    // Parse frames
    while (pos + 10 < tagSize + 10) {
        const unsigned char* frame = id3v2 + pos;

        // Check for padding (all zeros)
        if (frame[0] == 0) break;

        // Get frame ID
        char frameId[5] = {0};
        memcpy(frameId, frame, 4);

        // Get frame size
        size_t frameSize;
        if (version >= 4) {
            // v2.4 uses syncsafe integers
            frameSize = ((frame[4] & 0x7F) << 21) |
                        ((frame[5] & 0x7F) << 14) |
                        ((frame[6] & 0x7F) << 7) |
                        (frame[7] & 0x7F);
        } else {
            // v2.3 uses normal integers
            frameSize = (frame[4] << 24) | (frame[5] << 16) |
                        (frame[6] << 8) | frame[7];
        }

        if (frameSize == 0 || pos + 10 + frameSize > tagSize + 10) break;

        // Check if this is a CHAP frame
        if (strcmp(frameId, "CHAP") == 0) {
            const unsigned char* data = frame + 10;
            size_t dataLen = frameSize;

            // Find end of element ID (null-terminated)
            size_t elemIdLen = 0;
            while (elemIdLen < dataLen && data[elemIdLen] != 0) elemIdLen++;

            if (elemIdLen + 17 <= dataLen) {  // Element ID + null + 16 bytes for times/offsets
                // Start time in milliseconds (big-endian)
                uint32_t startMs = (data[elemIdLen + 1] << 24) |
                                   (data[elemIdLen + 2] << 16) |
                                   (data[elemIdLen + 3] << 8) |
                                   data[elemIdLen + 4];

                Chapter ch;
                ch.position = startMs / 1000.0;

                // Look for TIT2 sub-frame for chapter title
                size_t subPos = elemIdLen + 17;  // After times/offsets
                while (subPos + 10 < dataLen) {
                    char subFrameId[5] = {0};
                    memcpy(subFrameId, data + subPos, 4);

                    size_t subFrameSize;
                    if (version >= 4) {
                        subFrameSize = ((data[subPos + 4] & 0x7F) << 21) |
                                       ((data[subPos + 5] & 0x7F) << 14) |
                                       ((data[subPos + 6] & 0x7F) << 7) |
                                       (data[subPos + 7] & 0x7F);
                    } else {
                        subFrameSize = (data[subPos + 4] << 24) |
                                       (data[subPos + 5] << 16) |
                                       (data[subPos + 6] << 8) |
                                       data[subPos + 7];
                    }

                    if (subFrameSize == 0 || subPos + 10 + subFrameSize > dataLen) break;

                    if (strcmp(subFrameId, "TIT2") == 0 && subFrameSize > 1) {
                        // TIT2 frame: first byte is encoding, rest is text
                        unsigned char encoding = data[subPos + 10];
                        const unsigned char* textStart = data + subPos + 11;
                        size_t textLen = subFrameSize - 1;

                        if (encoding == 0 || encoding == 3) {
                            // ISO-8859-1 or UTF-8
                            std::string text((const char*)textStart, textLen);
                            // Remove trailing nulls
                            while (!text.empty() && text.back() == 0) text.pop_back();
                            ch.name = Utf8ToWide(text);
                        } else if (encoding == 1 || encoding == 2) {
                            // UTF-16 with or without BOM
                            if (textLen >= 2) {
                                bool bigEndian = (encoding == 2) ||
                                                 (textStart[0] == 0xFE && textStart[1] == 0xFF);
                                const unsigned char* start = textStart;
                                if (textStart[0] == 0xFF || textStart[0] == 0xFE) {
                                    start += 2;
                                    textLen -= 2;
                                }
                                std::wstring wtext;
                                for (size_t i = 0; i + 1 < textLen; i += 2) {
                                    wchar_t wc = bigEndian ?
                                        ((start[i] << 8) | start[i + 1]) :
                                        (start[i] | (start[i + 1] << 8));
                                    if (wc == 0) break;
                                    wtext += wc;
                                }
                                ch.name = wtext;
                            }
                        }
                        break;
                    }
                    subPos += 10 + subFrameSize;
                }

                // Generate default name if none found
                if (ch.name.empty()) {
                    wchar_t buf[32];
                    swprintf(buf, 32, L"Chapter %d", (int)g_chapters.size() + 1);
                    ch.name = buf;
                }

                g_chapters.push_back(ch);
            }
        }

        pos += 10 + frameSize;
    }

    // Sort chapters by position
    std::sort(g_chapters.begin(), g_chapters.end(),
              [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
}

// External chapters: a caller (Podcast 2.0 RSS chapter JSON, etc.) pre-loads
// chapters via SetExternalChapters() BEFORE the file/URL load runs. We then
// no-op the first ParseChapters() call so the in-file chapters don't
// clobber the externally provided ones. The flag is one-shot — the NEXT
// load goes through normal parsing.
static bool g_chaptersExternal = false;

void SetExternalChapters(const std::vector<Chapter>& chapters) {
    g_chapters = chapters;
    g_chaptersExternal = !chapters.empty();
    // v2.34 — external chapters from a non-cue source (e.g. Podcast 2.0 chapters)
    // are NOT cue tracks, so they must announce as "Chapter", not "Track". The
    // armed latch makes ParseChapters skip its embedded branch (where the flag is
    // otherwise reset), so this is the ONLY reset point for the external path.
    // The cue loader re-asserts g_chaptersAreCueTracks=true AFTER calling this.
    g_chaptersAreCueTracks = false;
}

// Fill g_chapters from whatever embedded-chapter format the stream uses.
// VorbisComment is checked first (covers Ogg/FLAC/Opus); if nothing turns
// up, fall back to ID3v2 CHAP (MP3). Other formats simply produce an empty
// chapter list, which makes the chapter navigation hotkeys no-op.
void ParseChapters(HSTREAM stream) {
    if (g_chaptersExternal) {
        g_chaptersExternal = false;
        return;
    }

    // v2.34 — real embedded parsing is running, so these chapters are NOT cue
    // tracks. Reset the wording flag and forget the stored cue path so a later
    // non-cue load says "Chapter" and isn't re-associated with the old cue on
    // restart.
    g_chaptersAreCueTracks = false;
    g_currentCuePath.clear();
    g_chapters.clear();

    if (!stream) return;

    ParseVorbisCommentChapters(stream);

    if (g_chapters.empty()) {
        ParseID3v2Chapters(stream);
    }
}

// Load and play a file (or URL)

// Shared preamble for the MPV loaders: tear down any BASS playback,
// ensure MPV is initialized, show the video window and size it sensibly.
// Returns false (with g_isLoading already cleared) if MPV could not be
// brought up — caller should propagate that immediately.
static bool PrepareForMpvLoad() {
    // Free existing BASS streams
    if (g_activeEngine == PlaybackEngine::BASS && g_fxStream) {
        if (g_endSync) { BASS_ChannelRemoveSync(g_fxStream, g_endSync); g_endSync = 0; }
        RemoveDSPEffects();
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) { BASS_StreamFree(g_stream); g_stream = 0; }
    g_sourceStream = 0;
    FreeTempoProcessor();

    if (!IsMPVInitialized()) {
        if (!InitMPV(g_videoHwnd)) {
            Speak(Ts("Failed to initialize video engine"));
            g_isLoading = false;
            return false;
        }
    }
    if (g_videoHwnd) ShowWindow(g_videoHwnd, SW_SHOW);
    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    if ((wr.right - wr.left) < 640 || (wr.bottom - wr.top) < 400)
        SetWindowPos(g_hwnd, nullptr, 0, 0, 854, 530, SWP_NOMOVE | SWP_NOZORDER);
    return true;
}

// Load a video file via MPV engine
static bool LoadVideoFile(const wchar_t* path) {
    g_isLoading = true;
    if (!PrepareForMpvLoad()) return false;
    if (!MPVLoadFile(path)) {
        Speak(Ts("Failed to load video"));
        g_isLoading = false;
        return false;
    }
    ApplyVideoVolume();   // v2.44 — single-owner video volume (keeps subtitle duck)
    MPVSetMute(g_muted);
    g_activeEngine = PlaybackEngine::MPV;
    g_isVideoPlaying = true;
    g_isLiveStream = false;
    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Load a video URL via MPV engine
static bool LoadVideoURL(const wchar_t* url) {
    g_isLoading = true;
    if (!PrepareForMpvLoad()) return false;
    if (!MPVLoadURL(url)) {
        Speak(Ts("Failed to load video URL"));
        g_isLoading = false;
        return false;
    }
    ApplyVideoVolume();   // v2.44 — single-owner video volume (keeps subtitle duck)
    MPVSetMute(g_muted);
    g_activeEngine = PlaybackEngine::MPV;
    g_isVideoPlaying = true;
    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// Tear down the MPV engine before loading non-video media.
//
// MediaAccess has two playback engines (BASS for audio, libmpv for video).
// LoadFile/LoadURL historically freed only BASS — if MPV was playing a video
// and the user opened an audio file (Ctrl+O, Ctrl+V, Enter in playlist,
// double-click, radio, podcast, YouTube cache hit, etc.), the video kept
// running on top of the new audio. This helper centralizes the cleanup so
// every entry point that loads new audio explicitly stops MPV first.
//
// Also cancels any pending YouTube hybrid swap so a background download
// that finishes seconds after a manual load doesn't clobber the user's
// chosen track.
static void TeardownMpvBeforeLoad() {
    YouTubeCancelHybrid();
    if (g_activeEngine == PlaybackEngine::MPV) {
        MPVStop();
        if (g_videoHwnd) ShowWindow(g_videoHwnd, SW_HIDE);
        g_isVideoPlaying = false;
        g_activeEngine = PlaybackEngine::None;
    }
}

// Check if extension is a known video format (regardless of mpv availability)
static bool IsVideoExtension(const wchar_t* path) {
    if (!path) return false;
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    static const wchar_t* videoExts[] = {
        L".mkv", L".avi", L".mov", L".webm", L".flv", L".ts", L".m2ts",
        L".vob", L".ogv", L".3gp", L".mpg", L".mpeg", L".m4v", L".divx", L".rmvb"
    };
    for (auto ve : videoExts)
        if (_wcsicmp(ext, ve) == 0) return true;
    return false;
}

// v1.76 — Extensions that share an MP4-style container and can legitimately
// carry either audio-only (M4B-style audiobook) or audio+video (Captvty
// recording, screencast, etc.). LoadFile probes these for a video track
// (HasMp4VideoTrack below) before deciding between MPV and BASS, so the
// user never silently loses the video track of a recorded TV show.
// .m4b is NOT included on purpose: by convention it's an audiobook in an
// MP4 container, BASS handles it fine with full tempo/pitch/DSP support,
// and the probe would be wasted work.
static bool IsAmbiguousMp4Ext(const wchar_t* path) {
    if (!path) return false;
    const wchar_t* ext = wcsrchr(path, L'.');
    if (!ext) return false;
    return _wcsicmp(ext, L".mp4") == 0;
}

// Recursively scan an ISO-BMFF (MP4) container atom for a `hdlr` atom with
// handler_type == 'vide'. Used to detect a video track in an MP4 file when
// the extension alone is ambiguous. Designed for the moov atom payload only;
// caller has already extracted moov from the file.
//
// Atom layout: 4 bytes big-endian size, 4 bytes type, then payload. Size of
// 1 means a 64-bit extended size follows; size of 0 means "extends to EOF".
// `hdlr` payload: 4 bytes version+flags, 4 bytes pre_defined, 4 bytes
// handler_type — so handler_type sits at offset 8 inside the payload.
static bool ScanMp4ForVideoHandler(const uint8_t* buf, size_t len) {
    size_t p = 0;
    while (p + 8 <= len) {
        uint64_t atomSize = ((uint64_t)buf[p] << 24) | ((uint64_t)buf[p+1] << 16) |
                            ((uint64_t)buf[p+2] << 8)  | (uint64_t)buf[p+3];
        size_t headerLen = 8;
        if (atomSize == 1) {
            if (p + 16 > len) break;
            atomSize = 0;
            for (int i = 0; i < 8; i++) atomSize = (atomSize << 8) | buf[p+8+i];
            headerLen = 16;
        } else if (atomSize == 0) {
            atomSize = len - p;
        }
        if (atomSize < headerLen || p + atomSize > len) break;
        const uint8_t* atomType = buf + p + 4;
        if (memcmp(atomType, "hdlr", 4) == 0) {
            if (atomSize >= headerLen + 12) {
                const uint8_t* handlerType = buf + p + headerLen + 8;
                if (memcmp(handlerType, "vide", 4) == 0) return true;
            }
        } else if (memcmp(atomType, "trak", 4) == 0 ||
                   memcmp(atomType, "mdia", 4) == 0) {
            if (ScanMp4ForVideoHandler(buf + p + headerLen,
                                       (size_t)(atomSize - headerLen)))
                return true;
        }
        p += (size_t)atomSize;
    }
    return false;
}

// v1.76 — Probe an MP4/ISO-BMFF file to see if it carries a video track.
// Returns true only if at least one `trak` with handler_type 'vide' is
// found inside the moov atom. Returns false on any I/O error or malformed
// container — better to let BASS try (and possibly succeed with the audio
// track) than to false-positive into MPV and hand the user a broken file.
//
// The moov atom is usually at the start (faststart MP4s) or at the very
// end (recordings that flush moov last). We scan the first 64 MB, and if
// not found, the last 4 MB. Anything weirder isn't worth chasing.
static bool HasMp4VideoTrack(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(h, &fileSize)) { CloseHandle(h); return false; }
    uint64_t size = (uint64_t)fileSize.QuadPart;

    auto readAt = [&](uint64_t offset, void* buf, DWORD len) -> bool {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
        DWORD got = 0;
        return ReadFile(h, buf, len, &got, nullptr) && got == len;
    };

    // Scan top-level atoms in [startPos, startPos + scanLimit) for `moov`.
    // Stores its payload offset and size in outOff/outSize. Returns true if found.
    auto findMoov = [&](uint64_t startPos, uint64_t scanLimit,
                        uint64_t& outOff, uint64_t& outSize) -> bool {
        uint64_t pos = startPos;
        uint64_t hardStop = (startPos + scanLimit > size) ? size : startPos + scanLimit;
        while (pos + 8 <= hardStop) {
            uint8_t hdr[8];
            if (!readAt(pos, hdr, 8)) return false;
            uint64_t atomSize = ((uint64_t)hdr[0] << 24) | ((uint64_t)hdr[1] << 16) |
                                ((uint64_t)hdr[2] << 8)  | (uint64_t)hdr[3];
            uint64_t headerLen = 8;
            if (atomSize == 1) {
                uint8_t ext[8];
                if (!readAt(pos + 8, ext, 8)) return false;
                atomSize = 0;
                for (int i = 0; i < 8; i++) atomSize = (atomSize << 8) | ext[i];
                headerLen = 16;
            } else if (atomSize == 0) {
                atomSize = size - pos;
            }
            if (atomSize < headerLen || pos + atomSize > size) return false;
            if (memcmp(hdr + 4, "moov", 4) == 0) {
                outOff = pos + headerLen;
                outSize = atomSize - headerLen;
                return true;
            }
            pos += atomSize;
        }
        return false;
    };

    uint64_t moovOffset = 0, moovSize = 0;
    findMoov(0, 64ULL * 1024 * 1024, moovOffset, moovSize);
    if (moovOffset == 0 && size > 4ULL * 1024 * 1024) {
        findMoov(size - 4ULL * 1024 * 1024, 4ULL * 1024 * 1024, moovOffset, moovSize);
    }

    if (moovOffset == 0 || moovSize == 0 || moovSize > 16ULL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    std::vector<uint8_t> moovBuf((size_t)moovSize);
    bool ok = readAt(moovOffset, moovBuf.data(), (DWORD)moovSize);
    CloseHandle(h);
    if (!ok) return false;

    return ScanMp4ForVideoHandler(moovBuf.data(), moovBuf.size());
}

// Load a local file (audio, video, or — if the path is a URL — punt to
// LoadURL/LoadVideoURL). Decides between BASS and MPV by extension, then
// runs the appropriate loader. Stops any DAISY book and any rival engine
// before starting so two media never play at once.
//
// Returns false on decode failure. The error box is suppressed when the
// playlist has more than one entry — so a single corrupt track in a long
// playlist doesn't spam the user; PlayTrack just advances past it.
bool LoadFile(const wchar_t* path) {
    // Loading any other media unloads the current DAISY book first so the
    // user doesn't end up with overlapping audio streams.
    mediaaccess::DaisyClose();

    if (IsURL(path)) {
        return LoadURL(path);
    }
    // Route unambiguous video files to MPV engine
    if (IsVideoFile(path)) {
        return LoadVideoFile(path);
    }
    // v1.76 — Smart routing for ambiguous MP4 containers. The .mp4 extension
    // is deliberately NOT in IsVideoFile because it may be an audio-only
    // file (an M4B-style audiobook in an MP4 wrapper, a podcast, etc.) that
    // BASS handles with full tempo/pitch/DSP. But a .mp4 may equally be a
    // recorded TV show with both AVC video and AAC audio — in which case
    // BASS would happily extract the audio track and the user would lose
    // the video silently. Probe the moov atom to detect a video track; if
    // present, route to MPV. If absent (true audio-only MP4), continue to
    // BASS. Triggered for Sèb's Captvty replay .mp4 in particular.
    if (IsAmbiguousMp4Ext(path) && IsMPVAvailable() && HasMp4VideoTrack(path)) {
        LogF("LoadFile", "ambiguous MP4 has video track, routing to MPV: %ls", path);
        return LoadVideoFile(path);
    }
    // Video file but mpv DLL failed to load — this should never happen on a
    // proper install (libmpv-2.dll is bundled in the lib\ folder). Show a
    // neutral error and suggest reinstall; never tell users to download
    // anything themselves — MediaAccess must be fully self-contained.
    if (IsVideoExtension(path)) {
        std::wstring msg = T("Cannot play video file:");
        msg += L"\n";
        msg += GetFileName(path);
        msg += L"\n\n";
        msg += T("The video playback engine could not be loaded. Please reinstall MediaAccess.");
        // Technical detail is written to the log file but never shown to
        // the user (autonomy rule: never surface DLL names, paths, or
        // implementation details in a user-facing message).
        if (!g_lastMpvLoadError.empty()) {
            int n = WideCharToMultiByte(CP_UTF8, 0, g_lastMpvLoadError.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string utf8(n > 0 ? n - 1 : 0, '\0');
            if (n > 0) WideCharToMultiByte(CP_UTF8, 0, g_lastMpvLoadError.c_str(), -1, &utf8[0], n, nullptr, nullptr);
            LogF("MPV", "video load failure detail: %s", utf8.c_str());
        }
        MessageBoxW(GetMessageBoxOwner(), msg.c_str(), APP_NAME, MB_ICONWARNING);
        return false;
    }
    g_isLoading = true;
    g_isLiveStream = false;  // Local files are always seekable

    // If a video is currently playing via mpv, stop it before starting BASS.
    // Without this, mpv keeps playing the video while BASS starts the audio
    // — two media playing simultaneously, which is exactly what we don't want.
    TeardownMpvBeforeLoad();

    // Free existing streams safely
    if (g_fxStream) {
        // Remove sync first to prevent callbacks during cleanup
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        // Remove DSP effects before freeing stream
        RemoveDSPEffects();
        // Stop playback before freeing
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }

    // Decode stream: BASS_MIDI_StreamCreateFile is only used when the user
    // wants sinc interpolation (Options → MIDI), which the generic
    // BASS_StreamCreateFile path can't request. Otherwise generic handles
    // every format including MIDI (lower CPU, lower quality).
    if (IsMidiFile(path) && g_midiSincInterp) {
        DWORD flags = BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT | BASS_MIDI_SINCINTER;
        g_stream = BASS_MIDI_StreamCreateFile(FALSE, path, 0, 0, flags, 0);
        // Apply SoundFont to this specific stream if loaded
        if (g_stream && g_hSoundFont) {
            BASS_MIDI_FONT font;
            font.font = g_hSoundFont;
            font.preset = -1;
            font.bank = 0;
            BASS_MIDI_StreamSetFonts(g_stream, &font, 1);
        }
    } else {
        g_stream = BASS_StreamCreateFile(FALSE, path, 0, 0, BASS_UNICODE | BASS_STREAM_DECODE | BASS_SAMPLE_FLOAT);
    }
    if (!g_stream) {
        g_isLoading = false;
        // v1.76 — Defensive fallback for ambiguous MP4 containers. The moov
        // probe above tries to route MP4s with video to MPV proactively,
        // but it may miss in rare cases (corrupted moov, unusual layout,
        // or a video track BASS rejects for codec reasons before we can
        // detect it). If BASS refused an .mp4, give MPV one last chance
        // — it can usually play what BASS cannot.
        if (IsAmbiguousMp4Ext(path) && IsMPVAvailable()) {
            LogF("LoadFile", "BASS rejected .mp4 (error %d), trying MPV fallback: %ls",
                 BASS_ErrorGetCode(), path);
            return LoadVideoFile(path);
        }
        // Only show error if this is the only file in the playlist
        if (g_playlist.size() <= 1) {
            int error = BASS_ErrorGetCode();
            const wchar_t* errorMsg;
            switch (error) {
                case BASS_ERROR_FILEOPEN: errorMsg = T("Could not open the file."); break;
                case BASS_ERROR_FILEFORM: errorMsg = T("Unsupported file format."); break;
                case BASS_ERROR_CODEC:    errorMsg = T("Required codec is not available."); break;
                case BASS_ERROR_FORMAT:   errorMsg = T("Unsupported sample format."); break;
                case BASS_ERROR_MEM:      errorMsg = T("Out of memory."); break;
                case BASS_ERROR_NO3D:     errorMsg = T("3D sound is not available."); break;
                default:                  errorMsg = T("Unknown error."); break;
            }
            std::wstring msg = T("Cannot play file:");
            msg += L"\n";
            msg += GetFileName(path);
            msg += L"\n\n";
            msg += errorMsg;
            MessageBoxW(GetMessageBoxOwner(), msg.c_str(), APP_NAME, MB_ICONERROR);
        }
        return false;
    }

    // Get original sample frequency for rate control
    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(g_stream, &info);
    g_originalFreq = static_cast<float>(info.freq);

    // Store source stream for VBR bitrate queries (not freed separately - owned by tempo processor)
    g_sourceStream = g_stream;

    // Capture initial bitrate
    float bitrate = 0;
    BASS_ChannelGetAttribute(g_stream, BASS_ATTRIB_BITRATE, &bitrate);
    g_currentBitrate = static_cast<int>(bitrate);

    // Set up tempo processor based on selected algorithm
    TempoAlgorithm algo = static_cast<TempoAlgorithm>(g_tempoAlgorithm);
    SetCurrentAlgorithm(algo);

    // Create or reinitialize tempo processor
    FreeTempoProcessor();
    TempoProcessor* processor = GetTempoProcessor();

    // Restore tempo/pitch settings to processor before initializing
    // Note: Rate is handled via BASS_ATTRIB_FREQ below, not through tempo processor
    processor->SetTempo(g_tempo);
    processor->SetPitch(g_pitch);

    // Initialize processor — this creates g_fxStream, which is what the rest
    // of the app (DSP, recording, position queries) operates on. For
    // SoundTouch this wraps g_stream via BASS_FX_FREESOURCE; for
    // Speedy/Signalsmith it's a fresh BASS_StreamCreate fed from g_stream.
    g_fxStream = processor->Initialize(g_stream, g_originalFreq);
    if (!g_fxStream) {
        // Algorithm init failure (rare — usually a build without USE_SPEEDY
        // or USE_SIGNALSMITH). Fall back to SoundTouch which always compiles.
        if (algo != TempoAlgorithm::SoundTouch) {
            FreeTempoProcessor();
            SetCurrentAlgorithm(TempoAlgorithm::SoundTouch);
            processor = GetTempoProcessor();
            processor->SetTempo(g_tempo);
            processor->SetPitch(g_pitch);
            g_fxStream = processor->Initialize(g_stream, g_originalFreq);
        }

        if (!g_fxStream) {
            BASS_StreamFree(g_stream);
            g_stream = 0;
            g_isLoading = false;
            if (g_playlist.size() <= 1) {
                MessageBoxW(GetMessageBoxOwner(), T("Failed to create tempo stream."), APP_NAME, MB_ICONERROR);
            }
            return false;
        }
    }

    // For SoundTouch, g_stream is now owned by g_fxStream via BASS_FX_FREESOURCE;
    // null our copy so FreeBass/LoadFile cleanup doesn't double-free it.
    // Speedy/Signalsmith keep g_stream alive separately (they pull from it
    // in their own StreamProc), so we leave it set in those cases.
    if (processor->GetAlgorithm() == TempoAlgorithm::SoundTouch) {
        g_stream = 0;
    }

    // Rate is handled separately from tempo (which lives in the processor):
    // it just scales the playback frequency, affecting both speed and pitch.
    // Cheap and lossless, runs on the BASS sample-rate converter.
    if (g_rate != 1.0f) {
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
    }

    ApplyDSPEffects();

    // Set up end sync for auto-advance on output stream
    g_endSync = BASS_ChannelSetSync(g_fxStream, BASS_SYNC_END, 0, OnTrackEnd, nullptr);

    // Restore saved position for this file (if any)
    double savedPos = LoadFilePosition(path);
    if (savedPos > 0) {
        processor->SetPosition(savedPos);
    }

    // Start playback on output stream
    BASS_ChannelPlay(g_fxStream, FALSE);

    // Parse chapters from the ORIGINAL decoder stream. BASS_FX tempo
    // wrappers (used by the SoundTouch algorithm) don't expose ID3v2 /
    // Vorbis tag blocks, so calling ParseChapters on g_fxStream returns
    // nothing. g_sourceStream always holds the original decoder handle
    // regardless of which tempo algorithm wrapped it.
    ParseChapters(g_sourceStream);

    // Engine state: BASS active, hide video window if it was visible
    g_activeEngine = PlaybackEngine::BASS;
    g_isVideoPlaying = false;

    // v2.50 — A-B loop is session-only: a new track starts with no loop.
    // (Auto-advance from OnTrackEnd re-enters here on the UI thread, so this
    // covers track changes too.) ArmLoopSync clears the fresh processor.
    g_loopStart = g_loopEnd = -1.0;
    g_loopEnabled = false;
    ArmLoopSync();
    if (g_videoHwnd && IsWindowVisible(g_videoHwnd)) {
        ShowWindow(g_videoHwnd, SW_HIDE);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 500, 150, SWP_NOMOVE | SWP_NOZORDER);
    }

    g_isLoading = false;
    UpdateWindowTitle();
    UpdateStatusBar();
    return true;
}

// BASS_SYNC_END callback. Runs on a BASS worker thread — we cannot touch
// g_fxStream directly here (BASS forbids freeing streams from inside their
// own sync callback), so we just PostMessage the main thread to advance.
// lParam=1 signals "load the next track but don't auto-play", used when
// auto-advance is disabled to leave the user in a paused state.
void CALLBACK OnTrackEnd(HSYNC handle, DWORD channel, DWORD data, void* user) {
    if (g_autoAdvance || g_repeatMode != 0) {
        PostMessage(g_hwnd, WM_COMMAND, IDM_PLAY_NEXT, 0);
    } else {
        PostMessage(g_hwnd, WM_COMMAND, IDM_PLAY_NEXT, 1);
    }
}

// BASS_SYNC_META callback. Fires when internet radio stations send a new
// ICY StreamTitle (typically a new song). Runs on a BASS thread so we
// route through the message loop to do the actual announce/UI update.

void CALLBACK OnMetaChange(HSYNC handle, DWORD channel, DWORD data, void* user) {
    PostMessage(g_hwnd, WM_META_CHANGED, 0, 0);
}

// Called from main thread when metadata changes - announces new stream track
void AnnounceStreamMetadata() {
    HSTREAM stream = g_stream ? g_stream : g_fxStream;
    if (!stream) return;

    std::string streamTitle = GetStreamTitle(stream);
    if (streamTitle.empty()) return;

    // v2.38 — only announce / record when the ICY title actually changed.
    // Some stations re-send identical metadata repeatedly, which made NVDA
    // announce the current title non-stop and spammed the song history. The
    // dedup is keyed to the stream handle so a new connection (station/track
    // change) re-announces even if the first song matches the previous one.
    static HSTREAM s_lastMetaStream = 0;
    static std::wstring s_lastMetaTitle;
    if (stream != s_lastMetaStream) {
        s_lastMetaStream = stream;
        s_lastMetaTitle.clear();
    }

    // v1.60 — update the now-playing item so the window title shows the
    // freshest song name immediately. Order matters: do this BEFORE the
    // existing UpdateWindowTitle() in main.cpp's WM_META_CHANGED handler
    // gets a chance to render with stale data.
    std::wstring wideTitle = Utf8ToWide(streamTitle);
    SetNowPlayingItem(wideTitle);

    // v1.60 — if the RadioUrl case (ad-hoc URL paste) never got a source
    // name, try to pull the station's icy-name HTTP header now that BASS
    // has it. icy-name is static per connection so we only set if empty.
    if (g_nowPlayingType == SourceType::RadioUrl &&
        g_nowPlayingSource.empty()) {
        std::string stationName = GetStationName(stream);
        if (!stationName.empty()) {
            g_nowPlayingSource = Utf8ToWide(stationName);
            UpdateWindowTitle();
        }
    }

    // Nothing new to announce or record if the title is unchanged.
    if (wideTitle == s_lastMetaTitle) return;
    s_lastMetaTitle = wideTitle;

    // Record to song history (independent of speech setting).
    // v2.11 — store the live station URL as the replayable source so Enter in the
    // history window reconnects the station (the individual past song can't be
    // re-fetched from a live stream; reconnecting the station is the sensible
    // replay). g_playlist[g_currentTrack] is the stream URL here.
    {
        std::wstring src = (g_currentTrack >= 0 &&
                            g_currentTrack < static_cast<int>(g_playlist.size()))
                               ? g_playlist[g_currentTrack] : L"";
        int t = (g_nowPlayingType == SourceType::None)
                    ? static_cast<int>(SourceType::RadioUrl)
                    : static_cast<int>(g_nowPlayingType);
        AddSongHistoryEntry(wideTitle, src, t);
    }

    if (g_speechTrackChange) {
        Speak(streamTitle);
    }
}

// Toggle play/pause. Routes to MPV when video is active. With no stream
// loaded, falls back to Play() which tries to restart the current track or
// the first playlist item. Live streams use Stop() instead of Pause() —
// pausing a live stream would keep the network connection burning data
// in the background.
void PlayPause() {
    if (g_activeEngine == PlaybackEngine::MPV) { MPVPlayPause(); UpdateStatusBar(); return; }
    if (!g_fxStream) {
        // Nothing loaded - try to reload current track or play first
        Play();
        return;
    }

    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_PLAYING) {
        // For live streams, stop instead of pause
        if (g_isLiveStream) {
            Stop();
        } else {
            Pause();
        }
    } else {
        BASS_ChannelPlay(g_fxStream, FALSE);
        UpdateWindowTitle();
        UpdateStatusBar();
    }
}

// Free current stream (used when stopping live streams)
void FreeCurrentStream() {
    // Always tear MPV down if it's active. Removed an early-return that used
    // to skip the BASS cleanup below — after the dual-engine teardown was
    // added to LoadFile/LoadURL, both engines should never be live at the
    // same time, but FreeCurrentStream should still be honest and clean both.
    if (g_activeEngine == PlaybackEngine::MPV) {
        MPVStop();
        if (g_videoHwnd) ShowWindow(g_videoHwnd, SW_HIDE);
        g_isVideoPlaying = false;
        g_activeEngine = PlaybackEngine::None;
    }
    if (g_fxStream) {
        if (g_endSync) {
            BASS_ChannelRemoveSync(g_fxStream, g_endSync);
            g_endSync = 0;
        }
        RemoveDSPEffects();
        BASS_ChannelStop(g_fxStream);
        BASS_StreamFree(g_fxStream);
        g_fxStream = 0;
    }
    if (g_stream) {
        if (g_metaSync) {
            BASS_ChannelRemoveSync(g_stream, g_metaSync);
            g_metaSync = 0;
        }
        if (g_dlSync) {
            BASS_ChannelRemoveSync(g_stream, g_dlSync);
            g_dlSync = 0;
        }
        BASS_StreamFree(g_stream);
        g_stream = 0;
    }
    g_sourceStream = 0;
    g_isLiveStream = false;
    g_currentBitrate = 0;
    FreeTempoProcessor();
}

// Play. If already playing, restarts from position 0. If nothing is loaded
// but there's a current track in the playlist, reloads it first (recovers
// from the case where a live stream was Stop()'d and freed). If even that
// fails, starts the first playlist item.
void Play() {
    if (g_activeEngine == PlaybackEngine::MPV) { MPVPlay(); UpdateStatusBar(); return; }
    if (!g_fxStream) {
        // Nothing loaded - check if we have a current track to reload
        // (This handles the case where a live stream was stopped/freed)
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            const std::wstring& path = g_playlist[g_currentTrack];
            if (IsURL(path.c_str())) {
                LoadURL(path.c_str());
            } else {
                LoadFile(path.c_str());
            }
            if (g_fxStream) {
                BASS_ChannelPlay(g_fxStream, FALSE);
                UpdateWindowTitle();
                UpdateStatusBar();
            }
            return;
        }
        // No current track, try to play first track
        if (!g_playlist.empty()) {
            PlayTrack(0);
        }
        return;
    }

    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_PLAYING) {
        // Already playing - restart from beginning
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            processor->SetPosition(0);
        }
    }
    BASS_ChannelPlay(g_fxStream, FALSE);
    UpdateWindowTitle();
    UpdateStatusBar();
}

// Pause. No-op on live streams (we Speak() an explanation rather than
// silently failing). Optionally rewinds a configurable number of ms on
// pause — handy for users who pause to take notes, so resume starts a bit
// before where they stopped paying attention.
void Pause() {
    if (g_activeEngine == PlaybackEngine::MPV) { MPVPause(); UpdateStatusBar(); return; }
    if (g_fxStream) {
        // Don't allow pausing live streams
        if (g_isLiveStream) {
            Speak(Ts("Cannot pause live stream"));
            return;
        }
        BASS_ChannelPause(g_fxStream);

        if (g_rewindOnPauseMs > 0) {
            Seek(-g_rewindOnPauseMs / 1000.0);
        }

        UpdateWindowTitle();
        UpdateStatusBar();
    }
}

// Stop. For local/seekable content, BASS_ChannelStop + rewind to 0 so the
// next Play() starts from the beginning. For LIVE STREAMS we must free the
// stream entirely — otherwise BASS keeps the network buffer warm and a
// subsequent Stop/Play acts like Pause/Resume, replaying buffered content
// instead of reconnecting to the live feed.
void Stop() {
    // v2.50 — clear the A-B loop on Stop (covers both the MPV and BASS
    // branches). ArmLoopSync is null-safe if no processor is active.
    g_loopStart = g_loopEnd = -1.0;
    g_loopEnabled = false;
    ArmLoopSync();

    if (g_activeEngine == PlaybackEngine::MPV) {
        MPVStop();
        if (g_videoHwnd) ShowWindow(g_videoHwnd, SW_HIDE);
        g_isVideoPlaying = false;
        g_activeEngine = PlaybackEngine::None;
        SetWindowPos(g_hwnd, nullptr, 0, 0, 500, 150, SWP_NOMOVE | SWP_NOZORDER);
        // v1.86 — preserve NowPlaying on a video Stop, same as a local-audio
        // Stop in v1.85. Play() will reload the file via LoadFile if the
        // user resumes, and either way the window title stays meaningful
        // (the file is still selected in the playlist).
        UpdateStatusBar();
        return;
    }
    if (g_fxStream) {
        // For live streams, free the stream entirely to disconnect
        // (otherwise BASS buffers it and stop/play acts like pause/resume)
        if (g_isLiveStream) {
            FreeCurrentStream();
            ClearNowPlaying();  // v1.60 — live: nothing to come back to
        } else {
            BASS_ChannelStop(g_fxStream);
            TempoProcessor* processor = GetTempoProcessor();
            if (processor && processor->IsActive()) {
                processor->SetPosition(0);
            }
            // v1.85 — preserve NowPlaying on a local-stream Stop so that a
            // subsequent Play() (which re-uses g_fxStream from position 0)
            // keeps the title visible in the window. Sèb: "Stop then Play"
            // used to leave the window title as a bare "MediaAccess" because
            // Play() did not call SetNowPlaying and Stop() had wiped it.
        }
    } else {
        ClearNowPlaying();  // v1.60 — no stream loaded: safe to clear
    }
    UpdateStatusBar();
}

// Seek `seconds` (signed) relative to current position. Clamped to [0, len].
// On the MPV path we have to read pos/length BEFORE issuing the seek (MPV
// seeks are async — querying after MPVSeek may return the pre-seek position
// for a few hundred ms) so the position announcement reports the target,
// not the stale current.
// Position announcement is gated by g_speechSeekPosition (Options).
void Seek(double seconds) {
    if (g_activeEngine == PlaybackEngine::MPV) {
        // v1.64 — capture the target position so we can announce it
        // even though MPVSeek may not have applied yet by the time we
        // call MPVGetPosition (mpv seek is async).
        double cur = MPVGetPosition();
        double len = MPVGetLength();
        double target = cur + seconds;
        if (target < 0) target = 0;
        if (len > 0 && target > len) target = len;
        MPVSeek(seconds);
        UpdateStatusBar();
        if (g_speechSeekPosition) SpeakW(FormatTime(target));  // v1.65 gate
        return;
    }
    if (!g_fxStream || g_isBusy || g_isLoading) return;

    // Verify stream is still valid
    DWORD state = BASS_ChannelIsActive(g_fxStream);
    if (state == BASS_ACTIVE_STOPPED && BASS_ErrorGetCode() == BASS_ERROR_HANDLE) {
        g_fxStream = 0;
        g_stream = 0;
        return;
    }

    // Use tempo processor for position handling
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;

    double length = processor->GetLength();
    if (length <= 0) return;  // Invalid or unknown length

    double currentPos = processor->GetPosition();
    double newPos = currentPos + seconds;
    if (newPos < 0) newPos = 0;
    if (newPos > length) newPos = length;

    processor->SetPosition(newPos);

    UpdateStatusBar();
    // v1.64 — announce the new position so NVDA users don't have to
    // press NVDA+End after every left/right press. Speak() defaults to
    // interrupt=true so rapid repeated seeks coalesce to the last one.
    // v1.65 — gated by Options > Speech > "Announce position after seek".
    if (g_speechSeekPosition) SpeakW(FormatTime(newPos));
}

// Seek by tracks (positive = forward, negative = backward)
void SeekTracks(int tracks) {
    if (g_playlist.empty() || g_isBusy) return;

    int newTrack = g_currentTrack + tracks;
    if (newTrack < 0) newTrack = 0;
    if (newTrack >= static_cast<int>(g_playlist.size())) {
        newTrack = static_cast<int>(g_playlist.size()) - 1;
    }

    if (newTrack != g_currentTrack) {
        PlayTrack(newTrack);
    }
}

// v1.79 — Spring's "skip exactly N seconds with one keystroke" path.
// Mirrors the IDM_PLAY_SEEKBACK / IDM_PLAY_SEEKFWD handler in main.cpp but
// takes the unit from a fixed index instead of g_currentSeekIndex. Indices:
//   0..8   → g_seekAmounts[0..8]  (1 s, 5 s, 10 s, 30 s, 1 m, 5 m, 10 m,
//                                  30 m, 1 h)
//   9..11  → g_seekAmounts[9..11] (1, 5, 10 tracks)
//   12     → "1 chapter"
// direction is -1 (backward) or +1 (forward); anything else is treated as +1.
// Fallback rules — kept identical to the legacy seek so existing user habits
// keep working:
//   * Track jump when playlist has <= 1 entry  → fall back to the first
//     enabled time unit in g_seekAmounts (Spring requested explicit units
//     anyway, but the fallback still helps if the playlist later drops to
//     a single file).
//   * Chapter jump when no chapters are parsed → silent no-op.
// Daisy/EPUB books reuse DaisySeekRelative for time jumps; chapter and
// track jumps no-op while a book is active (the Books category already
// owns Shift+arrows for nav).
void PerformGranularSeek(int unitIdx, int direction) {
    if (unitIdx < 0 || unitIdx > 12) return;
    int dir = (direction < 0) ? -1 : +1;

    // Chapter jump (unit 12).
    if (unitIdx == 12) {
        if (mediaaccess::DaisyIsActive()) return;  // Books own their nav.
        if (g_chapters.empty()) return;            // Silent no-op per design.
        if (dir > 0) SeekToNextChapter();
        else         SeekToPrevChapter();
        return;
    }

    // Track jumps (units 9..11 → 1/5/10 tracks).
    if (unitIdx >= 9 && unitIdx <= 11) {
        if (mediaaccess::DaisyIsActive()) return;
        int trackStep = static_cast<int>(g_seekAmounts[unitIdx].value);
        if (g_playlist.size() <= 1) {
            // Fallback to first enabled time unit, same as legacy SEEKBACK/FWD.
            for (int i = 0; i < g_seekAmountCount; ++i) {
                if (g_seekEnabled[i] && !g_seekAmounts[i].isTrack) {
                    Seek(dir * g_seekAmounts[i].value);
                    return;
                }
            }
            return;  // No enabled time unit either — nothing to do.
        }
        SeekTracks(dir * trackStep);
        return;
    }

    // Time jumps (units 0..8).
    double secs = g_seekAmounts[unitIdx].value;
    if (mediaaccess::DaisyIsActive()) {
        mediaaccess::DaisySeekRelative(dir * secs);
        return;
    }
    Seek(dir * secs);
}

// v2.50 — push the current A-B loop region into the active tempo processor,
// which owns the per-algorithm seamless wrap. Null-safe: if no processor is
// active (e.g. MPV/video or stopped), SetLoopRegion is simply not called.
void ArmLoopSync() {
    TempoProcessor* p = GetTempoProcessor();
    if (p) p->SetLoopRegion(g_loopStart, g_loopEnd, g_loopEnabled);
}

// Seek to absolute position in seconds. announce=false suppresses the spoken
// position (used by the A-B loop wrap so it never talks over the loop message).
void SeekToPosition(double seconds, bool announce) {
    if (g_activeEngine == PlaybackEngine::MPV) {
        double len = MPVGetLength();
        double target = seconds;
        if (target < 0) target = 0;
        if (len > 0 && target > len) target = len;
        MPVSeekToPosition(seconds);
        UpdateStatusBar();
        if (announce && g_speechSeekPosition) SpeakW(FormatTime(target));  // v1.64/65
        return;
    }
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;

    double duration = processor->GetLength();
    if (seconds < 0) seconds = 0;
    if (seconds > duration) seconds = duration;

    processor->SetPosition(seconds);
    UpdateStatusBar();
    if (announce && g_speechSeekPosition) SpeakW(FormatTime(seconds));  // v1.64/65
}

// Get current playback position in seconds
double GetCurrentPosition() {
    if (g_activeEngine == PlaybackEngine::MPV) return MPVGetPosition();
    if (!g_fxStream) return 0.0;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return 0.0;
    return processor->GetPosition();
}

// Get index of current chapter based on playback position (-1 if no chapters)
int GetCurrentChapterIndex() {
    if (g_chapters.empty()) return -1;

    double pos = GetCurrentPosition();
    int currentChapter = -1;

    // Find the last chapter whose position is <= current position
    for (size_t i = 0; i < g_chapters.size(); i++) {
        if (g_chapters[i].position <= pos) {
            currentChapter = static_cast<int>(i);
        } else {
            break;  // Chapters are sorted
        }
    }
    return currentChapter;
}

// v2.34 — chapter vs. cue-track announcement label. When the current chapters
// were injected from a .cue sheet, the navigation announcements say "Track N"
// instead of "Chapter N". Localized via the existing Ts() table.
static std::string ChapterLabel() {
    return g_chaptersAreCueTracks ? Ts("Track ") : Ts("Chapter ");
}

// Seek to next chapter (returns true if successful)
bool SeekToNextChapter() {
    if (g_chapters.empty() || !g_fxStream) return false;

    double pos = GetCurrentPosition();

    // Find the first chapter after current position (with small tolerance for current chapter)
    for (size_t i = 0; i < g_chapters.size(); i++) {
        if (g_chapters[i].position > pos + 0.5) {  // 0.5s tolerance
            SeekToPosition(g_chapters[i].position);

            // Announce chapter name
            if (!g_chapters[i].name.empty()) {
                Speak(ChapterLabel() + std::to_string(i + 1) + ": " + WideToUtf8(g_chapters[i].name));
            } else {
                Speak(ChapterLabel() + std::to_string(i + 1));
            }
            return true;
        }
    }
    return false;  // Already at or past last chapter
}

// Seek to previous chapter (returns true if successful)
bool SeekToPrevChapter() {
    if (g_chapters.empty() || !g_fxStream) return false;

    double pos = GetCurrentPosition();

    // Find the chapter before current position
    // If we're more than 3 seconds into a chapter, go to its start
    // Otherwise go to the previous chapter
    int currentChapter = GetCurrentChapterIndex();

    if (currentChapter < 0) {
        // Before first chapter, go to start
        SeekToPosition(0);
        return true;
    }

    double chapterStart = g_chapters[currentChapter].position;

    if (pos - chapterStart > 3.0 && currentChapter >= 0) {
        // More than 3 seconds into current chapter - restart it
        SeekToPosition(chapterStart);
        if (!g_chapters[currentChapter].name.empty()) {
            Speak(ChapterLabel() + std::to_string(currentChapter + 1) + ": " + WideToUtf8(g_chapters[currentChapter].name));
        } else {
            Speak(ChapterLabel() + std::to_string(currentChapter + 1));
        }
        return true;
    } else if (currentChapter > 0) {
        // Go to previous chapter
        int prevChapter = currentChapter - 1;
        SeekToPosition(g_chapters[prevChapter].position);
        if (!g_chapters[prevChapter].name.empty()) {
            Speak(ChapterLabel() + std::to_string(prevChapter + 1) + ": " + WideToUtf8(g_chapters[prevChapter].name));
        } else {
            Speak(ChapterLabel() + std::to_string(prevChapter + 1));
        }
        return true;
    } else {
        // At first chapter, go to start
        SeekToPosition(0);
        Speak(Ts("Beginning"));
        return true;
    }
}

// v2.43 (Feature #9, Romain) — jump to the previous/next MEDIA bookmark of the
// current file relative to the playback position. Book bookmarks use a separate
// clip/offset model and are out of scope. We seek IN PLACE (not JumpToBookmark,
// which switches tracks). The "Bookmark N of M" announcement is distinct from
// the optional seek-timecode SeekToPosition may speak, so they complement rather
// than duplicate. Works for BASS and MPV (GetCurrentPosition/SeekToPosition both
// branch on the engine) — no g_fxStream guard.
static std::vector<Bookmark> BookmarksForCurrentFileSorted() {
    std::vector<Bookmark> v;
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) return v;
    const std::wstring& cur = g_playlist[g_currentTrack];
    // GetAllBookmarks() is ordered by creation (DESC), across ALL files — filter
    // to the current file, then sort ascending by position.
    for (const auto& bm : GetAllBookmarks()) {
        if (_wcsicmp(bm.filePath.c_str(), cur.c_str()) == 0) v.push_back(bm);
    }
    std::sort(v.begin(), v.end(),
              [](const Bookmark& a, const Bookmark& b) { return a.position < b.position; });
    return v;
}

void SeekToNextBookmark() {
    std::vector<Bookmark> v = BookmarksForCurrentFileSorted();
    if (v.empty()) { Speak(Ts("No bookmarks")); return; }
    double pos = GetCurrentPosition();
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].position > pos + 0.5) {  // 0.5s tolerance, like SeekToNextChapter
            SeekToPosition(v[i].position);
            Speak(Ts("Bookmark ") + std::to_string(i + 1) + " " + Ts("of ") + std::to_string(v.size()));
            return;
        }
    }
    Speak(Ts("No next bookmark"));  // announce and stay, no wrap
}

void SeekToPrevBookmark() {
    std::vector<Bookmark> v = BookmarksForCurrentFileSorted();
    if (v.empty()) { Speak(Ts("No bookmarks")); return; }
    double pos = GetCurrentPosition();
    for (int i = (int)v.size() - 1; i >= 0; i--) {
        if (v[i].position < pos - 0.5) {
            SeekToPosition(v[i].position);
            Speak(Ts("Bookmark ") + std::to_string(i + 1) + " " + Ts("of ") + std::to_string(v.size()));
            return;
        }
    }
    Speak(Ts("No previous bookmark"));  // announce and stay, no wrap
}

// Set volume (0.0 - 1.0)
// Volume is applied via DSP (not BASS_ATTRIB_VOL) so recording captures full volume
// Unless legacy mode is enabled, which uses BASS_ATTRIB_VOL (faster but affects recordings)
void SetVolume(float vol) {
    float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > maxVol) vol = maxVol;
    g_volume = vol;

    // Propagate to MPV if active. v2.44 — via ApplyVideoVolume so a volume change
    // during a spoken subtitle preserves the duck instead of jumping to full.
    if (g_activeEngine == PlaybackEngine::MPV) {
        ApplyVideoVolume();
    }

    // Propagate to DAISY book stream if a book is loaded
    if (mediaaccess::DaisyIsActive()) {
        mediaaccess::DaisyApplyVolume();
    }

    // In legacy mode, use BASS_ATTRIB_VOL (faster but affects recordings)
    // In normal mode, volume DSP automatically uses updated g_volume
    if (g_legacyVolume && g_fxStream) {
        // v2.52 — fold in the caption duck so a volume change during a spoken
        // caption keeps the music attenuated (mirrors the MPV ApplyVideoVolume fix).
        float curvedVolume = (vol * vol) * g_subtitleBassDuck.load(std::memory_order_relaxed);
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
    }

    // Announce volume if setting enabled
    if (g_speechVolume) {
        char buf[64];
        snprintf(buf, sizeof(buf), Ts("Volume %d%%").c_str(), static_cast<int>(g_volume * 100 + 0.5f));
        Speak(buf);
    }

    UpdateStatusBar();
}

// Toggle mute (recording captures full volume since mute is applied via DSP)
// In legacy mode, mute uses BASS_ATTRIB_VOL (faster but affects recordings)
void ToggleMute() {
    g_muted = !g_muted;

    // Propagate to MPV if active
    if (g_activeEngine == PlaybackEngine::MPV) {
        MPVSetMute(g_muted);
    }

    // Propagate to DAISY book stream if a book is loaded
    if (mediaaccess::DaisyIsActive()) {
        mediaaccess::DaisyApplyVolume();
    }

    // In legacy mode, use BASS_ATTRIB_VOL
    if (g_legacyVolume && g_fxStream) {
        if (g_muted) {
            BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, 0.0f);
        } else {
            // v2.52 — preserve the caption duck on unmute mid-caption.
            float curvedVolume = (g_volume * g_volume) * g_subtitleBassDuck.load(std::memory_order_relaxed);
            BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
        }
    }
    // In normal mode, volume DSP automatically uses updated g_muted

    Speak(g_muted ? Ts("Muted") : Ts("Unmuted"));
    UpdateStatusBar();
}

// Combined tempo + rate multiplier. BASS tempo is a ±% (e.g. +100 = 2x
// faster, -50 = half speed) and BASS rate is a direct multiplier on the
// sample rate. The effective speed at which audio actually plays is the
// product of the two. Clamped to a small positive minimum so callers can
// safely divide by it without checking for zero.
double GetEffectivePlaybackSpeed() {
    double tempoFactor = 1.0 + (double)g_tempo / 100.0;
    double speed = tempoFactor * (double)g_rate;
    if (speed < 0.01) speed = 0.01;
    return speed;
}

// Speak elapsed time — reported in real wall-clock seconds (divides the
// source-content position by the effective playback speed).
void SpeakElapsed() {
    // v2.13 — MPV video has no g_fxStream/TempoProcessor; the elapsed/remaining/
    // total shortcuts (Ctrl+Shift+E/R/T) used to silently do nothing on videos.
    // Route to the MPV clock so they work for video too. Reported by Sèb.
    if (g_activeEngine == PlaybackEngine::MPV) {
        double pos = MPVGetPosition();
        if (pos < 0) pos = 0;
        Speak(WideToUtf8(FormatTime(pos)));
        return;
    }
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double pos = processor->GetPosition() / GetEffectivePlaybackSpeed();
    std::wstring posStr = FormatTime(pos);
    Speak(WideToUtf8(posStr));
}

// Speak remaining time — adjusted for current playback speed.
void SpeakRemaining() {
    if (g_activeEngine == PlaybackEngine::MPV) {
        double pos = MPVGetPosition();
        double len = MPVGetLength();
        double remaining = len - pos;
        if (remaining < 0) remaining = 0;
        Speak(WideToUtf8(FormatTime(remaining)));
        return;
    }
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double speed = GetEffectivePlaybackSpeed();
    double pos = processor->GetPosition() / speed;
    double len = processor->GetLength()  / speed;
    double remaining = len - pos;
    if (remaining < 0) remaining = 0;
    std::wstring remStr = FormatTime(remaining);
    Speak(WideToUtf8(remStr));
}

// Speak total time — adjusted for current playback speed, so a 33-min
// file at 3x is announced as 11 min.
void SpeakTotal() {
    if (g_activeEngine == PlaybackEngine::MPV) {
        double len = MPVGetLength();
        if (len < 0) len = 0;
        Speak(WideToUtf8(FormatTime(len)));
        return;
    }
    if (!g_fxStream) return;
    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) return;
    double len = processor->GetLength() / GetEffectivePlaybackSpeed();
    std::wstring lenStr = FormatTime(len);
    Speak(WideToUtf8(lenStr));
}

// Play the playlist track at `index`. On load failure, walks forward up to
// 10 entries skipping broken files so a single corrupt track doesn't trap
// the user. After load, refreshes the Now Playing infrastructure (window
// title + speech announcement) and optionally announces the new track via
// TTS if g_speechTrackChange is on.
//
// autoPlay=false loads and immediately pauses — used by the BASS_SYNC_END
// callback when auto-advance is disabled, so the next track is queued but
// playback halts.
//
// v2.11 — shared now-playing/title assignment for the current playlist entry.
// Extracted from PlayTrack so the startup state-restore path (LoadPlaybackState,
// which opens via LoadFile and never calls PlayTrack) gives the window title and
// the now-playing-on-focus announcement the same state a normal play would.
// Local file -> SourceType::Local + tag title / filename fallback (localized
// placeholder guard). URL -> RadioUrl with empty source UNLESS a typed source
// (radio favourite / podcast / YouTube) was already preset by the caller, in
// which case we only refresh the title from the tags/ICY we have now.
void ApplyNowPlayingForCurrentTrack() {
    if (g_currentTrack < 0 ||
        g_currentTrack >= static_cast<int>(g_playlist.size())) {
        return;
    }
    const std::wstring& path = g_playlist[g_currentTrack];
    bool isUrl = IsURL(path.c_str());
    if (!isUrl) {
        // Local audio file: source is "(Local)" (resolved at display time via
        // T()), item is the tag title or filename fallback. Compare against the
        // LOCALISED placeholders (T()), not the English literals, so the French
        // build does not show "Aucune lecture en cours" as the item (v1.78).
        std::wstring item = GetTagTitle();
        if (item.empty() ||
            item == L"No title" ||
            item == L"Nothing playing" ||
            item == T("No title") ||
            item == T("Nothing playing")) {
            item = GetFileNameNoExt(path);
        }
        SetNowPlaying(SourceType::Local, L"", item);
    } else {
        // URL: if no typed source was preset, fall back to RadioUrl with an
        // empty source (icy-name fills in later via WM_META_CHANGED).
        if (g_nowPlayingType == SourceType::None ||
            g_nowPlayingType == SourceType::Local) {
            SetNowPlaying(SourceType::RadioUrl, L"", L"");
        } else {
            UpdateWindowTitle();
        }
    }

    // v2.11 (issue #3) — record this item into the play history. This is the
    // reliable chokepoint for every PlayTrack-based source (local, video, radio,
    // podcast): g_currentTrack and g_playlist are valid here, so the playlist
    // entry IS the replayable target. Books never reach PlayTrack (DAISY player),
    // and YouTube is recorded from its own layer (it doesn't use g_playlist).
    if (g_nowPlayingType != SourceType::None &&
        g_nowPlayingType != SourceType::Book &&
        g_nowPlayingType != SourceType::YouTube) {
        std::wstring title = !g_nowPlayingItem.empty()   ? g_nowPlayingItem
                           : !g_nowPlayingSource.empty() ? g_nowPlayingSource
                           : GetFileName(path);
        AddSongHistoryEntry(title, path, static_cast<int>(g_nowPlayingType));
    }
}

// g_isBusy gate prevents re-entrancy from rapid Next/Prev presses.
void PlayTrack(int index, bool autoPlay) {
    if (g_isBusy) return;  // Prevent re-entrancy
    if (index < 0 || index >= static_cast<int>(g_playlist.size())) {
        return;
    }

    g_isBusy = true;

    // Save position of current track before switching
    if (g_fxStream && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        SaveFilePosition(g_playlist[g_currentTrack]);
    }

    // Try to load tracks, skipping failures (up to 10 attempts to prevent infinite loop)
    int attempts = 0;
    bool loadedSuccessfully = false;
    while (index < static_cast<int>(g_playlist.size()) && attempts < 10) {
        g_currentTrack = index;
        if (LoadFile(g_playlist[index].c_str())) {
            loadedSuccessfully = true;
            // Add to recent files (only for local files, not URLs)
            AddToRecentFiles(g_playlist[index]);
            break;  // Success
        }
        // Try next track if this one failed and we have multiple files
        if (g_playlist.size() > 1) {
            index++;
            attempts++;
        } else {
            break;  // Single file, don't loop
        }
    }

    // If autoPlay is false, pause immediately after loading
    if (loadedSuccessfully && !autoPlay && g_fxStream) {
        BASS_ChannelPause(g_fxStream);
    }

    // Notify playlist dialog about track change
    if (loadedSuccessfully) {
        NotifyPlaylistTrackChanged();

        // v1.60 — Now-playing infrastructure. Source-typed callers (radio
        // favourite, podcast, YouTube) call SetNowPlaying() BEFORE
        // PlayTrack so g_nowPlayingType is already set. For local files
        // the callers don't bother — PlayTrack defaults to Local here
        // (and overrides any stale RadioFavorite/Podcast/YouTube state
        // left over from the previous track). v2.11 — extracted into a
        // shared helper so the startup state-restore path (which loads via
        // LoadFile, not PlayTrack) sets the same now-playing/title state.
        ApplyNowPlayingForCurrentTrack();
    }

    // Announce track change if setting is enabled
    if (loadedSuccessfully && g_speechTrackChange) {
        // For streams, announce the stream title; for files, announce title or filename
        HSTREAM stream = g_stream ? g_stream : g_fxStream;
        if (stream) {
            std::string streamTitle = GetStreamTitle(stream);
            if (!streamTitle.empty()) {
                Speak(streamTitle);
            } else {
                std::string title = GetMetadataTag(stream, "TITLE");
                std::string artist = GetMetadataTag(stream, "ARTIST");

                // Try ID3v1 if nothing found
                if (title.empty()) {
                    const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
                    if (id3) {
                        title = GetTrimmedTag(id3->title, 30);
                        if (artist.empty()) artist = GetTrimmedTag(id3->artist, 30);
                    }
                }

                if (!title.empty() && !artist.empty()) {
                    Speak(artist + " - " + title);
                } else if (!title.empty()) {
                    Speak(title);
                } else {
                    // Fall back to filename
                    std::wstring path = g_playlist[g_currentTrack];
                    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
                    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');
                    std::wstring filename = lastSlash ? (lastSlash + 1) : path;
                    char buf[512];
                    WideCharToMultiByte(CP_UTF8, 0, filename.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
                    Speak(buf);
                }
            }
        }
    }

    g_isBusy = false;
}

// Advance to the next track in the playlist, respecting shuffle and repeat
// modes. Repeat-one restarts the current track. Repeat-all wraps from the
// last entry back to the first. No-repeat stops playback at the end.
// Shuffle picks a random entry other than the current one.
void NextTrack(bool autoPlay) {
    if (g_playlist.empty() || g_isBusy) return;

    // Repeat one: restart current track
    if (g_repeatMode == 1 && g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
        PlayTrack(g_currentTrack, autoPlay);
        return;
    }

    int next;
    if (g_shuffle && g_playlist.size() > 1) {
        // Random track (excluding current)
        do {
            next = rand() % static_cast<int>(g_playlist.size());
        } while (next == g_currentTrack && g_playlist.size() > 1);
    } else {
        next = g_currentTrack + 1;
        if (next >= static_cast<int>(g_playlist.size())) {
            // Repeat all: wrap to start
            if (g_repeatMode == 2) {
                next = 0;
            } else {
                // End of playlist - stop
                Stop();
                return;
            }
        }
    }
    PlayTrack(next, autoPlay);
}

// Cycle repeat mode: off -> repeat one -> repeat all -> off
void ToggleRepeatMode() {
    g_repeatMode = (g_repeatMode + 1) % 3;
    const std::string names[] = {Ts("Repeat off"), Ts("Repeat track"), Ts("Repeat all")};
    Speak(names[g_repeatMode]);
    SaveSettings();
}

// "Previous" behaviour matches every other media player: if we're > 3
// seconds into the current track, jump to the start of THIS track;
// otherwise actually go to the prior track. Most users expect this even
// though "Previous" sounds like it should always step backwards.
void PrevTrack() {
    if (g_playlist.empty() || g_isBusy) return;

    // If we're more than 3 seconds in, restart current track
    if (g_fxStream) {
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            double pos = processor->GetPosition();
            if (pos > 3.0) {
                processor->SetPosition(0);
                UpdateStatusBar();
                return;
            }
        }
    }

    int prev = g_currentTrack - 1;
    if (prev < 0) prev = 0;
    PlayTrack(prev);
}

// Tear down BASS and re-init it on a different output device.
// Used by the audio-device popup (Help → Audio device) so users can switch
// outputs without restarting the app.
//
// Process: save playback state → free FX/stream/processor → BASS_Free →
// BASS_Init on new device → reload the same file → restore position and
// the previous play/pause state. Falls back to the default device if the
// requested one fails.
bool ReinitBass(int device) {
    // Save current state
    bool wasPlaying = g_fxStream && (BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PLAYING);
    bool wasPaused = g_fxStream && (BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PAUSED);
    double position = 0;
    std::wstring currentFile;

    if (g_fxStream) {
        // Use tempo processor to get position
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            position = processor->GetPosition();
        }
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            currentFile = g_playlist[g_currentTrack];
        }
        // Remove DSP effects before freeing stream (resets handles to 0)
        RemoveDSPEffects();
        // Free tempo processor before freeing BASS
        FreeTempoProcessor();
        if (g_fxStream) {
            BASS_StreamFree(g_fxStream);
            g_fxStream = 0;
        }
        if (g_stream) {
            BASS_StreamFree(g_stream);
            g_stream = 0;
        }
    }

    BASS_Free();

    if (!BASS_Init(device, 44100, 0, g_hwnd, nullptr)) {
        // Try default device as fallback
        if (device != -1) {
            if (BASS_Init(-1, 44100, 0, g_hwnd, nullptr)) {
                g_selectedDevice = -1;
                g_selectedDeviceName.clear();
            }
        }
        return false;
    }

    g_selectedDevice = device;
    // Update device name from the actual device
    g_selectedDeviceName = GetDeviceName(device);

    // Restore playback state if we had a file loaded
    if (!currentFile.empty()) {
        LoadFile(currentFile.c_str());
        if (g_fxStream) {
            // Use tempo processor to set position
            TempoProcessor* processor = GetTempoProcessor();
            if (processor && processor->IsActive()) {
                processor->SetPosition(position);
            }
            // LoadFile() auto-starts playback, so we need to pause/stop if we weren't playing
            if (!wasPlaying) {
                if (wasPaused) {
                    BASS_ChannelPause(g_fxStream);
                } else {
                    // Was stopped - pause the stream (stop would reset position)
                    BASS_ChannelPause(g_fxStream);
                }
            }
            UpdateWindowTitle();
            UpdateStatusBar();
        }
    }

    return true;
}

// =============================================================================
// v2.32 — automatic output-device re-routing (GitHub issue #7).
//
// When Windows switches the default render endpoint (headphones unplugged, a
// device disabled/enabled, etc.), the audio_device_watcher posts
// WM_AUDIO_DEVICE_CHANGED. The UI thread coalesces the burst and then calls
// HandleAudioDeviceChange() below, which decides whether/where to reroute and
// reuses the proven ReinitBass() path. Default-ON; opt out via
// g_autoFollowDevice (Options → Playback).
// =============================================================================

// Re-entrancy guard. HandleAudioDeviceChange tears down and re-inits BASS; a
// nested call during that window would corrupt state. Always reset on exit
// (the RAII guard below enforces this).
static bool g_deviceRerouteInProgress = false;

// v2.33 — identity (lowercased GUID tail) of the default render endpoint we are
// currently bound to. The watcher fires on many endpoint notifications that do
// NOT actually move the default render device (communications-role changes from
// other apps, unrelated device-state flips). We only reroute + announce when this
// identity TRULY changes, which kills the spurious "Audio device changed" reports.
// UI-thread-only (HandleAudioDeviceChange + SeedAudioRerouteBaseline) — no lock.
static std::string g_lastDefaultRenderTail;
static bool        g_lastDefaultTailSeeded = false;

// v2.33 — capture the boot-time default-endpoint baseline (called from WM_CREATE
// after the watcher starts) so startup is silent and the first REAL change is
// still detected. seeded stays true even if the tail is "" (no device yet).
void SeedAudioRerouteBaseline() {
    g_lastDefaultRenderTail = mediaaccess::CurrentDefaultRenderEndpointTail();
    g_lastDefaultTailSeeded = true;
}

// Reroute to targetIndex via ReinitBass. When `transient` is true the device
// loss is treated as temporary (e.g. a named device vanished and we fell back
// to the system default): the user's persisted device NAME is restored so that
// when the named device reappears we reconnect to it. Never calls SaveSettings.
static bool RerouteAudio(int targetIndex, bool transient) {
    std::wstring savedName = g_selectedDeviceName;
    bool ok = ReinitBass(targetIndex);
    if (transient) {
        g_selectedDeviceName = savedName;
        g_selectedDevice = FindDeviceByName(savedName);
    }
    return ok;
}

void HandleAudioDeviceChange() {
    // Feature opt-out.
    if (!g_autoFollowDevice) return;

    // Re-entrancy guard with guaranteed reset on every return path.
    if (g_deviceRerouteInProgress) return;
    struct RerouteGuard {
        ~RerouteGuard() { g_deviceRerouteInProgress = false; }
    } guard;
    g_deviceRerouteInProgress = true;

    // Pure-video MPV session: MPV manages its own audio output and auto-follows
    // the default device, so there is nothing for BASS to reroute. (Above the
    // tail resolve below so a pure-video session never queries Core Audio.)
    if (!g_fxStream && g_activeEngine == PlaybackEngine::MPV) return;

    // v2.33 — resolve the CURRENT default render endpoint identity ONCE (UI
    // thread). Empty "" means no resolvable device.
    std::string nowTail = mediaaccess::CurrentDefaultRenderEndpointTail();

    // Recording in progress (legacy or system capture): defer — tearing down
    // BASS mid-recording would break the capture. Announce ONLY if the default
    // endpoint actually changed, and do NOT update the stored id: we did not
    // reroute, so we are still logically bound to the OLD endpoint; keeping the
    // old id means the deferred change is reconciled once recording stops.
    if (g_isRecording || mediaaccess::IsSystemCapturing()) {
        if (!nowTail.empty() && (!g_lastDefaultTailSeeded || nowTail != g_lastDefaultRenderTail)) {
            Speak(Ts("Audio device changed; recording in progress"));
        }
        return;
    }

    // BASS_GetDevice() returns the current device index, or 0xFFFFFFFF on error.
    DWORD curRaw = BASS_GetDevice();
    int current = (curRaw == (DWORD)-1) ? -1 : static_cast<int>(curRaw);

    const std::wstring& savedName = g_selectedDeviceName;
    bool userOnDefault = savedName.empty() || savedName == L"Default" || g_selectedDevice <= 1;

    if (userOnDefault) {
        // CORE FIX: only reroute + announce when the default render endpoint
        // TRULY changed. A non-empty tail equal to the stored one means the
        // notification was chatter (communications role, unrelated state flip) —
        // do nothing: no BASS reinit, no spurious announcement.
        if (g_lastDefaultTailSeeded && !nowTail.empty() && nowTail == g_lastDefaultRenderTail) {
            return;
        }
        RerouteAudio(-1, false);
        if (!nowTail.empty()) { g_lastDefaultRenderTail = nowTail; g_lastDefaultTailSeeded = true; }
        Speak(Ts("Audio device changed"));
        return;
    }

    // User selected a specific named device.
    int idx = FindDeviceByName(savedName);

    if (idx >= 0 && idx == current) {
        // Named device is still present and still the one we are playing on.
        // Record the observed default so a later switch back to default measures
        // from a fresh baseline.
        if (!nowTail.empty()) { g_lastDefaultRenderTail = nowTail; g_lastDefaultTailSeeded = true; }
        return;
    }

    if (idx >= 0) {
        // The named device exists (again) but we are not on it — e.g. it
        // disappeared earlier and we fell back to default, and it just came
        // back. Reconnect to it.
        if (RerouteAudio(idx, false)) {
            if (!nowTail.empty()) { g_lastDefaultRenderTail = nowTail; g_lastDefaultTailSeeded = true; }
            Speak(Ts("Reconnected to device"));
        } else {
            Speak(Ts("No audio device available"));
        }
        return;
    }

    // idx == -1: the named device has vanished. Fall back to the default device
    // but keep the persisted name so we reconnect when it returns (transient).
    if (RerouteAudio(-1, true)) {
        if (!nowTail.empty()) { g_lastDefaultRenderTail = nowTail; g_lastDefaultTailSeeded = true; }
        Speak(Ts("Audio device disconnected, switched to default device"));
    } else {
        Speak(Ts("No audio device available"));
    }
}

// ID3v1 genre names table
static const char* g_id3Genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
    "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
    "Rap", "Reggae", "Rock", "Techno", "Industrial", "Alternative", "Ska",
    "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
    "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise", "AlternRock",
    "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop",
    "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy",
    "Cult", "Gangsta", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychadelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro",
    "Musical", "Rock & Roll", "Hard Rock"
};
static const int g_id3GenreCount = sizeof(g_id3Genres) / sizeof(g_id3Genres[0]);

// Helper: Get a tag value from null-terminated string list (OGG, APE, MP4, etc.)
static std::string GetTagFromList(const char* tags, const char* key) {
    if (!tags || !key) return "";

    size_t keyLen = strlen(key);
    const char* p = tags;

    while (*p) {
        // Check if this line starts with key= (case insensitive)
        if (_strnicmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            return std::string(p + keyLen + 1);
        }
        p += strlen(p) + 1;  // Move to next string
    }
    return "";
}

// Helper: Parse ID3v2 text frame (handles encoding byte)
static std::string ParseID3v2TextFrame(const unsigned char* data, size_t size) {
    if (!data || size < 1) return "";

    unsigned char encoding = data[0];
    const unsigned char* text = data + 1;
    size_t textLen = size - 1;

    if (textLen == 0) return "";

    if (encoding == 0) {
        // ISO-8859-1 (Latin-1) - convert to UTF-8
        // Remove trailing nulls (single-byte encoding)
        while (textLen > 0 && text[textLen - 1] == 0) textLen--;
        if (textLen == 0) return "";

        std::wstring wstr;
        for (size_t i = 0; i < textLen; i++) {
            wstr += static_cast<wchar_t>(text[i]);
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return std::string((const char*)text, textLen);
    } else if (encoding == 1) {
        // UTF-16 with BOM
        if (textLen < 2) return "";
        bool bigEndian = (text[0] == 0xFE && text[1] == 0xFF);
        const unsigned char* textStart = text + 2;
        size_t byteCount = textLen - 2;

        // Remove trailing null WORDS (2 bytes at a time for UTF-16)
        while (byteCount >= 2 && textStart[byteCount - 1] == 0 && textStart[byteCount - 2] == 0) {
            byteCount -= 2;
        }

        size_t charCount = byteCount / 2;

        // Build wstring, handling endianness
        std::wstring wstr;
        for (size_t i = 0; i < charCount; i++) {
            wchar_t ch;
            if (bigEndian) {
                ch = (textStart[i * 2] << 8) | textStart[i * 2 + 1];
            } else {
                ch = textStart[i * 2] | (textStart[i * 2 + 1] << 8);
            }
            if (ch == 0) break;  // Stop at null terminator
            wstr += ch;
        }

        // Convert to UTF-8
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return "";
    } else if (encoding == 2) {
        // UTF-16BE without BOM
        // Remove trailing null WORDS (2 bytes at a time)
        while (textLen >= 2 && text[textLen - 1] == 0 && text[textLen - 2] == 0) {
            textLen -= 2;
        }

        size_t charCount = textLen / 2;
        std::wstring wstr;
        for (size_t i = 0; i < charCount; i++) {
            wchar_t ch = (text[i * 2] << 8) | text[i * 2 + 1];
            if (ch == 0) break;
            wstr += ch;
        }
        int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (utf8Len > 0) {
            std::string result(utf8Len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], utf8Len, nullptr, nullptr);
            return result;
        }
        return "";
    } else if (encoding == 3) {
        // UTF-8 - remove trailing nulls (single-byte null terminator)
        while (textLen > 0 && text[textLen - 1] == 0) textLen--;
        if (textLen == 0) return "";
        return std::string((const char*)text, textLen);
    }
    return "";
}

// Helper: Get ID3v2 frame by ID (e.g., "TIT2", "TPE1", "TALB")
static std::string GetID3v2Frame(const unsigned char* tag, const char* frameId) {
    if (!tag || !frameId) return "";

    // Check ID3v2 header
    if (tag[0] != 'I' || tag[1] != 'D' || tag[2] != '3') return "";

    unsigned char version = tag[3];
    // unsigned char revision = tag[4];
    unsigned char flags = tag[5];

    // Tag size (syncsafe integer)
    size_t tagSize = ((tag[6] & 0x7F) << 21) | ((tag[7] & 0x7F) << 14) |
                     ((tag[8] & 0x7F) << 7) | (tag[9] & 0x7F);

    const unsigned char* pos = tag + 10;
    const unsigned char* end = tag + 10 + tagSize;

    // Skip extended header if present
    if (flags & 0x40) {
        size_t extSize = (pos[0] << 24) | (pos[1] << 16) | (pos[2] << 8) | pos[3];
        pos += 4 + extSize;
    }

    // Parse frames
    while (pos < end - 10) {
        // Frame header
        char id[5] = {(char)pos[0], (char)pos[1], (char)pos[2], (char)pos[3], 0};

        // End of frames (padding)
        if (id[0] == 0) break;

        size_t frameSize;
        if (version >= 4) {
            // ID3v2.4: syncsafe integer
            frameSize = ((pos[4] & 0x7F) << 21) | ((pos[5] & 0x7F) << 14) |
                        ((pos[6] & 0x7F) << 7) | (pos[7] & 0x7F);
        } else {
            // ID3v2.3: regular integer
            frameSize = (pos[4] << 24) | (pos[5] << 16) | (pos[6] << 8) | pos[7];
        }

        // unsigned short frameFlags = (pos[8] << 8) | pos[9];
        pos += 10;

        if (frameSize == 0 || pos + frameSize > end) break;

        // Check if this is the frame we want
        if (strcmp(id, frameId) == 0) {
            return ParseID3v2TextFrame(pos, frameSize);
        }

        pos += frameSize;
    }

    return "";
}

// Map common tag names to ID3v2 frame IDs
static const char* GetID3v2FrameId(const char* tagName) {
    if (_stricmp(tagName, "TITLE") == 0) return "TIT2";
    if (_stricmp(tagName, "ARTIST") == 0) return "TPE1";
    if (_stricmp(tagName, "ALBUM") == 0) return "TALB";
    if (_stricmp(tagName, "YEAR") == 0) return "TYER";
    if (_stricmp(tagName, "DATE") == 0) return "TDRC";
    if (_stricmp(tagName, "TRACK") == 0) return "TRCK";
    if (_stricmp(tagName, "TRACKNUMBER") == 0) return "TRCK";
    if (_stricmp(tagName, "GENRE") == 0) return "TCON";
    if (_stricmp(tagName, "COMMENT") == 0) return "COMM";
    return nullptr;
}

// Helper: Get tag string trimmed (for ID3v1 fixed-length fields)
static std::string GetTrimmedTag(const char* data, size_t maxLen) {
    if (!data) return "";

    // Find actual length (ID3v1 fields are space-padded)
    size_t len = 0;
    for (size_t i = 0; i < maxLen && data[i] != '\0'; i++) {
        len = i + 1;
    }

    // Trim trailing spaces
    while (len > 0 && (data[len - 1] == ' ' || data[len - 1] == '\0')) {
        len--;
    }

    return std::string(data, len);
}

// Helper: Parse ICY headers (format: "key:value\r\n")
static std::string GetICYTag(const char* tags, const char* key) {
    if (!tags || !key) return "";

    size_t keyLen = strlen(key);
    const char* p = tags;

    while (*p) {
        // Check if line starts with key: (case insensitive)
        if (_strnicmp(p, key, keyLen) == 0 && p[keyLen] == ':') {
            const char* value = p + keyLen + 1;
            // Skip leading spaces
            while (*value == ' ') value++;
            // Find end (newline or null)
            const char* end = value;
            while (*end && *end != '\r' && *end != '\n') end++;
            return std::string(value, end - value);
        }
        // Move to next line
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return "";
}

// Helper: Parse Shoutcast META tags (format: "StreamTitle='value';")
static std::string GetMetaTag(const char* meta, const char* key) {
    if (!meta || !key) return "";

    // Try standard Shoutcast format: key='value';
    // Note: Look for '; to handle apostrophes in titles (e.g., "Don't Call Me Up")
    std::string searchKey = std::string(key) + "='";
    const char* start = strstr(meta, searchKey.c_str());
    if (start) {
        start += searchKey.length();
        // Look for '; which marks end of field, not just ' which could be an apostrophe
        const char* end = strstr(start, "';");
        if (end) {
            return std::string(start, end - start);
        } else {
            // No semicolon - might be last field, find trailing quote
            end = start + strlen(start);
            if (end > start && *(end - 1) == '\'') {
                return std::string(start, end - start - 1);
            }
        }
    }

    // Try iHeart/alternative format: key="value" (with double quotes)
    searchKey = std::string(key) + "=\"";
    start = strstr(meta, searchKey.c_str());
    if (start) {
        start += searchKey.length();
        // Find closing quote, but handle nested quotes in url field
        const char* end = start;
        while (*end && *end != '"') end++;
        if (*end == '"') return std::string(start, end - start);
    }

    return "";
}

// Helper: Parse iHeart-style StreamTitle
// Format 1: title="...",artist="...",url="..."
// Format 2: Artist - text="Title" song_spot="M" ...
static void ParseIHeartTitle(const std::string& streamTitle, std::string& artist, std::string& title) {
    // Check for Format 2: "Artist - text="Title" ..."
    size_t textPos = streamTitle.find(" - text=\"");
    if (textPos != std::string::npos) {
        // Artist is everything before " - text="
        artist = streamTitle.substr(0, textPos);
        // Title is inside text="..."
        size_t titleStart = textPos + 9;  // Skip ' - text="'
        size_t titleEnd = streamTitle.find("\"", titleStart);
        if (titleEnd != std::string::npos) {
            title = streamTitle.substr(titleStart, titleEnd - titleStart);
        }
        return;
    }

    // Check for Format 1: title="...",artist="..."
    if (streamTitle.find("title=\"") != std::string::npos) {
        // Extract title
        size_t titleStart = streamTitle.find("title=\"");
        if (titleStart != std::string::npos) {
            titleStart += 7;  // Skip 'title="'
            size_t titleEnd = streamTitle.find("\"", titleStart);
            if (titleEnd != std::string::npos) {
                title = streamTitle.substr(titleStart, titleEnd - titleStart);
            }
        }

        // Extract artist
        size_t artistStart = streamTitle.find("artist=\"");
        if (artistStart != std::string::npos) {
            artistStart += 8;  // Skip 'artist="'
            size_t artistEnd = streamTitle.find("\"", artistStart);
            if (artistEnd != std::string::npos) {
                artist = streamTitle.substr(artistStart, artistEnd - artistStart);
            }
        }
    }
}

// Helper: Parse StreamTitle into artist and title (format: "Artist - Title" or iHeart format)
static void ParseStreamTitle(const std::string& streamTitle, std::string& artist, std::string& title) {
    artist = "";
    title = "";

    // First try iHeart format (title="...",artist="...")
    ParseIHeartTitle(streamTitle, artist, title);
    if (!title.empty() || !artist.empty()) {
        return;
    }

    // Then try standard "Artist - Title" format
    size_t sep = streamTitle.find(" - ");
    if (sep != std::string::npos) {
        artist = streamTitle.substr(0, sep);
        title = streamTitle.substr(sep + 3);
    } else {
        // No separator, treat entire string as title
        title = streamTitle;
    }
}

// Universal tag accessor: walks every tag format BASS exposes for the
// stream in turn, returning the first non-empty match for `tagName` (e.g.
// "TITLE", "ARTIST", "ALBUM"). Order matters — modern container formats
// (OGG/APE/MP4/WMA/RIFF/MF) win over ID3v2, and ID3v2 wins over ICY/HTTP
// header heuristics. For internet radio, ICY META is consulted for
// TITLE/ARTIST so the currently-playing song shows up, with a fall-back
// to the station name (icy-name) when no song info is present.
static std::string GetMetadataTag(HSTREAM stream, const char* tagName) {
    if (!stream) return "";

    std::string result;

    // Try OGG/Vorbis comments first (also used by FLAC, Opus)
    const char* oggTags = BASS_ChannelGetTags(stream, BASS_TAG_OGG);
    if (oggTags) {
        result = GetTagFromList(oggTags, tagName);
        if (!result.empty()) return result;
    }

    // Try APE tags
    const char* apeTags = BASS_ChannelGetTags(stream, BASS_TAG_APE);
    if (apeTags) {
        result = GetTagFromList(apeTags, tagName);
        if (!result.empty()) return result;
    }

    // Try MP4/iTunes tags
    const char* mp4Tags = BASS_ChannelGetTags(stream, BASS_TAG_MP4);
    if (mp4Tags) {
        result = GetTagFromList(mp4Tags, tagName);
        if (!result.empty()) return result;
    }

    // Try WMA tags
    const char* wmaTags = BASS_ChannelGetTags(stream, BASS_TAG_WMA);
    if (wmaTags) {
        result = GetTagFromList(wmaTags, tagName);
        if (!result.empty()) return result;
    }

    // Try RIFF INFO tags (for WAV files)
    const char* riffTags = BASS_ChannelGetTags(stream, BASS_TAG_RIFF_INFO);
    if (riffTags) {
        result = GetTagFromList(riffTags, tagName);
        if (!result.empty()) return result;
    }

    // Try Media Foundation tags
    const char* mfTags = BASS_ChannelGetTags(stream, BASS_TAG_MF);
    if (mfTags) {
        result = GetTagFromList(mfTags, tagName);
        if (!result.empty()) return result;
    }

    // Try ID3v2 tags (common for MP3 files)
    const unsigned char* id3v2 = (const unsigned char*)BASS_ChannelGetTags(stream, BASS_TAG_ID3V2);
    if (id3v2) {
        const char* frameId = GetID3v2FrameId(tagName);
        if (frameId) {
            result = GetID3v2Frame(id3v2, frameId);
            if (!result.empty()) return result;
        }
    }

    // Try ICY (Shoutcast/Icecast) headers for streams
    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        // Map common tag names to ICY header names
        if (_stricmp(tagName, "TITLE") == 0 || _stricmp(tagName, "ARTIST") == 0) {
            // For title/artist, check META first (has current song info)
            const char* meta = BASS_ChannelGetTags(stream, BASS_TAG_META);
            if (meta) {
                std::string streamTitle = GetMetaTag(meta, "StreamTitle");
                if (!streamTitle.empty()) {
                    std::string artist, title;
                    ParseStreamTitle(streamTitle, artist, title);
                    if (_stricmp(tagName, "TITLE") == 0 && !title.empty()) return title;
                    if (_stricmp(tagName, "ARTIST") == 0 && !artist.empty()) return artist;
                }
            }
            // Fall back to station name for title
            if (_stricmp(tagName, "TITLE") == 0) {
                result = GetICYTag(icyTags, "icy-name");
                if (!result.empty()) return result;
            }
        } else if (_stricmp(tagName, "GENRE") == 0) {
            result = GetICYTag(icyTags, "icy-genre");
            if (!result.empty()) return result;
        }
    }

    // Also try HTTP headers for streams
    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        // HTTP headers use similar format to ICY
        if (_stricmp(tagName, "TITLE") == 0) {
            result = GetICYTag(httpTags, "icy-name");
            if (!result.empty()) return result;
        } else if (_stricmp(tagName, "GENRE") == 0) {
            result = GetICYTag(httpTags, "icy-genre");
            if (!result.empty()) return result;
        }
    }

    return "";
}

// Get stream title directly from META tags (for streams)
// Returns formatted "Artist - Title" or just raw stream title
static std::string GetStreamTitle(HSTREAM stream) {
    if (!stream) return "";

    const char* meta = BASS_ChannelGetTags(stream, BASS_TAG_META);
    if (meta) {
        std::string rawTitle = GetMetaTag(meta, "StreamTitle");
        if (!rawTitle.empty()) {
            // Try to parse iHeart or standard format
            std::string artist, title;
            ParseStreamTitle(rawTitle, artist, title);

            // Return formatted string
            if (!artist.empty() && !title.empty()) {
                return artist + " - " + title;
            } else if (!title.empty()) {
                return title;
            }
            // Fall back to raw title if parsing failed
            return rawTitle;
        }
    }
    return "";
}

// Get station name from ICY headers
static std::string GetStationName(HSTREAM stream) {
    if (!stream) return "";

    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        std::string name = GetICYTag(icyTags, "icy-name");
        if (!name.empty()) return name;
    }

    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        std::string name = GetICYTag(httpTags, "icy-name");
        if (!name.empty()) return name;
    }

    return "";
}

// Get stream bitrate from ICY headers
static int GetStreamBitrate(HSTREAM stream) {
    if (!stream) return 0;

    const char* icyTags = BASS_ChannelGetTags(stream, BASS_TAG_ICY);
    if (icyTags) {
        std::string br = GetICYTag(icyTags, "icy-br");
        if (!br.empty()) return atoi(br.c_str());
    }

    const char* httpTags = BASS_ChannelGetTags(stream, BASS_TAG_HTTP);
    if (httpTags) {
        std::string br = GetICYTag(httpTags, "icy-br");
        if (!br.empty()) return atoi(br.c_str());
    }

    return 0;
}

// Pick the right stream handle for tag reading.
// BASS_ChannelGetTags only returns ID3v2/Vorbis/etc. blocks from the
// original decoder stream — the BASS_FX tempo wrapper does not forward
// them. So when we still have g_stream (Speedy/Signalsmith path, or before
// SoundTouch nulled it via BASS_FX_FREESOURCE), use it. Otherwise fall back
// to g_fxStream, which works for SoundTouch where g_fxStream actually IS
// the decoder stream wrapped by tempo, and BASS forwards tag access through.
static HSTREAM GetTagStream() {
    return g_stream ? g_stream : g_fxStream;
}

// Helper to speak UTF-8 text with proper Unicode support
static void SpeakUtf8(const std::string& text) {
    SpeakW(Utf8ToWide(text));
}

void SpeakTagTitle() {
    // v1.60 — speak the unified "<source> - <item>" the user also sees in
    // the window title. BuildNowPlayingSpeech composes from the v1.60
    // now-playing globals + falls back to ICY/MPV/tags when the item is
    // still empty (e.g. just after Play, before the first ICY metadata).
    std::wstring composed = BuildNowPlayingSpeech();
    if (!composed.empty()) {
        SpeakW(composed);
        return;
    }

    // Pre-v1.60 fallback path: nothing in the now-playing globals AND
    // nothing usable from BASS/MPV. Either we're truly idle or we're in
    // a state the new infrastructure didn't catch (BASS stream with no
    // tags + no SetNowPlaying call).
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string streamTitle = GetStreamTitle(stream);
    if (!streamTitle.empty()) {
        SpeakUtf8(streamTitle);
        return;
    }

    std::string title = GetMetadataTag(stream, "TITLE");
    std::string artist = GetMetadataTag(stream, "ARTIST");
    if (title.empty() || artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            if (title.empty())  title  = GetTrimmedTag(id3->title, 30);
            if (artist.empty()) artist = GetTrimmedTag(id3->artist, 30);
        }
    }

    if (!artist.empty() && !title.empty()) SpeakUtf8(artist + " - " + title);
    else if (!title.empty())               SpeakUtf8(title);
    else if (!artist.empty())              SpeakUtf8(artist);
    else                                   Speak(Ts("No title"));
}

void SpeakTagArtist() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string artist = GetMetadataTag(stream, "ARTIST");

    // Try ID3v1 if nothing found
    if (artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            artist = GetTrimmedTag(id3->artist, 30);
        }
    }

    if (!artist.empty()) {
        SpeakUtf8(Ts("Artist: ") + artist);
    } else {
        Speak(Ts("No artist"));
    }
}

void SpeakTagAlbum() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string album = GetMetadataTag(stream, "ALBUM");

    // Try ID3v1 if nothing found
    if (album.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            album = GetTrimmedTag(id3->album, 30);
        }
    }

    // For streams, fall back to station name
    if (album.empty()) {
        std::string station = GetStationName(stream);
        if (!station.empty()) {
            SpeakUtf8(Ts("Station: ") + station);
            return;
        }
    }

    if (!album.empty()) {
        SpeakUtf8(Ts("Album: ") + album);
    } else {
        Speak(Ts("No album"));
    }
}

void SpeakTagYear() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string year = GetMetadataTag(stream, "DATE");
    if (year.empty()) year = GetMetadataTag(stream, "YEAR");

    // Try ID3v1 if nothing found
    if (year.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            year = GetTrimmedTag(id3->year, 4);
        }
    }

    if (!year.empty()) {
        SpeakUtf8(Ts("Year: ") + year);
    } else {
        Speak(Ts("No year"));
    }
}

void SpeakTagTrack() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string track = GetMetadataTag(stream, "TRACKNUMBER");
    if (track.empty()) track = GetMetadataTag(stream, "TRACK");

    // Try ID3v1.1 track number (stored in comment field)
    if (track.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->comment[28] == '\0' && id3->comment[29] != 0) {
            // ID3v1.1 format: comment[28] is 0, comment[29] is track number
            int trackNum = (unsigned char)id3->comment[29];
            if (trackNum > 0) {
                track = std::to_string(trackNum);
            }
        }
    }

    if (!track.empty()) {
        SpeakUtf8(Ts("Track: ") + track);
    } else {
        Speak(Ts("No track number"));
    }
}

void SpeakTagGenre() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string genre = GetMetadataTag(stream, "GENRE");

    // Try ID3v1 if nothing found
    if (genre.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->genre < g_id3GenreCount) {
            genre = g_id3Genres[id3->genre];
        }
    }

    if (!genre.empty()) {
        SpeakUtf8(Ts("Genre: ") + genre);
    } else {
        Speak(Ts("No genre"));
    }
}

void SpeakTagComment() {
    HSTREAM stream = GetTagStream();
    if (!stream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::string comment = GetMetadataTag(stream, "COMMENT");
    if (comment.empty()) comment = GetMetadataTag(stream, "DESCRIPTION");

    // Try ID3v1 if nothing found
    if (comment.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            // Check for ID3v1.1 (comment is only 28 chars if track is present)
            if (id3->comment[28] == '\0' && id3->comment[29] != 0) {
                comment = GetTrimmedTag(id3->comment, 28);
            } else {
                comment = GetTrimmedTag(id3->comment, 30);
            }
        }
    }

    if (!comment.empty()) {
        SpeakUtf8(Ts("Comment: ") + comment);
    } else {
        Speak(Ts("No comment"));
    }
}

void SpeakTagBitrate() {
    // v2.13 — MPV video has no BASS stream, so this said "Nothing playing" on
    // every video (reported by Sèb / Winaide). Announce the video's technical
    // info (resolution, codec, bitrate) instead. Empty string -> generic note.
    if (g_activeEngine == PlaybackEngine::MPV) {
        std::wstring info = MPVGetVideoInfo();
        if (!info.empty()) Speak(WideToUtf8(info));
        else               Speak(Ts("No media information"));
        return;
    }
    if (!g_fxStream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(g_fxStream, &info)) {
        Speak(Ts("Cannot get info"));
        return;
    }

    HSTREAM sourceStream = g_stream ? g_stream : g_fxStream;

    // Get bitrate if available (for compressed formats)
    float bitrate = 0;
    BASS_ChannelGetAttribute(sourceStream, BASS_ATTRIB_BITRATE, &bitrate);

    // If no bitrate from attribute, try ICY headers (for streams)
    if (bitrate <= 0) {
        int icyBitrate = GetStreamBitrate(sourceStream);
        if (icyBitrate > 0) bitrate = (float)icyBitrate;
    }

    char buf[128];
    std::string channels = info.chans == 1 ? Ts("mono") : (info.chans == 2 ? Ts("stereo") : Ts("multi-channel"));
    if (bitrate > 0) {
        snprintf(buf, sizeof(buf), Ts("%d kbps, %d Hz, %s").c_str(),
                 (int)bitrate, info.freq, channels.c_str());
    } else {
        // Probably a lossless format
        int bits = (info.flags & BASS_SAMPLE_8BITS) ? 8 :
                   (info.flags & BASS_SAMPLE_FLOAT) ? 32 : 16;
        snprintf(buf, sizeof(buf), Ts("%d-bit, %d Hz, %s").c_str(),
                 bits, info.freq, channels.c_str());
    }

    Speak(buf);
}

// Best-effort current bitrate in kbps for the status bar.
// Tries the live BASS attribute first (updates per-frame for VBR files),
// then the cached value captured at load time, then ICY headers for
// internet streams. Returns 0 if none of those produce a value (e.g.
// lossless source where the concept doesn't apply).
int GetCurrentBitrate() {
    if (g_sourceStream) {
        float bitrate = 0;
        if (BASS_ChannelGetAttribute(g_sourceStream, BASS_ATTRIB_BITRATE, &bitrate) && bitrate > 0) {
            return static_cast<int>(bitrate);
        }
    }

    // Fall back to cached bitrate (captured at load time)
    if (g_currentBitrate > 0) return g_currentBitrate;

    // Fall back to ICY headers for internet streams
    if (g_sourceStream) {
        int icyBitrate = GetStreamBitrate(g_sourceStream);
        if (icyBitrate > 0) return icyBitrate;
    }

    return 0;
}

void SpeakTagDuration() {
    // v2.13 — MPV video: report the real length from the MPV clock instead of
    // "Nothing playing" (there is no BASS stream during video playback).
    if (g_activeEngine == PlaybackEngine::MPV) {
        double len = MPVGetLength();
        if (len <= 0) { Speak(Ts("Unknown duration")); return; }
        int totalSecs = (int)len;
        int h = totalSecs / 3600, m = (totalSecs % 3600) / 60, s = totalSecs % 60;
        char buf[128];
        if (h > 0)
            snprintf(buf, sizeof(buf), Ts("Duration: %d hours, %d minutes, %d seconds").c_str(), h, m, s);
        else if (m > 0)
            snprintf(buf, sizeof(buf), Ts("Duration: %d minutes, %d seconds").c_str(), m, s);
        else
            snprintf(buf, sizeof(buf), Ts("Duration: %d seconds").c_str(), s);
        Speak(buf);
        return;
    }
    if (!g_fxStream) {
        Speak(Ts("Nothing playing"));
        return;
    }

    TempoProcessor* processor = GetTempoProcessor();
    double length = 0;
    if (processor && processor->IsActive()) {
        length = processor->GetLength();
    }

    if (length <= 0) {
        // Check if it's a stream (URL)
        if (g_currentTrack >= 0 && g_currentTrack < (int)g_playlist.size()) {
            if (IsURL(g_playlist[g_currentTrack].c_str())) {
                Speak(Ts("Live stream"));
                return;
            }
        }
        Speak(Ts("Unknown duration"));
        return;
    }

    int totalSecs = (int)length;
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;

    char buf[128];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), Ts("Duration: %d hours, %d minutes, %d seconds").c_str(), hours, mins, secs);
    } else if (mins > 0) {
        snprintf(buf, sizeof(buf), Ts("Duration: %d minutes, %d seconds").c_str(), mins, secs);
    } else {
        snprintf(buf, sizeof(buf), Ts("Duration: %d seconds").c_str(), secs);
    }

    Speak(buf);
}

void SpeakTagFilename() {
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) {
        Speak(Ts("Nothing playing"));
        return;
    }

    std::wstring path = g_playlist[g_currentTrack];

    // Check if it's a URL
    if (IsURL(path.c_str())) {
        // For streams, show the full URL
        SpeakW(std::wstring(T("URL: ")) + path);
        return;
    }

    // Get just the filename
    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');

    std::wstring filename = lastSlash ? (lastSlash + 1) : path;

    SpeakW(std::wstring(T("Filename: ")) + filename);
}

// Tag retrieval functions for display in dialog
std::wstring GetTagTitle() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string streamTitle = GetStreamTitle(stream);
    if (!streamTitle.empty()) {
        return Utf8ToWide(streamTitle);
    }

    std::string title = GetMetadataTag(stream, "TITLE");
    std::string artist = GetMetadataTag(stream, "ARTIST");

    if (title.empty() || artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) {
            if (title.empty()) title = GetTrimmedTag(id3->title, 30);
            if (artist.empty()) artist = GetTrimmedTag(id3->artist, 30);
        }
    }

    if (!artist.empty() && !title.empty()) {
        return Utf8ToWide(artist + " - " + title);
    } else if (!title.empty()) {
        return Utf8ToWide(title);
    } else if (!artist.empty()) {
        return Utf8ToWide(artist);
    }
    return T("No title");
}

std::wstring GetTagArtist() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string artist = GetMetadataTag(stream, "ARTIST");
    if (artist.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) artist = GetTrimmedTag(id3->artist, 30);
    }
    return artist.empty() ? std::wstring(T("No artist")) : Utf8ToWide(artist);
}

std::wstring GetTagAlbum() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string album = GetMetadataTag(stream, "ALBUM");
    if (album.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) album = GetTrimmedTag(id3->album, 30);
    }
    if (album.empty()) {
        std::string station = GetStationName(stream);
        if (!station.empty()) return Utf8ToWide(station);
    }
    return album.empty() ? std::wstring(T("No album")) : Utf8ToWide(album);
}

std::wstring GetTagYear() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string year = GetMetadataTag(stream, "DATE");
    if (year.empty()) year = GetMetadataTag(stream, "YEAR");
    if (year.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) year = GetTrimmedTag(id3->year, 4);
    }
    return year.empty() ? std::wstring(T("No year")) : Utf8ToWide(year);
}

std::wstring GetTagTrack() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string track = GetMetadataTag(stream, "TRACKNUMBER");
    if (track.empty()) track = GetMetadataTag(stream, "TRACK");
    if (track.empty()) {
        // Try ID3v1.1 track number (stored in comment field)
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->comment[28] == '\0' && id3->comment[29] != 0) {
            int trackNum = (unsigned char)id3->comment[29];
            if (trackNum > 0) track = std::to_string(trackNum);
        }
    }
    return track.empty() ? std::wstring(T("No track")) : Utf8ToWide(track);
}

std::wstring GetTagGenre() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string genre = GetMetadataTag(stream, "GENRE");
    if (genre.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3 && id3->genre < 192) {
            // ID3v1 genre lookup (simplified - first few common ones)
            static const char* genres[] = {
                "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge",
                "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other", "Pop", "R&B",
                "Rap", "Reggae", "Rock", "Techno", "Industrial"
            };
            if (id3->genre < 20) genre = genres[id3->genre];
            else genre = std::to_string(id3->genre);
        }
    }
    return genre.empty() ? std::wstring(T("No genre")) : Utf8ToWide(genre);
}

std::wstring GetTagComment() {
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    std::string comment = GetMetadataTag(stream, "COMMENT");
    if (comment.empty()) comment = GetMetadataTag(stream, "DESCRIPTION");
    if (comment.empty()) {
        const TAG_ID3* id3 = (const TAG_ID3*)BASS_ChannelGetTags(stream, BASS_TAG_ID3);
        if (id3) comment = GetTrimmedTag(id3->comment, 30);
    }
    return comment.empty() ? std::wstring(T("No comment")) : Utf8ToWide(comment);
}

std::wstring GetTagBitrate() {
    // v2.13 — MPV video: report the video's technical info (see SpeakTagBitrate).
    if (g_activeEngine == PlaybackEngine::MPV) {
        std::wstring info = MPVGetVideoInfo();
        return info.empty() ? std::wstring(T("No media information")) : info;
    }
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    BASS_CHANNELINFO info;
    BASS_ChannelGetInfo(stream, &info);

    float bitrate = 0;
    BASS_ChannelGetAttribute(stream, BASS_ATTRIB_BITRATE, &bitrate);

    if (bitrate > 0) {
        wchar_t buf[128];
        const wchar_t* channels = info.chans == 1 ? T("Mono") : (info.chans == 2 ? T("Stereo") : T("Multi-channel"));
        swprintf(buf, 128, T("%.0f kbps, %d Hz, %s"),
            bitrate, info.freq, channels);
        return buf;
    }
    return T("Unknown bitrate");
}

std::wstring GetTagDuration() {
    // v2.13 — MPV video: real length from the MPV clock.
    if (g_activeEngine == PlaybackEngine::MPV) {
        double len = MPVGetLength();
        return len > 0 ? FormatTime(len) : std::wstring(T("Unknown duration"));
    }
    HSTREAM stream = GetTagStream();
    if (!stream) return T("Nothing playing");

    TempoProcessor* processor = GetTempoProcessor();
    if (!processor || !processor->IsActive()) {
        QWORD len = BASS_ChannelGetLength(stream, BASS_POS_BYTE);
        if (len == (QWORD)-1) return T("Unknown duration");
        double duration = BASS_ChannelBytes2Seconds(stream, len);
        return FormatTime(duration);
    }
    return FormatTime(processor->GetLength());
}

std::wstring GetTagFilename() {
    if (g_currentTrack < 0 || g_currentTrack >= (int)g_playlist.size()) {
        return L"Nothing playing";
    }

    std::wstring path = g_playlist[g_currentTrack];
    if (IsURL(path.c_str())) return path;

    const wchar_t* lastSlash = wcsrchr(path.c_str(), L'\\');
    if (!lastSlash) lastSlash = wcsrchr(path.c_str(), L'/');
    return lastSlash ? (lastSlash + 1) : path;
}

// Convert friendly tokens ({année}, {year}, etc.) into C strftime codes.
// This lets users type natural French/English placeholders while still
// using the standard C library underneath. Existing strftime codes pass
// through unchanged so old templates keep working.
std::wstring ExpandFilenameTokens(const std::wstring& tmpl) {
    struct TokenMap { const wchar_t* token; const wchar_t* code; };
    static const TokenMap kTokens[] = {
        // French
        { L"{année}",   L"%Y" }, { L"{annee}",   L"%Y" },
        { L"{mois}",    L"%m" },
        { L"{jour}",    L"%d" },
        { L"{heure}",   L"%H" },
        { L"{minute}",  L"%M" },
        { L"{seconde}", L"%S" },
        // English
        { L"{year}",    L"%Y" },
        { L"{month}",   L"%m" },
        { L"{day}",     L"%d" },
        { L"{hour}",    L"%H" },
        { L"{min}",     L"%M" },
        { L"{second}",  L"%S" }, { L"{sec}", L"%S" },
    };
    std::wstring out = tmpl;
    for (const auto& m : kTokens) {
        size_t pos = 0;
        while ((pos = out.find(m.token, pos)) != std::wstring::npos) {
            out.replace(pos, wcslen(m.token), m.code);
            pos += wcslen(m.code);
        }
    }
    return out;
}

// Generate output filename based on template
static std::wstring GenerateRecordingFilename() {
    // Get current time
    time_t now = time(nullptr);
    struct tm localTime;
    localtime_s(&localTime, &now);

    // Expand friendly tokens first, then run strftime
    std::wstring expanded = ExpandFilenameTokens(g_recordTemplate);

    // Format using the template
    wchar_t buffer[256];
    wcsftime(buffer, 256, expanded.c_str(), &localTime);

    // Add appropriate extension
    const wchar_t* ext;
    switch (g_recordFormat) {
        case 1: ext = L".mp3"; break;
        case 2: ext = L".ogg"; break;
        case 3: ext = L".flac"; break;
        default: ext = L".wav"; break;
    }

    std::wstring filename = buffer;
    filename += ext;

    return filename;
}

// Stop recording
void StopRecording() {
    if (!g_isRecording || !g_encoder) return;

    BASS_Encode_Stop(g_encoder);  // finalizes the file with the captured spans
    g_encoder = 0;
    g_isRecording = false;
    g_recordPaused = false;       // v2.24 — clear punch-in/out state

    Speak(Ts("Recording stopped"));
    UpdateStatusBar();
}

// Start (or stop) recording the current playback to a file. Format is
// chosen from g_recordFormat (0=WAV, 1=MP3, 2=OGG, 3=FLAC); MP3/OGG/FLAC
// fall back to WAV with a fresh timestamped filename if the lossy encoder
// can't be started (missing encoder plugin, unusable parameters).
//
// The encoder taps into g_fxStream at the default priority (0). The volume
// DSP runs at -2000000000 (effectively last), so the recording captures
// audio at full pre-volume amplitude regardless of the user's playback
// volume — unless legacy volume mode is on, in which case BASS_ATTRIB_VOL
// attenuates earlier and the recording inherits the playback volume.
// v1.94 — system-audio (WASAPI loopback) recording helper. This is the
// SEPARATE path; it never touches g_isRecording / g_encoder / StopRecording.
// IsSystemCapturing() is the source of truth for its state. Output folder and
// filename reuse the exact same logic as the legacy ToggleRecording() below.
static void ToggleSystemRecording() {
    // Already capturing? Stop and finalize.
    if (mediaaccess::IsSystemCapturing()) {
        mediaaccess::StopSystemCapture();
        g_recordPaused = false;   // v2.24 — clear punch-in/out state on stop
        Speak(Ts("Recording stopped"));
        UpdateStatusBar();
        return;
    }

    // Determine output directory (same logic as legacy ToggleRecording).
    std::wstring outputPath;
    if (g_recordPath.empty()) {
        wchar_t musicPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
            outputPath = musicPath;
        } else {
            wchar_t currentDir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, currentDir);
            outputPath = currentDir;
        }
    } else {
        outputPath = g_recordPath;
    }
    CreateDirectoryW(outputPath.c_str(), nullptr);

    std::wstring fullPath = outputPath;
    if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
        fullPath += L'\\';
    }
    fullPath += GenerateRecordingFilename();

    // Resolve the loopback device to capture.
    int bwaIndex = g_systemRecordDevice;
    if (bwaIndex < 0) {
        // Auto: follow MediaAccess's active output device.
        bwaIndex = mediaaccess::FindLoopbackForCurrentBassDevice();
        if (bwaIndex < 0) {
            // v2.11 — could not identify the loopback of the device MediaAccess
            // is actually playing to. Do NOT silently grab an arbitrary loopback:
            // that previously captured a virtual cable (Cable Input) without the
            // user knowing. Tell them to pick a device manually instead.
            Speak(Ts("Could not identify the output device to record. "
                     "Choose a device in Options, Recording."));
            return;
        }
    }

    if (bwaIndex < 0) {
        Speak(Ts("No audio devices found"));
        return;
    }

    std::wstring capturedName;
    if (!mediaaccess::StartSystemCapture(bwaIndex, fullPath, g_recordFormat,
                                         g_recordBitrate, capturedName)) {
        MessageBoxW(GetMessageBoxOwner(),
                    T("Failed to start system audio recording."),
                    APP_NAME, MB_ICONERROR);
        return;
    }
    g_recordPaused = false;   // v2.24 — fresh capture is never paused

    // Announce: "Recording started, source system, device <name>".
    std::wstring msg = T("Recording started");
    msg += L", ";
    msg += T("source system");
    if (!capturedName.empty()) {
        msg += L", ";
        msg += capturedName;
    }
    SpeakW(msg);
    UpdateStatusBar();
}

void ToggleRecording() {
    // v1.94 — system-audio recording is a SEPARATE path. When the user has
    // selected source 1 (Windows system audio), dispatch to the loopback
    // engine and return. The "MediaAccess output" recording below is unchanged;
    // g_recordSource == 1 is the ONLY entry point to the new engine.
    if (g_recordSource == 1) {
        ToggleSystemRecording();
        return;
    }

    // If already recording, stop
    if (g_isRecording) {
        StopRecording();
        return;
    }

    // Need a stream to record from
    if (!g_fxStream) {
        Speak(Ts("Nothing to record"));
        return;
    }

    // Determine output path
    std::wstring outputPath;
    if (g_recordPath.empty()) {
        // Default to Music folder
        wchar_t musicPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
            outputPath = musicPath;
        } else {
            // Fall back to current directory
            wchar_t currentDir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, currentDir);
            outputPath = currentDir;
        }
    } else {
        outputPath = g_recordPath;
    }

    // Ensure output directory exists
    CreateDirectoryW(outputPath.c_str(), nullptr);

    // Generate filename
    std::wstring filename = GenerateRecordingFilename();
    std::wstring fullPath = outputPath;
    if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/') {
        fullPath += L'\\';
    }
    fullPath += filename;

    // Get stream info for encoder setup
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(g_fxStream, &info)) {
        Speak(Ts("Cannot get stream info"));
        return;
    }

    // Start appropriate encoder based on format
    // Note: BASS_ENCODE_FP_16BIT converts floating-point audio to 16-bit integer
    // which is required for WAV and FLAC encoders
    const DWORD wavFlags = BASS_ENCODE_AUTOFREE | BASS_ENCODE_FP_16BIT;

    // If the chosen lossy/lossless encoder fails to start, fall back to plain
    // WAV with a fresh timestamped filename. Centralized so MP3/OGG/FLAC all
    // surface the same neutral message and end up in the same recovery state.
    auto fallbackToWav = [&](const wchar_t* failedFormatMsg) {
        MessageBoxW(GetMessageBoxOwner(), failedFormatMsg, APP_NAME, MB_ICONWARNING);
        fullPath = outputPath;
        if (!fullPath.empty() && fullPath.back() != L'\\') fullPath += L'\\';
        fullPath += GenerateRecordingFilename();
        g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
    };

    switch (g_recordFormat) {
        case 0: {
            // WAV - use BASS_Encode_StartPCMFile for direct WAV output
            g_encoder = BASS_Encode_StartPCMFile(g_fxStream, wavFlags, fullPath.c_str());
            break;
        }
        case 1: {
            // MP3 - use bassenc_mp3
            wchar_t options[64];
            swprintf(options, 64, L"--preset cbr %d", g_recordBitrate);
            g_encoder = BASS_Encode_MP3_StartFile(g_fxStream, options, BASS_ENCODE_AUTOFREE, fullPath.c_str());
            if (!g_encoder) fallbackToWav(T("MP3 encoding failed.\nFalling back to WAV format."));
            break;
        }
        case 2: {
            // OGG - use bassenc_ogg
            wchar_t options[64];
            swprintf(options, 64, L"--bitrate %d", g_recordBitrate);
            g_encoder = BASS_Encode_OGG_StartFile(g_fxStream, options, BASS_ENCODE_AUTOFREE, fullPath.c_str());
            if (!g_encoder) fallbackToWav(T("OGG encoding failed.\nFalling back to WAV format."));
            break;
        }
        case 3: {
            // FLAC - use bassenc_flac (also needs FP conversion)
            g_encoder = BASS_Encode_FLAC_StartFile(g_fxStream, nullptr, wavFlags, fullPath.c_str());
            if (!g_encoder) fallbackToWav(T("FLAC encoding failed.\nFalling back to WAV format."));
            break;
        }
    }

    if (!g_encoder) {
        int err = BASS_ErrorGetCode();
        wchar_t msg[256];
        swprintf(msg, 256, T("Failed to start recording (error %d)"), err);
        MessageBoxW(GetMessageBoxOwner(), msg, APP_NAME, MB_ICONERROR);
        return;
    }

    g_isRecording = true;
    g_recordPaused = false;        // v2.24 — never inherit a stale paused state
    Speak(Ts("Recording started"));
    UpdateStatusBar();
}

// v2.24 — pause/resume whichever recording is active. We pause the ENCODER, not
// the playback/capture, so the file simply skips the paused span (punch-in/out):
// one file, only the un-paused segments. Reported by Romain.
void ToggleRecordingPause() {
    // Loopback (system audio) path — independent of g_fxStream / g_isRecording.
    if (mediaaccess::IsSystemCapturing()) {
        bool nowPaused = mediaaccess::ToggleSystemCapturePaused();
        g_recordPaused = nowPaused;  // mirror engine truth for the status bar
        Speak(nowPaused ? Ts("Recording paused") : Ts("Recording resumed"));
        UpdateStatusBar();
        return;
    }
    // Legacy "MediaAccess output" path.
    if (g_isRecording && g_encoder) {
        g_recordPaused = !g_recordPaused;
        BASS_Encode_SetPaused(g_encoder, g_recordPaused ? TRUE : FALSE);
        Speak(g_recordPaused ? Ts("Recording paused") : Ts("Recording resumed"));
        UpdateStatusBar();
        return;
    }
    Speak(Ts("Not recording"));     // neither active; no state change
}


// ============================================================
// Dual-engine facade query functions.
// Audio playback runs through BASS, video through libmpv. These accessors
// dispatch on g_activeEngine so the rest of the app (status bar, hotkeys,
// announcements) never has to care which engine owns the current media.
// ============================================================

double GetTrackLength() {
    if (g_activeEngine == PlaybackEngine::MPV) return MPVGetLength();
    TempoProcessor* p = GetTempoProcessor();
    return (p && p->IsActive()) ? p->GetLength() : 0.0;
}

bool IsCurrentlyPlaying() {
    if (g_activeEngine == PlaybackEngine::MPV) return MPVIsPlaying();
    return g_fxStream && BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PLAYING;
}

bool IsCurrentlyPaused() {
    if (g_activeEngine == PlaybackEngine::MPV) return MPVIsPaused();
    return g_fxStream && BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_PAUSED;
}

bool IsCurrentlyStopped() {
    if (g_activeEngine == PlaybackEngine::MPV) return MPVIsStopped();
    return !g_fxStream || BASS_ChannelIsActive(g_fxStream) == BASS_ACTIVE_STOPPED;
}
