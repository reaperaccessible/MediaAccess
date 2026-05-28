/*
 * keyboard_help.cpp — describe a key press for the F12 keyboard-help mode
 *
 * When g_keyboardHelpMode is on, the main WndProc routes every WM_KEYDOWN
 * through DescribeKey() instead of executing the bound action. The function
 * returns a localized "Shortcut: Action name" string for Speak().
 *
 * Two layers of binding to honour:
 *   - Physical-position shortcuts (the Winamp transport row + a few more):
 *     keyed by hardware scan code, no modifiers. See main.cpp WM_KEYDOWN.
 *   - VK-based accelerators: keyed by virtual key + modifier flags,
 *     declared in MediaAccess.rc IDA_ACCEL.
 *
 * Order matters: the no-modifier physical-position table is checked first
 * (because those are the ones a French user pressing W on AZERTY actually
 * gets, with VK = VK_W but scan = 0x2C). If nothing matches, fall back to
 * the VK+modifier table.
 *
 * Output format examples (English / French):
 *   "Ctrl+O: Open file"          /  "Ctrl+O : Ouvrir un fichier"
 *   "Z: Previous track"          /  "Z : Piste précédente"
 *   "K: no action assigned"      /  "K : aucune action assignée"
 *   "F12: Keyboard help"         /  "F12 : Aide clavier"
 */

#include "mediaaccess/keyboard_help.h"
#include "mediaaccess/translations.h"
#include "resource.h"

#include <windows.h>
#include <string>

// -----------------------------------------------------------------------------
// Per-scan-code descriptions (no modifier). These mirror the WM_KEYDOWN
// scan-code dispatcher in main.cpp so the descriptions stay in sync.
// -----------------------------------------------------------------------------
static const char* DescribeScanCode(UINT scan)
{
    switch (scan) {
        case 0x2C: return "Previous track";          // physical Z
        case 0x2D: return "Play";                    // physical X
        case 0x2E: return "Pause";                   // physical C
        case 0x2F: return "Stop";                    // physical V
        case 0x30: return "Next track";              // physical B
        case 0x32: return "Add bookmark";            // physical M
        case 0x33: return "Decrease seek unit";      // physical ,
        case 0x34: return "Increase seek unit";      // physical .
        case 0x12: return "Cycle repeat mode";       // physical E
        case 0x13: return "Toggle recording";        // physical R
        case 0x16: return "Toggle mute";             // physical U
        case 0x19: return "Effect presets menu";     // physical P
        case 0x1A: return "Previous effect parameter"; // physical [
        case 0x1B: return "Next effect parameter";   // physical ]
        case 0x1E: return "Audio device menu";       // physical A
        case 0x23: return "Toggle shuffle";          // physical H
        case 0x24: return "Jump to time";            // physical J
        default:   return nullptr;
    }
}

// -----------------------------------------------------------------------------
// Per-VK descriptions, with modifiers. Each row encodes (vk, ctrl, shift, alt).
// -----------------------------------------------------------------------------
struct VKBinding {
    UINT vk;
    bool ctrl;
    bool shift;
    bool alt;
    const char* desc;
};

static const VKBinding kVkBindings[] = {
    // File menu — Ctrl + letter
    { 'O',           true,  false, false, "Open file" },
    { 'O',           true,  true,  false, "Add folder" },
    { 'P',           true,  false, false, "Playlist" },
    { 'U',           true,  false, false, "Open URL" },
    { 'Y',           true,  false, false, "YouTube" },
    { 'R',           true,  false, false, "Radio" },
    { 'D',           true,  false, false, "Add stream to favorites" },
    { 'P',           true,  true,  false, "Podcasts" },
    { 'S',           true,  false, false, "Schedule" },
    { 'H',           true,  false, false, "Hide to tray" },
    { 'H',           true,  true,  false, "Song history" },
    { 'V',           true,  false, false, "Paste from clipboard" },
    { VK_OEM_COMMA,  true,  false, false, "Options" },
    { 'M',           true,  false, false, "Bookmarks manager" },

    // Playback / movement / volume
    { VK_SPACE,      false, false, false, "Play/Pause" },
    { VK_HOME,       false, false, false, "Beginning of track" },
    { VK_LEFT,       false, false, false, "Seek backward" },
    { VK_RIGHT,      false, false, false, "Seek forward" },
    { VK_UP,         false, false, false, "Increase current parameter" },
    { VK_DOWN,       false, false, false, "Decrease current parameter" },
    { VK_UP,         true,  false, false, "Volume up" },
    { VK_DOWN,       true,  false, false, "Volume down" },
    { VK_BACK,       false, false, false, "Reset effect to default" },
    { VK_HOME,       true,  false, false, "Set effect to minimum" },
    { VK_END,        true,  false, false, "Set effect to maximum" },

    // On-demand speech
    { 'E',           true,  true,  false, "Speak elapsed time" },
    { 'R',           true,  true,  false, "Speak remaining time" },
    { 'T',           true,  true,  false, "Speak total duration" },

    // Effect toggles — Ctrl + number
    { '1',           true,  false, false, "Toggle Volume" },
    { '2',           true,  false, false, "Toggle Pitch" },
    { '3',           true,  false, false, "Toggle Tempo" },
    { '4',           true,  false, false, "Toggle Rate" },
    { '5',           true,  false, false, "Cycle Reverb algorithm" },
    { '6',           true,  false, false, "Toggle Echo" },
    { '7',           true,  false, false, "Toggle Equalizer" },
    { '8',           true,  false, false, "Toggle Compressor" },
    { '9',           true,  false, false, "Toggle Stereo Width" },
    { '0',           true,  false, false, "Toggle Center Cancel" },
    { VK_OEM_MINUS,  true,  false, false, "Toggle Convolution Reverb" },
    { VK_OEM_PLUS,   true,  false, false, "Toggle 3D Audio" },

    // Tag reading — bare number
    { '1',           false, false, false, "Speak title tag" },
    { '2',           false, false, false, "Speak artist tag" },
    { '3',           false, false, false, "Speak album tag" },
    { '4',           false, false, false, "Speak year tag" },
    { '5',           false, false, false, "Speak track number tag" },
    { '6',           false, false, false, "Speak genre tag" },
    { '7',           false, false, false, "Speak comment tag" },
    { '8',           false, false, false, "Speak bitrate tag" },
    { '9',           false, false, false, "Speak duration tag" },
    { '0',           false, false, false, "Speak filename" },

    // Tag display — Shift + number
    { '1',           false, true,  false, "Show title in window" },
    { '2',           false, true,  false, "Show artist in window" },
    { '3',           false, true,  false, "Show album in window" },
    { '4',           false, true,  false, "Show year in window" },
    { '5',           false, true,  false, "Show track number in window" },
    { '6',           false, true,  false, "Show genre in window" },
    { '7',           false, true,  false, "Show comment in window" },
    { '8',           false, true,  false, "Show bitrate in window" },
    { '9',           false, true,  false, "Show duration in window" },
    { '0',           false, true,  false, "Show filename in window" },

    // Video
    { VK_F11,        false, false, false, "Toggle fullscreen" },
    { 'T',           true,  true,  false, "Cycle subtitles" },
    { 'A',           true,  true,  false, "Cycle audio track" },
    { 'S',           true,  true,  false, "Take screenshot" },

    // Help
    { VK_F1,         false, false, false, "Open manual" },
    { VK_F12,        false, false, false, "Toggle keyboard help" },
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static std::string FormatModifierPrefix(bool ctrl, bool shift, bool alt)
{
    std::string out;
    if (ctrl)  out += Ts("KEY_MOD_CTRL")  + "+";
    if (shift) out += Ts("KEY_MOD_SHIFT") + "+";
    if (alt)   out += Ts("KEY_MOD_ALT")   + "+";
    return out;
}

static std::string KeyNameFromScan(UINT scan)
{
    // GetKeyNameTextW gives a layout-aware label for the physical key — so a
    // French user pressing the key in position 0x2C reads "W" (the AZERTY
    // letter at that spot) while an English user reads "Z".
    wchar_t buf[64] = {0};
    LONG lparam = (LONG)(scan << 16);
    int n = GetKeyNameTextW(lparam, buf, 64);
    if (n <= 0) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string out(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, &out[0], sz, nullptr, nullptr);
    return out;
}

static std::string KeyNameFromVK(UINT vk)
{
    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    // Some extended keys need the extended flag in bit 24 for a correct name.
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
    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

    UINT vk   = (UINT)wParam;
    UINT scan = (UINT)((lParam >> 16) & 0xFF);

    std::string prefix = FormatModifierPrefix(ctrl, shift, alt);

    // 1) No-modifier physical-position bindings first.
    if (!ctrl && !shift && !alt) {
        if (const char* desc = DescribeScanCode(scan)) {
            std::string keyName = KeyNameFromScan(scan);
            if (keyName.empty()) keyName = KeyNameFromVK(vk);
            return keyName + " : " + Ts(desc);
        }
    }

    // 2) VK + modifier table.
    for (const auto& b : kVkBindings) {
        if (b.vk == vk && b.ctrl == ctrl && b.shift == shift && b.alt == alt) {
            std::string keyName = KeyNameFromVK(vk);
            return prefix + keyName + " : " + Ts(b.desc);
        }
    }

    // 3) Unknown key — announce name + "no action assigned".
    std::string keyName = KeyNameFromVK(vk);
    if (keyName.empty()) keyName = KeyNameFromScan(scan);
    if (keyName.empty()) keyName = "?";
    return prefix + keyName + " : " + Ts("no action assigned");
}
