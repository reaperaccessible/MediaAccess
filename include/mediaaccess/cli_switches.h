#pragma once
#ifndef MEDIAACCESS_CLI_SWITCHES_H
#define MEDIAACCESS_CLI_SWITCHES_H

// =============================================================================
// cli_switches.h — Foobar2000-style command-line switch parsing and dispatch
//
// v1.63 (Jack's request). The launcher in main.cpp uses ParseCliSwitch() in
// the first pass over argv to separate fileless control commands from file
// arguments. Switches are then either:
//   - relayed to the existing instance via WM_COPYDATA (dwData=4, UTF-16
//     "verb\0param\0" payload) when one is running, or
//   - enqueued in g_pendingCliCommands and drained by ApplyCliSwitches()
//     after WM_CREATE finishes initial BASS setup, when this is the first
//     instance.
//
// Quit/hide with no instance are silently dropped — nothing to act on.
// =============================================================================

#include <windows.h>
#include <string>
#include <vector>

enum class CliVerb {
    Unknown,
    Play, Pause, Stop, Toggle, Next, Prev,
    VolUp, VolDown, Volume,
    Mute,
    SeekRel,    // param is signed seconds delta
    SeekAbs,    // param is unsigned seconds absolute
    Quit, Show, Hide,
    Slot        // v1.67 — switch to audio slot N (1-10)
};

struct CliCommand {
    CliVerb     verb       = CliVerb::Unknown;
    std::wstring param;     // raw string, parsed at dispatch time
    int          intParam  = 0;
    bool         hasIntParam = false;
};

// Parse one argv token like "/volume:50" or "/seek:+30" or "/play" into a
// CliCommand. Returns false (and leaves out untouched) on unknown verb.
bool ParseCliSwitch(const wchar_t* token, CliCommand& out);

// True when token looks like "/something" AND is in the known-switch
// whitelist. Used by main.cpp to disambiguate switch vs file path.
bool IsKnownCliSwitch(const wchar_t* token);

// Encode a CliCommand into a "verb\0param\0" UTF-16 buffer suitable for
// WM_COPYDATA cbData=4. Caller passes a std::wstring that's used as the
// underlying storage (kept alive across SendMessage).
void EncodeCliPayload(const CliCommand& cmd, std::wstring& out);

// Inverse of EncodeCliPayload — decode a WM_COPYDATA dwData=4 payload back
// into a CliCommand. Returns false if the payload is malformed.
bool DecodeCliPayload(const void* data, size_t cbData, CliCommand& out);

// Execute one CliCommand against the live MediaAccess instance. Must be
// called from the main thread (posts WM_COMMAND to g_hwnd internally).
// fromRemote=true means the command came in via WM_COPYDATA — used to
// gate /show /hide /quit announcements (silent for in-process queue
// drain, audible for cross-process scripting).
void ApplyCliCommand(HWND hwnd, const CliCommand& cmd, bool fromRemote);

// Drain g_pendingCliCommands. Called once after WM_CREATE finishes init.
void DrainPendingCliCommands(HWND hwnd);

// Defined in cli_switches.cpp; populated by the launcher when no other
// instance is running and the command line carries control switches.
extern std::vector<CliCommand> g_pendingCliCommands;

#endif // MEDIAACCESS_CLI_SWITCHES_H
