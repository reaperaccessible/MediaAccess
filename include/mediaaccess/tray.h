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

#endif // MEDIAACCESS_TRAY_H
