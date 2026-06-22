/*
 * keyboard_help.cpp — describe a key press for the F12 keyboard-help mode
 *
 * When g_keyboardHelpMode is on, the main WndProc routes every WM_KEYDOWN
 * through DescribeKey() instead of executing the bound action. The function
 * returns a localized "Shortcut: Action name" string for Speak().
 *
 * Since the introduction of the Actions/Keymap system, all bindings live
 * in the active keymap (mediaaccess::GetActiveKeyMap()). DescribeKey now
 * simply builds a Shortcut from the keypress and looks up the action.
 *
 * Output format examples (English / French):
 *   "Ctrl+O: Open file"          /  "Ctrl+O : Ouvrir un fichier"
 *   "Z: Previous track"          /  "Z : Piste précédente"
 *   "K: no action assigned"      /  "K : aucune action assignée"
 *   "F12: Keyboard help"         /  "F12 : Aide clavier"
 */

#include "mediaaccess/keyboard_help.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/keymap.h"
#include "mediaaccess/daisy_player.h"
#include "mediaaccess/translations.h"

#include <windows.h>
#include <string>

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static std::string KeyNameFromVK(UINT vk)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    bool ext = (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                vk == VK_HOME || vk == VK_END  || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_INSERT || vk == VK_DELETE);
    LONG lparam = (LONG)(scan << 16);
    if (ext) lparam |= (1L << 24);
    wchar_t buf[64] = {0};
    int n = GetKeyNameTextW(lparam, buf, 64);
    if (n <= 0) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string out(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], sz, nullptr, nullptr);
    return out;
}

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------
std::string DescribeKey(WPARAM wParam, LPARAM lParam)
{
    UINT vk = (UINT)wParam;

    // Stay silent when the pressed key IS a modifier alone (Ctrl, Shift,
    // Alt, Windows, Caps/Num/Scroll Lock). The user wants modifiers to be
    // announced only when held together with another key.
    switch (vk) {
        case VK_CONTROL:  case VK_LCONTROL: case VK_RCONTROL:
        case VK_SHIFT:    case VK_LSHIFT:   case VK_RSHIFT:
        case VK_MENU:     case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:     case VK_RWIN:
        case VK_CAPITAL:  case VK_NUMLOCK:  case VK_SCROLL:
            return std::string();
    }

    mediaaccess::Shortcut sc;
    sc.vk    = vk;
    sc.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    sc.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    sc.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    sc.win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
               ((GetKeyState(VK_RWIN) & 0x8000) != 0);  // v1.66

    std::string label = mediaaccess::ShortcutToDisplay(sc);
    if (label.empty()) label = KeyNameFromVK(vk);
    if (label.empty()) label = "?";

    // Mirror the dispatcher's priority: when a DAISY book is loaded, Books
    // category bindings take precedence so the description matches what
    // would actually happen if the user pressed the key.
    std::string actionId;
    if (mediaaccess::DaisyIsActive()) {
        actionId = mediaaccess::GetActiveKeyMap()
            .FindActionFor(sc, mediaaccess::ActionCategory::Books);
    }
    if (actionId.empty()) {
        actionId = mediaaccess::GetActiveKeyMap()
            .FindActionFor(sc, mediaaccess::ActionCategory::Main);
    }
    if (actionId.empty()) {
        return label + " : " + Ts("no action assigned");
    }
    const mediaaccess::Action* a = mediaaccess::ActionByStringId(actionId);
    if (!a) return label + " : " + Ts("no action assigned");
    return label + " : " + mediaaccess::ActionDisplayName(*a);
}

// -----------------------------------------------------------------------------
// Keyboard-help exit key
// -----------------------------------------------------------------------------
bool IsKeyboardHelpToggleKey(WPARAM wParam)
{
    UINT vk = (UINT)wParam;

    // F12 always works as an emergency exit so the user can never get stuck
    // in help mode, even if the action is unbound or rebound to a dead key.
    if (vk == VK_F12) return true;

    // Bare modifiers are never the toggle key on their own.
    switch (vk) {
        case VK_CONTROL:  case VK_LCONTROL: case VK_RCONTROL:
        case VK_SHIFT:    case VK_LSHIFT:   case VK_RSHIFT:
        case VK_MENU:     case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:     case VK_RWIN:
        case VK_CAPITAL:  case VK_NUMLOCK:  case VK_SCROLL:
            return false;
    }

    mediaaccess::Shortcut sc;
    sc.vk    = vk;
    sc.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    sc.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    sc.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    sc.win   = ((GetKeyState(VK_LWIN) & 0x8000) != 0) ||
               ((GetKeyState(VK_RWIN) & 0x8000) != 0);

    std::string actionId = mediaaccess::GetActiveKeyMap()
        .FindActionFor(sc, mediaaccess::ActionCategory::Main);
    return actionId == "KEYBOARD_HELP_TOGGLE";
}
