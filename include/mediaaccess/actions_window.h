#pragma once
#ifndef MEDIAACCESS_ACTIONS_WINDOW_H
#define MEDIAACCESS_ACTIONS_WINDOW_H

#include <windows.h>

namespace mediaaccess {

// Show the modal Actions / Keymap window (REAPER-style). Returns when the
// user closes it. All edits are persisted to the active keymap's source
// file immediately and reflected in menus.
void ShowActionsWindow(HWND owner);

} // namespace mediaaccess

#endif // MEDIAACCESS_ACTIONS_WINDOW_H
