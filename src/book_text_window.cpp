// =============================================================================
// book_text_window.cpp — modeless text display window for DAISY/EPUB books
// =============================================================================

#include "mediaaccess/book_text_window.h"
#include "mediaaccess/translations.h"
#include "resource.h"

#include <windows.h>

extern std::wstring g_configPath;

namespace mediaaccess {

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static HWND          g_textHwnd  = nullptr;
static HWND          g_editHwnd  = nullptr;
static BookTextTheme g_theme     = BookTextTheme::Standard;
static HBRUSH        g_bgBrush   = nullptr;
static HFONT         g_font      = nullptr;
static std::wstring  g_pendingText;  // Set before window exists

// -----------------------------------------------------------------------------
// Theme application
// -----------------------------------------------------------------------------

struct ThemeColors {
    COLORREF bg;
    COLORREF fg;
    int      fontPt;
    const wchar_t* face;
};

static ThemeColors GetThemeColors(BookTextTheme t) {
    switch (t) {
        case BookTextTheme::HighContrast: return { RGB(0,0,0),     RGB(255,255,0), 16, L"Segoe UI" };
        case BookTextTheme::Large:        return { RGB(255,255,255), RGB(0,0,0),    24, L"Segoe UI" };
        case BookTextTheme::Standard:
        default:                          return { RGB(255,255,255), RGB(0,0,0),    12, L"Segoe UI" };
    }
}

static void RebuildThemeResources() {
    if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
    if (g_font)    { DeleteObject(g_font);    g_font    = nullptr; }

    ThemeColors c = GetThemeColors(g_theme);
    g_bgBrush = CreateSolidBrush(c.bg);

    HDC hdc = GetDC(nullptr);
    int h = -MulDiv(c.fontPt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    g_font = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, c.face);

    if (g_editHwnd && g_font) {
        SendMessageW(g_editHwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    if (g_textHwnd) {
        InvalidateRect(g_textHwnd, nullptr, TRUE);
        InvalidateRect(g_editHwnd, nullptr, TRUE);
    }
}

// -----------------------------------------------------------------------------
// INI persistence
// -----------------------------------------------------------------------------

static void LoadPrefs() {
    if (g_configPath.empty()) return;
    int t = GetPrivateProfileIntW(L"Books", L"TextTheme", 0, g_configPath.c_str());
    if (t < 0 || t > 2) t = 0;
    g_theme = (BookTextTheme)t;
}

static void SaveTheme() {
    if (g_configPath.empty()) return;
    wchar_t buf[8];
    swprintf(buf, 8, L"%d", (int)g_theme);
    WritePrivateProfileStringW(L"Books", L"TextTheme", buf, g_configPath.c_str());
}

bool BookTextWindowGetAlwaysHide() {
    if (g_configPath.empty()) return false;
    return GetPrivateProfileIntW(L"Books", L"HideTextWindow", 0,
                                 g_configPath.c_str()) != 0;
}

void BookTextWindowSetAlwaysHide(bool hide) {
    if (g_configPath.empty()) return;
    WritePrivateProfileStringW(L"Books", L"HideTextWindow",
                               hide ? L"1" : L"0", g_configPath.c_str());
}

// -----------------------------------------------------------------------------
// Dialog proc — applies theme colors via WM_CTLCOLOR* and handles resize
// -----------------------------------------------------------------------------

static INT_PTR CALLBACK TextWindowDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            g_editHwnd = GetDlgItem(dlg, IDC_BOOK_TEXT_EDIT);
            RebuildThemeResources();
            if (!g_pendingText.empty()) {
                SetWindowTextW(g_editHwnd, g_pendingText.c_str());
                g_pendingText.clear();
            }
            return TRUE;

        case WM_SIZE:
            // Stretch the edit control to fill the client area.
            if (g_editHwnd) {
                MoveWindow(g_editHwnd, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            }
            return 0;

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            ThemeColors c = GetThemeColors(g_theme);
            HDC hdc = (HDC)wp;
            SetTextColor(hdc, c.fg);
            SetBkColor  (hdc, c.bg);
            return (INT_PTR)(g_bgBrush ? g_bgBrush : GetStockObject(WHITE_BRUSH));
        }

        case WM_CLOSE:
            // Hide rather than destroy so the next Ctrl+T pops it back with
            // its current text intact.
            ShowWindow(dlg, SW_HIDE);
            return TRUE;

        case WM_DESTROY:
            g_textHwnd = nullptr;
            g_editHwnd = nullptr;
            return 0;
    }
    return FALSE;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

static void EnsureCreated(HWND owner) {
    if (g_textHwnd) return;
    LoadPrefs();
    g_textHwnd = CreateDialogW(GetModuleHandleW(nullptr),
                               MAKEINTRESOURCEW(IDD_BOOK_TEXT_WINDOW),
                               owner, TextWindowDlgProc);
}

void BookTextWindowShow(HWND owner) {
    EnsureCreated(owner);
    if (g_textHwnd) ShowWindow(g_textHwnd, SW_SHOW);
}

void BookTextWindowHide() {
    if (g_textHwnd) ShowWindow(g_textHwnd, SW_HIDE);
}

void BookTextWindowToggle(HWND owner) {
    if (!g_textHwnd) { BookTextWindowShow(owner); return; }
    if (IsWindowVisible(g_textHwnd)) BookTextWindowHide();
    else                              ShowWindow(g_textHwnd, SW_SHOW);
}

void BookTextWindowSetText(const std::wstring& text) {
    if (!g_editHwnd) {
        // Stash for the next time the window is created.
        g_pendingText = text;
        return;
    }
    SetWindowTextW(g_editHwnd, text.c_str());
    // Scroll to top so the user reads from the beginning.
    SendMessageW(g_editHwnd, EM_SETSEL, 0, 0);
    SendMessageW(g_editHwnd, EM_SCROLLCARET, 0, 0);
}

void BookTextWindowSetTheme(BookTextTheme t) {
    g_theme = t;
    SaveTheme();
    RebuildThemeResources();
}

BookTextTheme BookTextWindowGetTheme() {
    LoadPrefs();
    return g_theme;
}

void BookTextWindowDestroy() {
    if (g_textHwnd) DestroyWindow(g_textHwnd);
    if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
    if (g_font)    { DeleteObject(g_font);    g_font    = nullptr; }
}

} // namespace mediaaccess
