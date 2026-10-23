#pragma once
#ifndef MEDIAACCESS_TRAY_H
#define MEDIAACCESS_TRAY_H

#include <windows.h>

// Tray icon management
void CreateTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowTrayMenu(HWND hwnd);

// Window visibility
void HideToTray(HWND hwnd);
void RestoreFromTray(HWND hwnd);
void ToggleWindow(HWND hwnd);

// v2.77 — the window that should take the foreground for |hwnd|: its open
// modal (update offer, message box...) when there is one, else |hwnd|.
// Activating a window disabled by a modal leaves the keyboard dead.
HWND ActiveWindowFor(HWND hwnd);

#endif // MEDIAACCESS_TRAY_H
