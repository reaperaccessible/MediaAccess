#pragma once
#ifndef MEDIAACCESS_HOTKEYS_H
#define MEDIAACCESS_HOTKEYS_H

#include <windows.h>

// System-wide hotkey registration. Walks g_hotkeys + always-on media keys.
void RegisterGlobalHotkeys();
void UnregisterGlobalHotkeys();

// Read legacy [Hotkeys] section from MediaAccess.ini into g_hotkeys.
// In v1.41+ the keymap immediately overwrites this; kept for boot-time
// initialization of g_hotkeysEnabled.
void LoadHotkeys();

#endif // MEDIAACCESS_HOTKEYS_H
