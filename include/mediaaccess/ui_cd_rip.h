#pragma once
// ui_cd_rip — the "Extract audio CD" window (modeless, like YouTube).
// Phase 1 of docs/PLAN_EXTRACTION_CD.md: drive, track list, eject.

#include <windows.h>

void ShowCdRipDialog(HWND parent);
HWND GetCdRipDialog();   // nullptr when it was never opened
// The window's own shortcuts (Ctrl+R, Ctrl+I, category CdRip). Called by the
// main message loop before IsDialogMessageW; true when the key was used.
bool CdRipTryShortcut(const MSG& msg);
