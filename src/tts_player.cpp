// =============================================================================
// tts_player.cpp — SAPI 5 voice wrapper for DAISY book reading
//
// Wraps ISpVoice using raw COM (no ATL / sphelper.h dependency since this
// MediaAccess build targets a VS Build Tools install without the ATL
// component). Lists voices, sets the active voice, speaks asynchronously,
// and surfaces "end of utterance" as a WM_TTS_END_OF_STREAM PostMessage on
// the main window so the DAISY player advances to the next paragraph from
// the main thread.
// =============================================================================

#include "mediaaccess/tts_player.h"
#include "mediaaccess/logger.h"

#include <windows.h>
#include <objbase.h>
#include <sapi.h>          // ISpVoice, ISpObjectToken, ISpObjectTokenCategory

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern HWND         g_hwnd;
extern std::wstring g_configPath;

namespace mediaaccess {

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static ISpVoice*             g_voice            = nullptr;
static std::wstring          g_activeVoiceId;
static std::vector<TtsVoice> g_voicesCache;
static bool                  g_voicesCacheValid = false;
static double                g_speedMultiplier  = 1.0;

// -----------------------------------------------------------------------------
// Helpers — raw COM, no ATL
// -----------------------------------------------------------------------------

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Wide-string from a CoTaskMemAlloc-ed LPWSTR. Frees the buffer.
static std::wstring TakeCoTaskString(LPWSTR p) {
    if (!p) return L"";
    std::wstring s = p;
    CoTaskMemFree(p);
    return s;
}

// Read [Books] TtsVoiceId from MediaAccess.ini.
static std::wstring LoadVoiceIdFromIni() {
    if (g_configPath.empty()) return L"";
    wchar_t buf[1024] = {0};
    GetPrivateProfileStringW(L"Books", L"TtsVoiceId", L"",
                             buf, 1024, g_configPath.c_str());
    return buf;
}

static void SaveVoiceIdToIni(const std::wstring& id) {
    if (g_configPath.empty()) return;
    WritePrivateProfileStringW(L"Books", L"TtsVoiceId",
                               id.empty() ? nullptr : id.c_str(),
                               g_configPath.c_str());
}

// Convert MediaAccess's multiplier (1.0 = normal) to SAPI's integer rate
// (-10..+10). SAPI's rate is roughly logarithmic; +10 is about 3x normal
// speed. We use log-base-3 of the multiplier scaled by 10 to keep the
// perceived speed change linear with the multiplier.
static long MultiplierToSapiRate(double m) {
    if (m < 0.1) m = 0.1;
    double r = 10.0 * std::log(m) / std::log(3.0);
    if (r < -10) r = -10;
    if (r > 10)  r = 10;
    return (long)(r + (r >= 0 ? 0.5 : -0.5));
}

// SAPI event callback — runs on a SAPI worker thread. Translate
// End-Of-Input-Stream events into a WM_TTS_END_OF_STREAM PostMessage so
// the main thread advances to the next paragraph safely.
static void __stdcall OnSapiEvent(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    if (!g_voice) return;
    SPEVENT ev;
    ULONG fetched = 0;
    while (g_voice->GetEvents(1, &ev, &fetched) == S_OK && fetched > 0) {
        if (ev.eEventId == SPEI_END_INPUT_STREAM) {
            // v2.35 — carry the ended SAPI stream number so the consumer can tell
            // a natural end of the CURRENT utterance from a stale/purged stream
            // (navigation jump) — see DaisyOnTtsEndOfStream.
            if (g_hwnd) PostMessageW(g_hwnd, WM_TTS_END_OF_STREAM,
                                     (WPARAM)ev.ulStreamNum, 0);
        }
        // No SpClearEvent helper without sphelper — manually free the
        // event payload if it was an object/string.
        if (ev.elParamType == SPET_LPARAM_IS_TOKEN ||
            ev.elParamType == SPET_LPARAM_IS_OBJECT) {
            IUnknown* u = reinterpret_cast<IUnknown*>(ev.lParam);
            if (u) u->Release();
        } else if (ev.elParamType == SPET_LPARAM_IS_STRING ||
                   ev.elParamType == SPET_LPARAM_IS_POINTER) {
            if (ev.lParam) CoTaskMemFree(reinterpret_cast<void*>(ev.lParam));
        }
    }
}

// Replacement for sphelper.h's SpEnumTokens — uses ISpObjectTokenCategory
// directly so we don't need atlbase.h.
static HRESULT EnumVoiceTokens(IEnumSpObjectTokens** outEnum) {
    *outEnum = nullptr;
    ISpObjectTokenCategory* cat = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_ISpObjectTokenCategory,
                                  reinterpret_cast<void**>(&cat));
    if (FAILED(hr) || !cat) return hr;
    hr = cat->SetId(SPCAT_VOICES, FALSE);
    if (SUCCEEDED(hr)) {
        hr = cat->EnumTokens(nullptr, nullptr, outEnum);
    }
    cat->Release();
    return hr;
}

// Read a single string attribute off a token. Returns empty string on miss.
static std::wstring TokenStringValue(ISpObjectToken* tok, LPCWSTR keyPath,
                                     LPCWSTR valueName) {
    if (!tok) return L"";
    ISpDataKey* key = nullptr;
    HRESULT hr;
    if (keyPath && *keyPath) {
        hr = tok->OpenKey(keyPath, &key);
    } else {
        // Read directly off the token (which itself implements ISpDataKey).
        hr = tok->QueryInterface(IID_ISpDataKey, reinterpret_cast<void**>(&key));
    }
    if (FAILED(hr) || !key) return L"";
    std::wstring out;
    LPWSTR value = nullptr;
    if (SUCCEEDED(key->GetStringValue(valueName, &value)) && value) {
        out = TakeCoTaskString(value);
    }
    key->Release();
    return out;
}

// Best-effort friendly description for a voice token. SAPI tokens carry the
// display name as the default value under their root key. Falls back to the
// "Name" attribute, then to the raw id if nothing else is available.
static std::wstring TokenDisplayName(ISpObjectToken* tok) {
    if (!tok) return L"";
    // Default value of the root key — same thing sphelper's SpGetDescription
    // returns in 99 % of cases.
    std::wstring n = TokenStringValue(tok, nullptr, nullptr);
    if (!n.empty()) return n;
    n = TokenStringValue(tok, L"Attributes", L"Name");
    if (!n.empty()) return n;
    LPWSTR id = nullptr;
    if (SUCCEEDED(tok->GetId(&id)) && id) return TakeCoTaskString(id);
    return L"";
}

// -----------------------------------------------------------------------------
// Init / shutdown
// -----------------------------------------------------------------------------

bool TtsInit() {
    if (g_voice) return true;
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ISpVoice,
                                  reinterpret_cast<void**>(&g_voice));
    if (FAILED(hr) || !g_voice) {
        LogF("tts", "CoCreateInstance(SpVoice) failed hr=0x%08X", (unsigned)hr);
        return false;
    }
    // Ask for end-of-stream notifications and route them through OnSapiEvent.
    g_voice->SetInterest(SPFEI(SPEI_END_INPUT_STREAM), SPFEI(SPEI_END_INPUT_STREAM));
    g_voice->SetNotifyCallbackFunction(OnSapiEvent, 0, 0);

    // Apply saved voice if any.
    std::wstring saved = LoadVoiceIdFromIni();
    if (!saved.empty()) {
        TtsSetActiveVoiceId(saved);
    }
    return true;
}

void TtsShutdown() {
    if (g_voice) {
        g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        SafeRelease(g_voice);
    }
    g_voicesCache.clear();
    g_voicesCacheValid = false;
}

// -----------------------------------------------------------------------------
// Voice enumeration
// -----------------------------------------------------------------------------

std::vector<TtsVoice> TtsListVoices() {
    if (g_voicesCacheValid) return g_voicesCache;

    std::vector<TtsVoice> out;
    IEnumSpObjectTokens* en = nullptr;
    HRESULT hr = EnumVoiceTokens(&en);
    if (FAILED(hr) || !en) {
        LogF("tts", "EnumVoiceTokens failed hr=0x%08X", (unsigned)hr);
        g_voicesCache = out;
        g_voicesCacheValid = true;
        return out;
    }
    ULONG fetched = 0;
    ISpObjectToken* tok = nullptr;
    while (en->Next(1, &tok, &fetched) == S_OK && fetched > 0) {
        TtsVoice v;
        LPWSTR idStr = nullptr;
        if (SUCCEEDED(tok->GetId(&idStr)) && idStr) v.id = TakeCoTaskString(idStr);
        v.displayName = TokenDisplayName(tok);
        v.language    = TokenStringValue(tok, L"Attributes", L"Language");
        if (!v.id.empty() && !v.displayName.empty()) out.push_back(v);
        SafeRelease(tok);
    }
    SafeRelease(en);
    g_voicesCache      = out;
    g_voicesCacheValid = true;
    return out;
}

bool TtsSetActiveVoiceId(const std::wstring& id) {
    if (!g_voice) return false;
    if (id.empty()) {
        // Reset to SAPI default — leave the current voice in place and just
        // remember the cleared preference.
        g_activeVoiceId.clear();
        SaveVoiceIdToIni(L"");
        return true;
    }
    IEnumSpObjectTokens* en = nullptr;
    if (FAILED(EnumVoiceTokens(&en)) || !en) return false;
    ULONG fetched = 0;
    ISpObjectToken* tok = nullptr;
    bool ok = false;
    while (en->Next(1, &tok, &fetched) == S_OK && fetched > 0) {
        LPWSTR idStr = nullptr;
        if (SUCCEEDED(tok->GetId(&idStr)) && idStr) {
            std::wstring thisId = TakeCoTaskString(idStr);
            if (thisId == id) {
                HRESULT hr = g_voice->SetVoice(tok);
                if (SUCCEEDED(hr)) {
                    g_activeVoiceId = id;
                    SaveVoiceIdToIni(id);
                    ok = true;
                    SafeRelease(tok);
                    break;
                }
            }
        }
        SafeRelease(tok);
    }
    SafeRelease(en);
    return ok;
}

std::wstring TtsGetActiveVoiceId() { return g_activeVoiceId; }

// -----------------------------------------------------------------------------
// Speed
// -----------------------------------------------------------------------------

void TtsSetSpeedMultiplier(double m) {
    g_speedMultiplier = m;
    if (g_voice) g_voice->SetRate(MultiplierToSapiRate(m));
}

double TtsGetSpeedMultiplier() { return g_speedMultiplier; }

// -----------------------------------------------------------------------------
// Speech control
// -----------------------------------------------------------------------------

// v2.35 — SAPI stream number of the most recently started utterance. The
// END_INPUT_STREAM consumer compares the ended stream to this so a stale/purged
// stream (from a navigation jump) is not mistaken for a natural end. 0 = none.
static ULONG g_lastTtsStream = 0;

bool TtsSpeak(const std::wstring& text) {
    if (!g_voice) return false;
    if (text.empty()) return false;
    DWORD flags = SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML;
    ULONG streamNum = 0;
    // ISpVoice::Speak fills pulStreamNumber synchronously on return, even with
    // SPF_ASYNC — this is the number SAPI will report in the matching
    // SPEI_END_INPUT_STREAM event. Record it BEFORE returning to the caller so
    // any older pending purge end-event still carries the prior number.
    HRESULT hr = g_voice->Speak(text.c_str(), flags, &streamNum);
    if (SUCCEEDED(hr)) g_lastTtsStream = streamNum;
    return SUCCEEDED(hr);
}

unsigned long TtsLastStreamNumber() { return (unsigned long)g_lastTtsStream; }

void TtsStop() {
    if (!g_voice) return;
    g_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
}

void TtsPause() {
    if (!g_voice) return;
    g_voice->Pause();
}

void TtsResume() {
    if (!g_voice) return;
    g_voice->Resume();
}

bool TtsIsSpeaking() {
    if (!g_voice) return false;
    SPVOICESTATUS st{};
    if (FAILED(g_voice->GetStatus(&st, nullptr))) return false;
    return st.dwRunningState == SPRS_IS_SPEAKING;
}

bool TtsIsPaused() {
    if (!g_voice) return false;
    SPVOICESTATUS st{};
    if (FAILED(g_voice->GetStatus(&st, nullptr))) return false;
    return st.dwRunningState != SPRS_IS_SPEAKING && st.ulCurrentStream != 0;
}

} // namespace mediaaccess
