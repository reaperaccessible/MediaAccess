#pragma once
#ifndef MEDIAACCESS_AUDIO_SLOTS_H
#define MEDIAACCESS_AUDIO_SLOTS_H

// =============================================================================
// audio_slots.h — 10 user-configurable audio output presets
//
// v1.63 (Jack's request). Each slot remembers an output device by NAME so
// USB devices reconnecting on a different port still resolve. Configuration
// happens in Tools → Audio slots… (the AudioSlotsDialog modal). Hotkeys are
// assigned via Tools → Actions, Global category, actions GLOBAL_AUDIO_SLOT_1
// through GLOBAL_AUDIO_SLOT_10 plus GLOBAL_AUDIO_DEVICE_CYCLE and
// GLOBAL_AUDIO_DEVICE_SPEAK. No shortcuts are pre-assigned.
// =============================================================================

#include <windows.h>
#include <array>
#include <string>

constexpr int kAudioSlotCount = 10;

// Slot storage. Empty string = unassigned. Persisted under
// [AudioSlots] Slot1..Slot10 in MediaAccess.ini.
extern std::array<std::wstring, kAudioSlotCount> g_audioSlots;

// INI plumbing — called from settings.cpp's LoadSettings / SaveSettings.
void LoadAudioSlots();
void SaveAudioSlots();

// Open the configuration dialog modally. Owner is typically g_hwnd.
void ShowAudioSlotsDialog(HWND owner);

// Activate a slot (called from main.cpp WM_COMMAND for IDM_AUDIO_SLOT_N).
// Speaks "Slot N is not configured" if the slot is empty, "Device not
// found: <name>" if the saved device is no longer present, and lets
// SelectAudioDevice() handle the success announcement on switch.
void ActivateAudioSlot(int slotIndex);

// Walk through BASS-enabled devices in their natural order and switch to
// the next one after the currently-active device. Wraps around. Speaks
// the new device name via SelectAudioDevice's existing announcement.
void CycleAudioDevice();

// Speak the name of the currently-active output device (or "Default
// device" when nothing has been explicitly selected).
void SpeakCurrentAudioDevice();

#endif // MEDIAACCESS_AUDIO_SLOTS_H
