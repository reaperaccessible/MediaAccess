// =============================================================================
// audio_device_watcher.cpp — v2.32. Core Audio endpoint-change watcher.
//
// Registers an IMMNotificationClient with the system MMDeviceEnumerator. When
// the default RENDER endpoint changes or any endpoint changes state, the COM
// callback posts WM_AUDIO_DEVICE_CHANGED to the main window (g_hwnd). The UI
// thread then performs the reroute (see HandleAudioDeviceChange in player.cpp).
//
// IMPORTANT: the callback methods run on a COM worker thread. The ONLY action
// they take is PostMessageW, which is thread-safe. They never touch BASS or any
// g_* state — all real work happens on the UI thread.
// =============================================================================

#include <windows.h>
#include <mmdeviceapi.h>   // IMMDeviceEnumerator / IMMNotificationClient

#include "mediaaccess/audio_device_watcher.h"
#include "mediaaccess/globals.h"   // g_hwnd

// ----------------------------------------------------------------------------
// Minimal IMMNotificationClient. Manual ref-count; no real per-device work —
// every relevant event collapses into a single PostMessageW to the UI thread.
// ----------------------------------------------------------------------------
class DeviceNotifyClient : public IMMNotificationClient {
public:
    DeviceNotifyClient() : m_ref(1) {}
    virtual ~DeviceNotifyClient() {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_ref);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient — the only events we care about post a message.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // v2.33 — only the MEDIA render default matters. BASS_Init binds to the
        // eConsole/eMultimedia default, NEVER eCommunications. The communications
        // default flips whenever any OTHER app opens/closes a comms session
        // (browser, notifications, Teams), which caused spurious "Audio device
        // changed" announcements unrelated to the user's playback device.
        if (flow == eRender && (role == eConsole || role == eMultimedia) && g_hwnd) {
            PostMessageW(g_hwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_AUDIO_DEVICE_CHANGED, 0, 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG m_ref;
};

// File-static watcher state. Kept alive for the app lifetime once started.
static IMMDeviceEnumerator* g_enum = nullptr;
static DeviceNotifyClient*  g_client = nullptr;
static bool g_watchCoInit = false;  // did WE initialize COM here?

bool StartAudioDeviceWatch(HWND /*hwnd*/) {
    if (g_enum && g_client) return true;  // already watching

    // Defensive COM init: tolerate an already-initialized apartment (WM_CREATE
    // runs on the UI/STA thread WinMain already initialized, so this normally
    // returns S_FALSE). Track whether WE initialized so we can balance it on a
    // failure path; on success we keep COM up for the app lifetime.
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_watchCoInit = (co == S_OK || co == S_FALSE);

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&g_enum))) || !g_enum) {
        g_enum = nullptr;
        if (g_watchCoInit) { CoUninitialize(); g_watchCoInit = false; }
        return false;
    }

    g_client = new DeviceNotifyClient();  // ref count starts at 1
    if (FAILED(g_enum->RegisterEndpointNotificationCallback(g_client))) {
        g_client->Release();
        g_client = nullptr;
        g_enum->Release();
        g_enum = nullptr;
        if (g_watchCoInit) { CoUninitialize(); g_watchCoInit = false; }
        return false;
    }

    return true;
}

void StopAudioDeviceWatch() {
    if (g_enum && g_client) {
        g_enum->UnregisterEndpointNotificationCallback(g_client);
    }
    // IMPORTANT: do NOT Release()/delete g_client. UnregisterEndpointNotification
    // Callback does not block callbacks already running on the COM worker thread,
    // so a concurrent OnDefaultDeviceChanged/OnDeviceStateChanged could still be
    // inside the object. Releasing to zero here would `delete this` mid-call — a
    // use-after-free. We intentionally leak the small client object; the OS
    // reclaims it at process exit. (Standard IMMNotificationClient mitigation.)
    g_client = nullptr;
    if (g_enum) {
        g_enum->Release();
        g_enum = nullptr;
    }
    if (g_watchCoInit) { CoUninitialize(); g_watchCoInit = false; }
}
