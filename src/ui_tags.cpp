#include "ui_internal.h"

// Tag view dialog data
static std::wstring g_tagDialogText;
static const wchar_t* g_tagDialogTitle;

// Tag view dialog procedure
static INT_PTR CALLBACK TagViewDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            SetWindowTextW(hwnd, g_tagDialogTitle);
            SetDlgItemTextW(hwnd, IDC_TAG_TEXT, g_tagDialogText.c_str());
            // Select all text for easy copying
            SendDlgItemMessageW(hwnd, IDC_TAG_TEXT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_TAG_TEXT));
            return FALSE;  // Don't set default focus

        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, LOWORD(wParam));
                return TRUE;
            }
            break;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// Show tag in a dialog
void ShowTagDialog(const wchar_t* title, const std::wstring& text) {
    g_tagDialogTitle = title;
    g_tagDialogText = text;
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_TAG_VIEW), g_hwnd, TagViewDlgProc);
}
