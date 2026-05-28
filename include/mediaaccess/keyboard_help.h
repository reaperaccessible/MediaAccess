#pragma once
#ifndef MEDIAACCESS_KEYBOARD_HELP_H
#define MEDIAACCESS_KEYBOARD_HELP_H

#include <windows.h>
#include <string>

// Returns a localized human-readable description for the given key event.
// Intended for the F12-toggled "keyboard help" mode that announces what a
// key WOULD do instead of executing it.
//
// wParam / lParam are the standard WM_KEYDOWN parameters. The function
// inspects current modifier states (Shift/Ctrl/Alt) internally, looks up
// the binding, and returns either:
//   "Ctrl+O: Open file"               — known shortcut with a modifier
//   "Z: Previous track"               — known shortcut with no modifier
//   "K: no action assigned"           — key has no binding in MediaAccess
//
// All command names are translated via the T() / Ts() layer.
std::string DescribeKey(WPARAM wParam, LPARAM lParam);

#endif
