// =============================================================================
// audio_slots.cpp — 10 user-configurable audio output presets (v1.63)
//
// See audio_slots.h for the contract. This file owns:
//   - the in-memory slot table (g_audioSlots)
//   - the INI load/save plumbing under [AudioSlots]
//   - the modal dialog (IDD_AUDIO_SLOTS) for configuration
//   - the action handlers ActivateAudioSlot / CycleAudioDevice /
//     SpeakCurrentAudioDevice that the Global hotkeys trigger
// =============================================================================

#include "mediaaccess/audio_slots.h"
#include "mediaaccess/player.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/utils.h"
#include "bass.h"
#include "resource.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern std::wstring g_configPath;
extern std::wstring g_selectedDeviceName;
extern int          g_selectedDevice;

std::array<std::wstring, kAudioSlotCount> g_audioSlots;

// -----------------------------------------------------------------------------
// INI persistence
// -----------------------------------------------------------------------------

void LoadAudioSlots() {
    if (g_configPath.empty()) return;
    for (int i = 0; i < kAudioSlotCount; ++i) {
        wchar_t key[16];
        std::swprintf(key, 16, L"Slot%d", i + 1);
        wchar_t buf[512] = {0};
        GetPrivateProfileStringW(L"AudioSlots", key, L"", buf, 512,
                                 g_configPath.c_str());
        g_audioSlots[i] = buf;
    }
}

void SaveAudioSlots() {
    if (g_configPath.empty()) return;
    for (int i = 0; i < kAudioSlotCount; ++i) {
        wchar_t key[16];
        std::swprintf(key, 16, L"Slot%d", i + 1);
        // Passing nullptr would delete the key; we keep all 10 keys
        // present for clarity (empty string == unassigned).
        WritePrivateProfileStringW(L"AudioSlots", key,
                                   g_audioSlots[i].c_str(),
                                   g_configPath.c_str());
    }
}

// -----------------------------------------------------------------------------
// Action handlers (called from main.cpp WM_COMMAND)
// -----------------------------------------------------------------------------

void ActivateAudioSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= kAudioSlotCount) return;
    const std::wstring& name = g_audioSlots[slotIndex];
    if (name.empty()) {
        char msg[128];
        std::snprintf(msg, 128, "%s %d %s",
                      Ts("Slot").c_str(),
                      slotIndex + 1,
                      Ts("is not configured").c_str());
        Speak(msg);
        return;
    }
    int idx = FindDeviceByName(name);
    if (idx < 1) {
        std::string msg = Ts("Device not found: ") + WideToUtf8(name);
        Speak(msg);
        return;
    }
    // SelectAudioDevice handles the success announcement itself.
    SelectAudioDevice(idx);
}

void CycleAudioDevice() {
    // Walk BASS-enabled devices in their natural order.
    std::vector<int> enabled;
    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); ++i) {
        if (info.flags & BASS_DEVICE_ENABLED) enabled.push_back(i);
    }
    if (enabled.empty()) {
        Speak(Ts("No audio devices found"));
        return;
    }
    int curPos = -1;
    for (size_t k = 0; k < enabled.size(); ++k) {
        if (enabled[k] == g_selectedDevice) { curPos = (int)k; break; }
    }
    int next = enabled[(curPos + 1) % (int)enabled.size()];
    SelectAudioDevice(next);
}

void SpeakCurrentAudioDevice() {
    std::wstring name = g_selectedDeviceName;
    if (name.empty()) {
        Speak(Ts("Audio output: ") + Ts("Default device"));
        return;
    }
    std::string msg = Ts("Audio output: ") + WideToUtf8(name);
    Speak(msg);
}

// -----------------------------------------------------------------------------
// Configuration dialog — IDD_AUDIO_SLOTS
//
// Layout note (v1.61 lesson): MSAA derives the accessible name of an
// unnamed control from the LTEXT immediately preceding it in source order.
// The .rc file (MediaAccess.rc) places each "Slot N:" LTEXT immediately
// before its IDC_AUDIO_SLOT_BASE+N combo for this reason. Don't reorder.
// -----------------------------------------------------------------------------

namespace {

// Snapshot of the current device list at dialog open. Indexed by combo
// item-data; saving a slot stores the device NAME (not the index) so
// USB reordering between sessions doesn't break bindings.
struct DeviceEntry {
    int          bassIndex;     // 1-based BASS device index
    std::wstring name;
};

// For each combo, we cache the slot's original (saved) device name so we
// can preserve "(disconnected) X" entries when the user doesn't touch
// that combo. Otherwise unplugging a USB device + opening the dialog +
// pressing OK would silently wipe the binding.
struct SlotState {
    std::wstring originalName;
    bool         hasDisconnectedItem = false;
};

struct DialogState {
    std::vector<DeviceEntry> devices;
    SlotState                slots[kAudioSlotCount];
};
static DialogState* s_state = nullptr;

void EnumerateDevices(std::vector<DeviceEntry>& out) {
    out.clear();
    BASS_DEVICEINFO info;
    for (int i = 1; BASS_GetDeviceInfo(i, &info); ++i) {
        if (!(info.flags & BASS_DEVICE_ENABLED)) continue;
        DeviceEntry e;
        e.bassIndex = i;
        if (info.name) {
            int n = MultiByteToWideChar(CP_UTF8, 0, info.name, -1, nullptr, 0);
            if (n > 1) {
                e.name.resize(n - 1);
                MultiByteToWideChar(CP_UTF8, 0, info.name, -1, &e.name[0], n);
            }
        }
        if (e.name.empty()) {
            wchar_t buf[32];
            std::swprintf(buf, 32, L"Device %d", i);
            e.name = buf;
        }
        out.push_back(std::move(e));
    }
}

void PopulateCombo(HWND combo, const std::vector<DeviceEntry>& devs,
                   const std::wstring& currentName,
                   SlotState& slot) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int noneIdx = (int)SendMessageW(combo, CB_ADDSTRING, 0,
                                    (LPARAM)T("(None)"));
    SendMessageW(combo, CB_SETITEMDATA, noneIdx, (LPARAM)(-1));

    for (size_t i = 0; i < devs.size(); ++i) {
        int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0,
                                    (LPARAM)devs[i].name.c_str());
        SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM)i);
    }

    slot.originalName = currentName;
    slot.hasDisconnectedItem = false;

    if (currentName.empty()) {
        SendMessageW(combo, CB_SETCURSEL, noneIdx, 0);
        return;
    }
    // Try to match by name.
    int found = (int)SendMessageW(combo, CB_FINDSTRINGEXACT, (WPARAM)-1,
                                  (LPARAM)currentName.c_str());
    if (found != CB_ERR) {
        SendMessageW(combo, CB_SETCURSEL, found, 0);
        return;
    }
    // Saved device isn't in current list — show as disconnected so the
    // user understands AND we don't wipe the binding on OK.
    std::wstring label = T("(disconnected) ");
    label += currentName;
    int idx = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    SendMessageW(combo, CB_SETITEMDATA, idx, (LPARAM)(-2));
    SendMessageW(combo, CB_SETCURSEL, idx, 0);
    slot.hasDisconnectedItem = true;
}

void PopulateAllCombos(HWND hwnd) {
    if (!s_state) return;
    EnumerateDevices(s_state->devices);
    for (int i = 0; i < kAudioSlotCount; ++i) {
        HWND combo = GetDlgItem(hwnd, IDC_AUDIO_SLOT_BASE + i);
        PopulateCombo(combo, s_state->devices, g_audioSlots[i],
                      s_state->slots[i]);
    }
}

void HarvestSlots(HWND hwnd) {
    if (!s_state) return;
    for (int i = 0; i < kAudioSlotCount; ++i) {
        HWND combo = GetDlgItem(hwnd, IDC_AUDIO_SLOT_BASE + i);
        int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (sel == CB_ERR) { g_audioSlots[i].clear(); continue; }
        LPARAM data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
        if (data == (LPARAM)(-1)) {
            g_audioSlots[i].clear();
        } else if (data == (LPARAM)(-2)) {
            // Disconnected — keep original name so it auto-rebinds.
            g_audioSlots[i] = s_state->slots[i].originalName;
        } else {
            size_t devIdx = (size_t)data;
            if (devIdx < s_state->devices.size()) {
                g_audioSlots[i] = s_state->devices[devIdx].name;
            } else {
                g_audioSlots[i].clear();
            }
        }
    }
    SaveAudioSlots();
}

INT_PTR CALLBACK AudioSlotsDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM /*lp*/) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            PopulateAllCombos(hwnd);
            SetFocus(GetDlgItem(hwnd, IDC_AUDIO_SLOT_BASE + 0));
            return FALSE;  // we set focus ourselves
        case WM_COMMAND: {
            WORD id = LOWORD(wp);
            if (id == IDOK) {
                HarvestSlots(hwnd);
                EndDialog(hwnd, IDOK);
                Speak(Ts("Audio slots saved"));
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            if (id == IDC_AUDIO_SLOTS_REFRESH) {
                // Re-enumerate without losing the user's in-flight choices.
                // Re-pull current state into g_audioSlots-ish snapshot first
                // (so unsaved selections survive the rebuild).
                std::wstring snapshot[kAudioSlotCount];
                for (int i = 0; i < kAudioSlotCount; ++i) {
                    HWND combo = GetDlgItem(hwnd, IDC_AUDIO_SLOT_BASE + i);
                    int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
                    if (sel == CB_ERR) continue;
                    LPARAM data = SendMessageW(combo, CB_GETITEMDATA, sel, 0);
                    if (data == (LPARAM)(-1)) {
                        snapshot[i].clear();
                    } else if (data == (LPARAM)(-2)) {
                        snapshot[i] = s_state->slots[i].originalName;
                    } else {
                        size_t devIdx = (size_t)data;
                        if (s_state && devIdx < s_state->devices.size()) {
                            snapshot[i] = s_state->devices[devIdx].name;
                        }
                    }
                }
                if (s_state) {
                    EnumerateDevices(s_state->devices);
                    for (int i = 0; i < kAudioSlotCount; ++i) {
                        HWND combo = GetDlgItem(hwnd, IDC_AUDIO_SLOT_BASE + i);
                        PopulateCombo(combo, s_state->devices,
                                      snapshot[i], s_state->slots[i]);
                    }
                }
                Speak(Ts("Device list refreshed"));
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

} // anonymous namespace

void ShowAudioSlotsDialog(HWND owner) {
    DialogState st;
    s_state = &st;
    DialogBoxW(GetModuleHandleW(nullptr),
               MAKEINTRESOURCEW(IDD_AUDIO_SLOTS),
               owner, AudioSlotsDlgProc);
    s_state = nullptr;
}
