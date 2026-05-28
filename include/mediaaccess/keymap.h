#pragma once
#ifndef MEDIAACCESS_KEYMAP_H
#define MEDIAACCESS_KEYMAP_H

// =============================================================================
// Keymap data model and file I/O
//
// A KeyMap is the user-customizable mapping of keyboard shortcuts to actions.
// It is loaded from / saved to plain text files with the .MediaAccessKeyMap
// extension. Three regional defaults ship with the installer (USA, FR-CA,
// FR-FR) and the user can save unlimited custom keymaps.
//
// File format example:
//   # MediaAccess KeyMap v1
//   NAME=USA
//   REGION=en-US
//   PLAYER_PLAY_PAUSE = Space, P
//   PLAYER_PLAY = X
//   FILE_OPEN = Ctrl+O
// =============================================================================

#include "actions.h"

#include <map>
#include <string>
#include <vector>

namespace mediaaccess {

struct KeyMap {
    std::wstring path;             // Source file path (empty for built-in default)
    std::string  name;             // Display name (e.g. "USA", "FR-CA", "My custom")
    std::string  region;           // Locale tag (e.g. "en-US", "fr-CA", "fr-FR")

    // For each action string ID: the list of shortcuts assigned to it.
    // Multiple shortcuts per action are supported (REAPER-style).
    std::map<std::string, std::vector<Shortcut>> bindings;

    // Convenience: clears everything.
    void Clear();

    // Convenience: returns the list of shortcuts assigned to an action.
    // Returns empty vector if none.
    const std::vector<Shortcut>* GetShortcuts(const std::string& actionStringId) const;

    // Add a shortcut to an action. No-op if already present.
    void AddShortcut(const std::string& actionStringId, const Shortcut& s);

    // Remove a shortcut from an action. No-op if not present.
    void RemoveShortcut(const std::string& actionStringId, const Shortcut& s);

    // Find the first action whose binding matches the shortcut, scoped to
    // a single category (Main vs Radio vs YouTube — same key can be reused
    // across sections). Returns the action's string ID, or empty string.
    std::string FindActionFor(const Shortcut& s, ActionCategory cat) const;

    // Same as FindActionFor() but returns the IDM command ID. Returns 0 if
    // no binding matches.
    int FindCommandFor(const Shortcut& s, ActionCategory cat) const;
};

// =============================================================================
// File I/O
// =============================================================================

// Load a keymap from a .MediaAccessKeyMap text file. On error, returns an
// empty keymap and sets *errorOut (UTF-8) if non-null.
KeyMap LoadKeyMap(const std::wstring& path, std::string* errorOut = nullptr);

// Save a keymap to disk. Returns true on success.
bool SaveKeyMap(const std::wstring& path, const KeyMap& km, std::string* errorOut = nullptr);

// Build a KeyMap from the registry's defaultUsa fields. This is what USA.keymap
// would contain if it were generated rather than shipped.
KeyMap BuildDefaultUsaKeyMap();

// Build a KeyMap with French France adjustments — same as USA but with
// position-dependent letter keys remapped for AZERTY (Z→W swap, etc.).
KeyMap BuildDefaultFrFrKeyMap();

// Build a KeyMap with French Canadian adjustments. Canadian Multilingual
// Standard keyboard is essentially QWERTY so this is almost identical to USA.
KeyMap BuildDefaultFrCaKeyMap();

// =============================================================================
// Active-keymap state
// =============================================================================

// Returns a const reference to the active keymap. Safe to call any time.
const KeyMap& GetActiveKeyMap();

// Mutable accessor (used by the Actions dialog while editing). After
// modifications, call NotifyKeymapChanged() so menus and accelerators refresh.
KeyMap& GetActiveKeyMapMut();

// Replaces the active keymap and fires UI refresh. Persists the choice
// to MediaAccess.ini under [Actions] CurrentKeyMap.
void SetActiveKeyMap(const KeyMap& km);

// Call after mutating GetActiveKeyMapMut() in place. Rebuilds menus,
// menu accelerator hints, and persists the keymap to its source file.
void NotifyKeymapChanged();

// =============================================================================
// First-run / initialization
// =============================================================================

// Detects Windows keyboard layout and returns the appropriate default
// keymap name: "USA", "FR-CA", or "FR-FR". Returns "USA" for any layout
// we don't recognize.
std::string DetectDefaultKeyMapName();

// Returns the absolute path of a shipped keymap (in <install dir>\KeyMaps\).
std::wstring GetShippedKeyMapPath(const std::string& name);

// Returns the absolute path of a user keymap (in %APPDATA%\MediaAccess\KeyMaps\).
std::wstring GetUserKeyMapPath(const std::string& name);

// Ensures %APPDATA%\MediaAccess\KeyMaps\ exists. Called by LoadActiveKeymap().
void EnsureUserKeyMapsDir();

// Lists all available keymap names (union of shipped and user). Display
// name is the bare stem (file name without extension).
std::vector<std::string> ListAvailableKeyMaps();

// Reads [Actions] CurrentKeyMap from MediaAccess.ini, falls back to layout
// auto-detection, loads the file, and installs it as active. Generates
// the shipped defaults on disk if they're missing (so the user always sees
// USA/FR-CA/FR-FR even from a portable build without an installer).
void LoadActiveKeyMapAtStartup();

// =============================================================================
// Menu text refresh
// =============================================================================
// Re-walks the main window's menu and rewrites each item's accelerator hint
// (the part after "\t") with the shortcut currently bound to its IDM command.
// No-op if there's no main window yet.
void RefreshMenuAcceleratorHints();

} // namespace mediaaccess

#endif // MEDIAACCESS_KEYMAP_H
