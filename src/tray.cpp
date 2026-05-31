// =============================================================================
// tray.cpp — Notification-area (system tray) icon lifecycle.
//
// The tray icon is created on demand when the user minimizes to tray
// (HideToTray) and destroyed when the window comes back. Tray clicks are
// routed to the main wndproc via WM_TRAYICON (a WM_USER+N message defined
// in resource.h); the wndproc dispatches left-click → restore, right-click
// → ShowTrayMenu.
// =============================================================================

#include "tray.h"
#include "globals.h"
#include "resource.h"
#include "translations.h"
#include <shellapi.h>

void CreateTrayIcon(HWND hwnd) {
    if (g_trayIconVisible) return;

    // NOTIFYICONDATAW is sized once and lives in globals — Shell_NotifyIconW
    // identifies "our" icon by (hWnd, uID), so we must keep those stable
    // across the ADD / MODIFY / DELETE calls.
    g_trayIcon.cbSize = sizeof(NOTIFYICONDATAW);
    g_trayIcon.hWnd = hwnd;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wcscpy_s(g_trayIcon.szTip, APP_NAME);

    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);
    g_trayIconVisible = true;
}

void RemoveTrayIcon() {
    if (!g_trayIconVisible) return;

    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
    g_trayIconVisible = false;
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE,   T("&Restore"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_PLAYPAUSE, T("&Play/Pause"));
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_STOP,      T("&Stop"));
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_PREV,      T("P&revious"));
    AppendMenuW(hMenu, MF_STRING, IDM_PLAY_NEXT,      T("&Next"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT,      T("E&xit"));

    // SetForegroundWindow + trailing WM_NULL post is the classic MSDN
    // workaround for TrackPopupMenu: without it, the menu wouldn't dismiss
    // on a click outside its bounds (Q135788).
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessage(hwnd, WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

void HideToTray(HWND hwnd) {
    CreateTrayIcon(hwnd);
    ShowWindow(hwnd, SW_HIDE);
}

void RestoreFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
}

void ToggleWindow(HWND hwnd) {
    if (IsWindowVisible(hwnd)) {
        HideToTray(hwnd);
    } else {
        RestoreFromTray(hwnd);
    }
}
