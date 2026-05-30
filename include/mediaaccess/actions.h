#pragma once
#ifndef MEDIAACCESS_ACTIONS_H
#define MEDIAACCESS_ACTIONS_H

// =============================================================================
// Action registry
//
// Single source of truth for every keyboard-accessible action in MediaAccess.
// Inspired by REAPER's Actions window: each action has a stable string ID
// (used in keymap files), a numeric command ID (sent via WM_COMMAND), a
// category (which "section" / context it belongs to), bilingual names, and
// an optional default keyboard shortcut.
//
// Adding a new action:
//   1) Add an entry in g_actions[] (src/actions.cpp).
//   2) Use a unique, stable string ID — never rename it after release, even
//      if the action's UI name changes. Existing keymap files refer to it.
//   3) The IDM_* command ID lives in resource.h.
// =============================================================================

#include <windows.h>
#include <string>
#include <vector>

namespace mediaaccess {

// REAPER-style "sections". Each section is an independent shortcut context:
// the same physical key can do different things depending on which window
// has focus.
enum class ActionCategory {
    Main    = 0,   // Main player window (default focus context)
    Radio   = 1,   // Radio search / favorites dialog
    YouTube = 2,   // YouTube search dialog
    Global  = 3,   // System-wide hotkeys (active even when MediaAccess has no focus)
    Books   = 4,   // DAISY / EPUB reader actions
    Count
};

// A single keyboard binding: virtual key + modifier flags.
//
// REAPER stores VK codes (layout-dependent). We do too — that's why we
// ship three regional keymaps (USA, FR-CA, FR-FR). The same Action can
// have multiple Shortcut entries (e.g. Space AND P both trigger Play/Pause).
//
// vk == 0 means "no shortcut".
struct Shortcut {
    UINT vk    = 0;
    bool ctrl  = false;
    bool shift = false;
    bool alt   = false;
    bool win   = false;  // v1.66 — Windows key modifier (Jack)

    bool valid() const { return vk != 0; }

    bool operator==(const Shortcut& o) const {
        return vk == o.vk && ctrl == o.ctrl && shift == o.shift &&
               alt == o.alt && win == o.win;
    }
};

// The catalog entry for one action.
struct Action {
    const char*     stringId;     // Stable ID for keymap files, e.g. "PLAYER_PLAY_PAUSE"
    int             commandId;    // IDM_* sent via WM_COMMAND when shortcut fires
    ActionCategory  category;
    const char*     nameEn;       // English display name
    const char*     nameFr;       // French display name
    Shortcut        defaultUsa;   // Default shortcut on USA keymap (vk==0 = none by default)
};

// =============================================================================
// Access to the registry
// =============================================================================

// Number of registered actions (size of g_actions[]).
int ActionCount();

// Pointer into the registry by index. Returns nullptr if out of range.
const Action* ActionAt(int index);

// Look up by stable string ID. Returns nullptr if not found.
const Action* ActionByStringId(const std::string& stringId);

// Look up by IDM command ID. Returns nullptr if not found.
const Action* ActionByCommandId(int commandId);

// Return all actions in a given category, in registry order.
std::vector<const Action*> ActionsInCategory(ActionCategory cat);

// Get the localized name for an action (English or French according to current
// MediaAccess UI language).
std::string ActionDisplayName(const Action& a);

// Get a human-readable category name in the current UI language.
std::string CategoryDisplayName(ActionCategory cat);

// =============================================================================
// Shortcut formatting / parsing helpers
// =============================================================================

// Format a Shortcut as a human-readable string like "Ctrl+Shift+O" using
// the current UI language for modifier prefixes and GetKeyNameTextW for the
// key label. Returns empty string when shortcut is invalid.
std::string ShortcutToDisplay(const Shortcut& s);

// Format a Shortcut for a .MediaAccessKeyMap file. Always English (canonical),
// e.g. "Ctrl+Shift+O", "F12", "Space", "VK0xBC". Designed to round-trip
// through ShortcutFromKeymapText().
std::string ShortcutToKeymapText(const Shortcut& s);

// Parse a Shortcut from a .MediaAccessKeyMap file. Returns invalid shortcut
// on parse failure.
Shortcut ShortcutFromKeymapText(const std::string& text);

} // namespace mediaaccess

#endif // MEDIAACCESS_ACTIONS_H
