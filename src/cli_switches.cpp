// =============================================================================
// cli_switches.cpp — Foobar2000-style command-line switch dispatch
//
// See cli_switches.h for the contract. This file implements the verb table
// and the receiver-side dispatcher.
// =============================================================================

#include "mediaaccess/cli_switches.h"
#include "mediaaccess/player.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/tray.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/audio_slots.h"  // v1.67 — ActivateAudioSlot for /slot:N
#include "resource.h"

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <vector>

// Pending CLI commands accumulated by the launcher when no other instance
// is running. Drained in WM_CREATE once init has finished.
std::vector<CliCommand> g_pendingCliCommands;

// Set true at the very top of WM_DESTROY so late CLI deliveries can be
// dropped before they reach a partially-torn-down BASS state.
extern bool g_isShuttingDown;

// Forward decls from player.cpp / main.cpp.
void Play();
void Pause();
void Stop();
void PlayPause();
void NextTrack();
void PrevTrack();
void VolumeUp();
void VolumeDown();
// SetVolume is declared in player.h (included above).
void ToggleMute();
void Seek(double seconds);
// SeekToPosition is declared in player.h (included above) — its v2.50 second
// parameter (announce) made a local redeclaration here ambiguous.
extern HWND g_hwnd;
extern bool g_allowAmplify;

// -----------------------------------------------------------------------------
// Verb table
// -----------------------------------------------------------------------------

namespace {

struct VerbDef {
    const wchar_t* name;       // ASCII lowercase, no leading '/'
    CliVerb        verb;
    bool           takesParam; // true for /volume, /seek
};

constexpr VerbDef kVerbs[] = {
    { L"play",    CliVerb::Play,    false },
    { L"pause",   CliVerb::Pause,   false },
    { L"stop",    CliVerb::Stop,    false },
    { L"toggle",  CliVerb::Toggle,  false },
    { L"next",    CliVerb::Next,    false },
    { L"prev",    CliVerb::Prev,    false },
    { L"volup",   CliVerb::VolUp,   false },
    { L"voldown", CliVerb::VolDown, false },
    { L"volume",  CliVerb::Volume,  true  },
    { L"mute",    CliVerb::Mute,    false },
    { L"seek",    CliVerb::SeekRel, true  },  // re-routed to SeekAbs below
    { L"quit",    CliVerb::Quit,    false },
    { L"show",    CliVerb::Show,    false },
    { L"hide",    CliVerb::Hide,    false },
    { L"slot",    CliVerb::Slot,    true  },  // v1.67 — /slot:N (1-10)
};

// Case-insensitive wide-string compare for ASCII-only verb names.
bool IEquals(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b)) return false;
        ++a; ++b;
    }
    return *a == 0 && *b == 0;
}

// Encode a verb back to its canonical lowercase name (used by the launcher
// when relaying to an existing instance via WM_COPYDATA).
const wchar_t* VerbName(CliVerb v) {
    for (const auto& d : kVerbs) {
        if (d.verb == v) return d.name;
    }
    // SeekRel and SeekAbs share the same wire name "seek"; the sign on
    // the parameter tells them apart at decode time.
    if (v == CliVerb::SeekAbs) return L"seek";
    return L"";
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Parsing
// -----------------------------------------------------------------------------

bool IsKnownCliSwitch(const wchar_t* token) {
    if (!token || token[0] != L'/') return false;
    CliCommand tmp;
    return ParseCliSwitch(token, tmp);
}

bool ParseCliSwitch(const wchar_t* token, CliCommand& out) {
    if (!token || token[0] != L'/') return false;
    // Split on ':' — left side is verb, right side is parameter.
    std::wstring rest = token + 1;
    std::wstring verbStr, paramStr;
    size_t colon = rest.find(L':');
    if (colon == std::wstring::npos) {
        verbStr = rest;
    } else {
        verbStr  = rest.substr(0, colon);
        paramStr = rest.substr(colon + 1);
    }
    if (verbStr.empty()) return false;

    for (const auto& d : kVerbs) {
        if (!IEquals(verbStr.c_str(), d.name)) continue;
        out.verb        = d.verb;
        out.param       = paramStr;
        out.hasIntParam = false;
        out.intParam    = 0;
        if (d.takesParam && !paramStr.empty()) {
            // Differentiate /seek:+30 (relative) from /seek:30 (absolute).
            bool signedParam = (paramStr[0] == L'+' || paramStr[0] == L'-');
            wchar_t* endp = nullptr;
            long v = std::wcstol(paramStr.c_str(), &endp, 10);
            if (endp && endp != paramStr.c_str()) {
                out.hasIntParam = true;
                out.intParam = (int)v;
            }
            if (out.verb == CliVerb::SeekRel && !signedParam) {
                out.verb = CliVerb::SeekAbs;
            }
        }
        return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Wire encoding for WM_COPYDATA dwData=4
// -----------------------------------------------------------------------------

void EncodeCliPayload(const CliCommand& cmd, std::wstring& out) {
    out.clear();
    const wchar_t* name = VerbName(cmd.verb);
    if (!name || !*name) return;
    out.append(name);
    out.push_back(L'\0');
    // For SeekRel make sure we preserve the sign — paramStr might already
    // contain it ("+30"), but if intParam was set without it, re-stringify.
    if (cmd.verb == CliVerb::SeekRel && !cmd.param.empty() &&
        cmd.param[0] != L'+' && cmd.param[0] != L'-') {
        wchar_t buf[32];
        std::swprintf(buf, 32, L"%+d", cmd.intParam);
        out.append(buf);
    } else {
        out.append(cmd.param);
    }
    out.push_back(L'\0');
}

bool DecodeCliPayload(const void* data, size_t cbData, CliCommand& out) {
    if (!data || cbData < 2 * sizeof(wchar_t)) return false;
    const wchar_t* verbPtr = static_cast<const wchar_t*>(data);
    size_t maxChars = cbData / sizeof(wchar_t);
    // Find verb terminator
    size_t verbLen = 0;
    while (verbLen < maxChars && verbPtr[verbLen] != L'\0') ++verbLen;
    if (verbLen == maxChars) return false;  // no NUL → malformed
    const wchar_t* paramPtr = verbPtr + verbLen + 1;
    size_t remaining = maxChars - verbLen - 1;
    size_t paramLen = 0;
    while (paramLen < remaining && paramPtr[paramLen] != L'\0') ++paramLen;
    // Param is allowed to be empty; missing trailing NUL is tolerated.

    // Reconstruct "/verb:param" and reuse ParseCliSwitch for one source of truth.
    std::wstring token = L"/";
    token.append(verbPtr, verbLen);
    if (paramLen > 0) {
        token.push_back(L':');
        token.append(paramPtr, paramLen);
    }
    return ParseCliSwitch(token.c_str(), out);
}

// -----------------------------------------------------------------------------
// Dispatch — runs on the main thread of the live instance
// -----------------------------------------------------------------------------

static void DoShow(HWND hwnd) {
    RestoreFromTray(hwnd);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void DoHide(HWND hwnd) {
    if (g_minimizeToTray) {
        HideToTray(hwnd);
    } else {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
}

void ApplyCliCommand(HWND hwnd, const CliCommand& cmd, bool fromRemote) {
    if (g_isShuttingDown) return;          // late delivery during WM_DESTROY
    if (!hwnd) return;

    switch (cmd.verb) {
        case CliVerb::Play:    PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_PLAY, 0);     break;
        case CliVerb::Pause:   PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_PAUSE, 0);    break;
        case CliVerb::Stop:    PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_STOP, 0);     break;
        case CliVerb::Toggle:  PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_PLAYPAUSE, 0);break;
        case CliVerb::Next:    PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_NEXT, 0);     break;
        case CliVerb::Prev:    PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_PREV, 0);     break;
        case CliVerb::VolUp:   PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_VOLUP, 0);    break;
        case CliVerb::VolDown: PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_VOLDOWN, 0);  break;
        case CliVerb::Mute:    PostMessageW(hwnd, WM_COMMAND, IDM_PLAY_MUTE, 0);     break;

        case CliVerb::Volume: {
            if (!cmd.hasIntParam) break;
            // 0..100 = normal range, 100..400 = amplify (only honoured when
            // the user has Options > Playback > Allow amplify enabled).
            constexpr int kMaxAmplifyPct = (int)(MAX_VOLUME_AMPLIFY * 100.0f);
            int pct = cmd.intParam;
            if (pct < 0)               pct = 0;
            if (pct > kMaxAmplifyPct)  pct = kMaxAmplifyPct;
            float maxVol = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
            float v = (float)pct / 100.0f;
            if (v > maxVol) v = maxVol;
            SetVolume(v);
            break;
        }

        case CliVerb::SeekRel:
            if (cmd.hasIntParam) Seek((double)cmd.intParam);
            break;

        case CliVerb::SeekAbs:
            if (cmd.hasIntParam) SeekToPosition((double)cmd.intParam);
            break;

        case CliVerb::Quit:
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            break;

        case CliVerb::Show:
            DoShow(hwnd);
            if (fromRemote) Speak(Ts("MediaAccess restored"));
            break;

        case CliVerb::Hide:
            DoHide(hwnd);
            if (fromRemote) Speak(Ts("MediaAccess hidden"));
            break;

        case CliVerb::Slot:
            // v1.67 — /slot:N switches to audio output slot N (1..kAudioSlotCount).
            // ActivateAudioSlot speaks success / "not configured" /
            // "device not found" itself.
            if (cmd.hasIntParam) {
                int slot = cmd.intParam;
                if (slot >= 1 && slot <= kAudioSlotCount) {
                    ActivateAudioSlot(slot - 1);
                }
            }
            break;

        case CliVerb::Unknown:
        default:
            break;
    }
}

void DrainPendingCliCommands(HWND hwnd) {
    if (g_pendingCliCommands.empty()) return;
    for (const auto& c : g_pendingCliCommands) {
        ApplyCliCommand(hwnd, c, /*fromRemote=*/false);
    }
    g_pendingCliCommands.clear();
}
