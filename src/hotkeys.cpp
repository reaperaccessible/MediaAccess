// =============================================================================
// hotkeys.cpp — system-wide RegisterHotKey() integration.
//
// As of v1.41 the user-visible add/edit UI lives in the Actions/Keymap
// window (Tools → Actions, Global category). The functions in this file
// are now thin: load any legacy [Hotkeys] entries from MediaAccess.ini
// (then they get overwritten by the keymap sync), register/unregister all
// entries from the g_hotkeys vector with Windows, and register the
// always-on media keys (Play/Pause / Stop / Prev / Next).
//
// SaveHotkeys() / HotkeyDlgProc / FormatHotkey were removed — the keymap
// system owns persistence and editing entirely.
// =============================================================================

#include "hotkeys.h"
#include "globals.h"
#include "types.h"
#include "translations.h"
#include "resource.h"
#include <cstdio>

// Media key hotkey IDs (use high values to avoid conflicts)
#define HOTKEY_ID_MEDIA_PLAYPAUSE   0x7F00
#define HOTKEY_ID_MEDIA_STOP        0x7F01
#define HOTKEY_ID_MEDIA_PREV        0x7F02
#define HOTKEY_ID_MEDIA_NEXT        0x7F03

// Register all global hotkeys
void RegisterGlobalHotkeys() {
    if (!g_hwnd) return;

    // Always register media keys (no modifiers needed)
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PLAYPAUSE, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_STOP, 0, VK_MEDIA_STOP);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PREV, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_NEXT, 0, VK_MEDIA_NEXT_TRACK);

    // Register user-defined hotkeys from the keymap-driven g_hotkeys vector.
    if (!g_hotkeysEnabled) return;
    for (const auto& hk : g_hotkeys) {
        RegisterHotKey(g_hwnd, hk.id, hk.modifiers, hk.vk);
    }
}

// Unregister all global hotkeys
void UnregisterGlobalHotkeys() {
    if (!g_hwnd) return;

    // Unregister media keys
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PLAYPAUSE);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_STOP);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_PREV);
    UnregisterHotKey(g_hwnd, HOTKEY_ID_MEDIA_NEXT);

    // Unregister user-defined hotkeys
    for (const auto& hk : g_hotkeys) {
        UnregisterHotKey(g_hwnd, hk.id);
    }
}

// Load any legacy [Hotkeys] entries from MediaAccess.ini.
//
// In v1.41+ these entries get immediately overwritten by SyncGlobalHotkeysFromKeymap()
// which rebuilds g_hotkeys from the keymap's Global category. We keep this
// function purely as a defensive read so g_hotkeysEnabled is initialized
// from any pre-existing config.
void LoadHotkeys() {
    g_hotkeysEnabled = GetPrivateProfileIntW(L"Hotkeys", L"Enabled", 1, g_configPath.c_str()) != 0;
    g_hotkeys.clear();
    int count = GetPrivateProfileIntW(L"Hotkeys", L"Count", 0, g_configPath.c_str());

    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        wchar_t value[64] = {0};

        swprintf(key, 32, L"Hotkey%d", i);
        GetPrivateProfileStringW(L"Hotkeys", key, L"", value, 64, g_configPath.c_str());

        // Parse "modifiers,vk,actionIdx"
        UINT mods = 0, vk = 0;
        int actionIdx = 0;
        if (swscanf(value, L"%u,%u,%d", &mods, &vk, &actionIdx) == 3) {
            if (actionIdx >= 0 && actionIdx < g_hotkeyActionCount) {
                GlobalHotkey hk{};
                hk.id = g_nextHotkeyId++;
                hk.modifiers = mods;
                hk.vk = vk;
                hk.actionIdx = actionIdx;
                hk.commandId = 0;  // Legacy path uses actionIdx; commandId stays 0.
                g_hotkeys.push_back(hk);
            }
        }
    }
}
