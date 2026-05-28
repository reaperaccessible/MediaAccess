// =============================================================================
// actions_window.cpp — REAPER-style Actions / Keymap dialog
//
// Layout (see IDD_ACTIONS in MediaAccess.rc):
//   - Section combo (Main / Radio / YouTube / Global)
//   - Filter edit box
//   - Actions listbox  (Action name + first bound shortcut between brackets)
//   - Shortcuts listbox for selected action
//   - Add / Edit / Delete buttons
//   - Keymap name label
//   - Load / Save As / Reset to defaults / Close buttons
// =============================================================================

#include "mediaaccess/actions_window.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/keymap.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/accessibility.h"  // Speak()
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace mediaaccess {

// =============================================================================
// State held by the open dialog
// =============================================================================

struct DialogState {
    ActionCategory             currentCategory = ActionCategory::Main;
    std::vector<const Action*> visibleActions;  // After filter applied
    std::string                searchText;
};
static DialogState* s_state = nullptr;

// =============================================================================
// Utility: UTF-8 ↔ wide
// =============================================================================

static std::wstring U8ToW(const std::string& s)
{
    if (s.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (sz <= 1) return L"";
    std::wstring w(sz - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], sz);
    return w;
}

static std::string WToU8(const std::wstring& w)
{
    if (w.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], sz, nullptr, nullptr);
    return s;
}

static std::string ToLowerAscii(const std::string& s)
{
    std::string out = s;
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

// =============================================================================
// Shortcut-learning sub-dialog
// =============================================================================
//
// The dialog hosts a read-only edit control that we subclass: every key press
// is captured and turned into a Shortcut. The user confirms with OK or cancels.
// Pure modifier keys are ignored. Backspace alone clears the capture.

struct AssignState {
    Shortcut captured;
    WNDPROC  origEditProc = nullptr;
    HWND     editHwnd     = nullptr;
};
static AssignState* s_assign = nullptr;

static void UpdateAssignDisplay(HWND /*dlg*/)
{
    if (!s_assign || !s_assign->editHwnd) return;
    std::string utf8;
    std::wstring text;
    if (s_assign->captured.valid()) {
        utf8 = ShortcutToDisplay(s_assign->captured);
        text = U8ToW(utf8);
    }
    // Write to the captured edit control directly so both the assign and
    // the find dialogs share the same update path.
    SetWindowTextW(s_assign->editHwnd, text.c_str());
    // Read-only edits don't fire UIA value-changed events for screen readers
    // when set programmatically, so the user hears nothing when they capture
    // a key. Speak it explicitly so NVDA / JAWS / Narrator announce the
    // capture in real time instead of forcing the user to Tab to OK first.
    if (!utf8.empty()) Speak(utf8);
}

static bool IsModifierVK(UINT vk)
{
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:
        case VK_MENU:    case VK_LMENU:    case VK_RMENU:
        case VK_LWIN:    case VK_RWIN:
        case VK_CAPITAL: case VK_NUMLOCK:  case VK_SCROLL:
            return true;
    }
    return false;
}

static LRESULT CALLBACK AssignEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!s_assign) {
        return CallWindowProcW(DefWindowProcW, hwnd, msg, wp, lp);
    }
    HWND dlg = GetParent(hwnd);
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            UINT vk = (UINT)wp;
            // Allow Tab to move focus normally (we don't want to capture it).
            if (vk == VK_TAB) break;
            if (IsModifierVK(vk)) return 0;
            // Backspace alone clears.
            if (vk == VK_BACK) {
                bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
                bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
                if (!ctrl && !shift && !alt) {
                    s_assign->captured = Shortcut{};
                    UpdateAssignDisplay(dlg);
                    return 0;
                }
            }
            Shortcut s;
            s.vk    = vk;
            s.ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            s.shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
            s.alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
            s_assign->captured = s;
            UpdateAssignDisplay(dlg);
            return 0;
        }
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0; // Swallow — we use WM_KEYDOWN instead.
    }
    return CallWindowProcW(s_assign->origEditProc, hwnd, msg, wp, lp);
}

static INT_PTR CALLBACK AssignDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(dlg);
            if (!s_assign) return TRUE;
            s_assign->editHwnd = GetDlgItem(dlg, IDC_SHORTCUT_DISPLAY);
            s_assign->origEditProc = (WNDPROC)SetWindowLongPtrW(
                s_assign->editHwnd, GWLP_WNDPROC, (LONG_PTR)AssignEditProc);
            UpdateAssignDisplay(dlg);
            SetFocus(s_assign->editHwnd);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK)     { EndDialog(dlg, IDOK);     return TRUE; }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
            break;
        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// Show the learn-a-shortcut dialog. Returns true if the user confirmed and
// out is populated with a valid shortcut.
static bool PromptForShortcut(HWND owner, Shortcut* out)
{
    AssignState st;
    s_assign = &st;
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDD_SHORTCUT_ASSIGN), owner, AssignDlgProc);
    bool ok = (r == IDOK && st.captured.valid());
    if (ok && out) *out = st.captured;
    s_assign = nullptr;
    return ok;
}

// =============================================================================
// Find-by-shortcut sub-dialog
//
// Same key-capture mechanic as PromptForShortcut, but a different dialog
// template and a "Search" default button instead of OK. The user captures
// a shortcut, Tabs to Search (or presses Enter), and we jump to the matching
// action in the Actions window.
// =============================================================================

static INT_PTR CALLBACK FindShortcutDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(dlg);
            if (!s_assign) return TRUE;
            s_assign->editHwnd = GetDlgItem(dlg, IDC_FIND_SHORTCUT_DISPLAY);
            s_assign->origEditProc = (WNDPROC)SetWindowLongPtrW(
                s_assign->editHwnd, GWLP_WNDPROC, (LONG_PTR)AssignEditProc);
            // Reuse the same display-update helper — but it looks up
            // IDC_SHORTCUT_DISPLAY by name. We inline the equivalent for
            // IDC_FIND_SHORTCUT_DISPLAY here.
            SetDlgItemTextW(dlg, IDC_FIND_SHORTCUT_DISPLAY, L"");
            SetFocus(s_assign->editHwnd);
            return FALSE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_FIND_SHORTCUT_SEARCH) {
                EndDialog(dlg, IDC_FIND_SHORTCUT_SEARCH);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

// We need a version of UpdateAssignDisplay that targets IDC_FIND_SHORTCUT_DISPLAY.
// Easiest: patch UpdateAssignDisplay to write to whatever editHwnd is set, by
// SetWindowText on the captured edit handle directly.
//
// AssignEditProc currently calls UpdateAssignDisplay(GetParent(hwnd)) which
// expects IDC_SHORTCUT_DISPLAY. To make the find dialog work without
// duplicating the edit subclass, we tweak UpdateAssignDisplay to write
// to the registered editHwnd directly.
//
// (This change applies retroactively to PromptForShortcut too — same
// behavior, just routed through the handle instead of the dialog control ID.)

// =============================================================================
// Population helpers
// =============================================================================

static void PopulateCategoryCombo(HWND dlg)
{
    HWND combo = GetDlgItem(dlg, IDC_ACTIONS_CATEGORY);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0,
        (LPARAM)U8ToW(CategoryDisplayName(ActionCategory::Main)).c_str());
    SendMessageW(combo, CB_ADDSTRING, 0,
        (LPARAM)U8ToW(CategoryDisplayName(ActionCategory::Radio)).c_str());
    SendMessageW(combo, CB_ADDSTRING, 0,
        (LPARAM)U8ToW(CategoryDisplayName(ActionCategory::YouTube)).c_str());
    SendMessageW(combo, CB_ADDSTRING, 0,
        (LPARAM)U8ToW(CategoryDisplayName(ActionCategory::Global)).c_str());
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)(int)s_state->currentCategory, 0);
}

static std::string FormatActionLine(const Action* a)
{
    std::string line = ActionDisplayName(*a);
    const KeyMap& km = GetActiveKeyMap();
    auto it = km.bindings.find(a->stringId);
    if (it != km.bindings.end() && !it->second.empty()) {
        line += "  [";
        for (size_t i = 0; i < it->second.size(); ++i) {
            if (i > 0) line += ", ";
            line += ShortcutToDisplay(it->second[i]);
        }
        line += "]";
    }
    return line;
}

static void PopulateActionsList(HWND dlg)
{
    if (!s_state) return;
    HWND list = GetDlgItem(dlg, IDC_ACTIONS_LIST);

    // Remember selected action's string ID so we can re-select after refresh.
    int curSel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    std::string keepStringId;
    if (curSel >= 0 && curSel < (int)s_state->visibleActions.size())
        keepStringId = s_state->visibleActions[curSel]->stringId;

    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    s_state->visibleActions = ActionsInCategory(s_state->currentCategory);

    // Apply filter (substring match, case-insensitive, on action name + bound shortcut text)
    if (!s_state->searchText.empty()) {
        std::string needle = ToLowerAscii(s_state->searchText);
        std::vector<const Action*> filtered;
        filtered.reserve(s_state->visibleActions.size());
        for (const Action* a : s_state->visibleActions) {
            std::string name = ToLowerAscii(ActionDisplayName(*a));
            bool match = name.find(needle) != std::string::npos;
            if (!match) {
                const KeyMap& km = GetActiveKeyMap();
                auto it = km.bindings.find(a->stringId);
                if (it != km.bindings.end()) {
                    for (const auto& sh : it->second) {
                        std::string t = ToLowerAscii(ShortcutToDisplay(sh));
                        if (t.find(needle) != std::string::npos) { match = true; break; }
                    }
                }
            }
            if (match) filtered.push_back(a);
        }
        s_state->visibleActions = std::move(filtered);
    }

    int reSelect = -1;
    for (size_t i = 0; i < s_state->visibleActions.size(); ++i) {
        std::string line = FormatActionLine(s_state->visibleActions[i]);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)U8ToW(line).c_str());
        if (!keepStringId.empty() && keepStringId == s_state->visibleActions[i]->stringId)
            reSelect = (int)i;
    }
    if (reSelect < 0 && !s_state->visibleActions.empty()) reSelect = 0;
    if (reSelect >= 0) SendMessageW(list, LB_SETCURSEL, reSelect, 0);
}

static const Action* GetSelectedAction(HWND dlg)
{
    if (!s_state) return nullptr;
    HWND list = GetDlgItem(dlg, IDC_ACTIONS_LIST);
    int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)s_state->visibleActions.size()) return nullptr;
    return s_state->visibleActions[sel];
}

static void PopulateShortcutsList(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_ACTIONS_SHORTCUTS);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const Action* a = GetSelectedAction(dlg);
    if (!a) return;
    const KeyMap& km = GetActiveKeyMap();
    auto it = km.bindings.find(a->stringId);
    if (it == km.bindings.end()) return;
    for (const auto& sh : it->second) {
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)U8ToW(ShortcutToDisplay(sh)).c_str());
    }
    if (!it->second.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
}

static void UpdateKeymapLabel(HWND dlg)
{
    std::wstring n = U8ToW(GetActiveKeyMap().name);
    SetDlgItemTextW(dlg, IDC_ACTIONS_KEYMAP_NAME, n.c_str());
}

// =============================================================================
// Add / Edit / Delete logic with REAPER-style conflict prompt
// =============================================================================

static bool ResolveConflictAndAssign(HWND dlg, const Action* target, const Shortcut& sc)
{
    // Look for existing binding in the same category.
    KeyMap& km = GetActiveKeyMapMut();
    std::string existingId = km.FindActionFor(sc, target->category);
    if (!existingId.empty() && existingId != target->stringId) {
        const Action* existing = ActionByStringId(existingId);
        std::string msg;
        if (existing) {
            msg = Ts("This shortcut is already assigned to") + ":\n  " +
                  ActionDisplayName(*existing) +
                  "\n\n" + Ts("Replace the existing assignment?");
        } else {
            msg = Ts("This shortcut is already assigned. Replace it?");
        }
        std::wstring wmsg = U8ToW(msg);
        int r = MessageBoxW(dlg, wmsg.c_str(), U8ToW(Ts("Shortcut conflict")).c_str(),
                            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return false;
        if (r == IDYES) {
            km.RemoveShortcut(existingId, sc);
        }
        // IDNO = keep both. Falls through to assign.
    }
    km.AddShortcut(target->stringId, sc);
    NotifyKeymapChanged();
    return true;
}

static void OnAdd(HWND dlg)
{
    const Action* a = GetSelectedAction(dlg);
    if (!a) return;
    Shortcut sc;
    if (!PromptForShortcut(dlg, &sc)) return;
    if (!ResolveConflictAndAssign(dlg, a, sc)) return;
    PopulateActionsList(dlg);
    PopulateShortcutsList(dlg);
}

static void OnEdit(HWND dlg)
{
    const Action* a = GetSelectedAction(dlg);
    if (!a) return;
    HWND scList = GetDlgItem(dlg, IDC_ACTIONS_SHORTCUTS);
    int sel = (int)SendMessageW(scList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    KeyMap& km = GetActiveKeyMapMut();
    auto it = km.bindings.find(a->stringId);
    if (it == km.bindings.end() || sel >= (int)it->second.size()) return;
    Shortcut old = it->second[sel];
    Shortcut nu;
    if (!PromptForShortcut(dlg, &nu)) return;
    if (nu == old) return;
    km.RemoveShortcut(a->stringId, old);
    if (!ResolveConflictAndAssign(dlg, a, nu)) {
        // User cancelled the conflict — restore the old binding.
        km.AddShortcut(a->stringId, old);
        NotifyKeymapChanged();
    }
    PopulateActionsList(dlg);
    PopulateShortcutsList(dlg);
}

static void OnDelete(HWND dlg)
{
    const Action* a = GetSelectedAction(dlg);
    if (!a) return;
    HWND scList = GetDlgItem(dlg, IDC_ACTIONS_SHORTCUTS);
    int sel = (int)SendMessageW(scList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return;
    KeyMap& km = GetActiveKeyMapMut();
    auto it = km.bindings.find(a->stringId);
    if (it == km.bindings.end() || sel >= (int)it->second.size()) return;
    Shortcut s = it->second[sel];
    km.RemoveShortcut(a->stringId, s);
    NotifyKeymapChanged();
    PopulateActionsList(dlg);
    PopulateShortcutsList(dlg);
}

// =============================================================================
// Load / Save As / Reset
// =============================================================================

static void OnLoad(HWND dlg)
{
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH] = L"";
    std::wstring initDir = GetUserKeyMapPath("").c_str();
    // Strip trailing filename portion to use folder as initial dir.
    {
        size_t slash = initDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) initDir = initDir.substr(0, slash);
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = dlg;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"MediaAccess KeyMap\0*.MediaAccessKeyMap\0All files\0*.*\0";
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    std::string err;
    KeyMap km = LoadKeyMap(buf, &err);
    if (km.bindings.empty()) {
        std::wstring msg = U8ToW(Ts("Could not load keymap") + (err.empty() ? "" : (": " + err)));
        MessageBoxW(dlg, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONWARNING);
        return;
    }
    SetActiveKeyMap(km);
    UpdateKeymapLabel(dlg);
    PopulateActionsList(dlg);
    PopulateShortcutsList(dlg);
}

static void OnSaveAs(HWND dlg)
{
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH] = L"";
    {
        std::wstring def = U8ToW(GetActiveKeyMap().name + ".MediaAccessKeyMap");
        wcsncpy_s(buf, def.c_str(), _TRUNCATE);
    }
    std::wstring initDir = GetUserKeyMapPath("").c_str();
    {
        size_t slash = initDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) initDir = initDir.substr(0, slash);
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = dlg;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"MediaAccess KeyMap\0*.MediaAccessKeyMap\0";
    ofn.lpstrDefExt = L"MediaAccessKeyMap";
    ofn.lpstrInitialDir = initDir.c_str();
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetSaveFileNameW(&ofn)) return;

    KeyMap toSave = GetActiveKeyMap();
    // Derive name from filename.
    std::wstring stem = buf;
    size_t slash = stem.find_last_of(L"\\/");
    if (slash != std::wstring::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    toSave.name = WToU8(stem);
    toSave.path = buf;

    std::string err;
    if (!SaveKeyMap(buf, toSave, &err)) {
        std::wstring msg = U8ToW(Ts("Could not save keymap") + (err.empty() ? "" : (": " + err)));
        MessageBoxW(dlg, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONWARNING);
        return;
    }
    // Switch to the saved keymap so further edits go there.
    SetActiveKeyMap(toSave);
    UpdateKeymapLabel(dlg);
}

static void OnFindShortcut(HWND dlg)
{
    // Open the find-by-shortcut dialog. Captures a Shortcut; on Search,
    // locates the matching action in the active keymap across all
    // categories, switches to that category, refreshes the list, and
    // selects the matched row.
    AssignState st;
    s_assign = &st;
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDD_FIND_SHORTCUT), dlg, FindShortcutDlgProc);
    Shortcut sc = st.captured;
    s_assign = nullptr;

    if (r != IDC_FIND_SHORTCUT_SEARCH) return;
    if (!sc.valid()) return;

    // Search ONLY within the currently selected category. The user picks
    // a section first, then asks "is this shortcut bound here?" — the
    // answer must be scoped to that section so a Main-shortcut doesn't
    // surface while the user is browsing Radio actions.
    const KeyMap& km = GetActiveKeyMap();
    std::string foundId = km.FindActionFor(sc, s_state->currentCategory);

    if (foundId.empty()) {
        std::wstring msg = U8ToW(Ts("No action is assigned to this shortcut in this section."));
        MessageBoxW(dlg, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONINFORMATION);
        return;
    }

    // Clear filter so the matched row is guaranteed to be visible.
    s_state->searchText.clear();
    SetDlgItemTextW(dlg, IDC_ACTIONS_SEARCH, L"");
    PopulateActionsList(dlg);

    // Select the matched action in the listbox.
    HWND list = GetDlgItem(dlg, IDC_ACTIONS_LIST);
    for (size_t i = 0; i < s_state->visibleActions.size(); ++i) {
        if (s_state->visibleActions[i]->stringId == foundId) {
            SendMessageW(list, LB_SETCURSEL, (WPARAM)(int)i, 0);
            PopulateShortcutsList(dlg);
            break;
        }
    }
    // Move focus to the action list so the user is right where they need to be.
    SetFocus(list);
}

static void OnReset(HWND dlg)
{
    std::wstring prompt = U8ToW(Ts(
        "Reset all shortcuts to the regional default for this keyboard layout?"));
    if (MessageBoxW(dlg, prompt.c_str(), L"MediaAccess",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    std::string region = DetectDefaultKeyMapName();
    KeyMap km;
    if      (region == "FR-CA") km = BuildDefaultFrCaKeyMap();
    else if (region == "FR-FR") km = BuildDefaultFrFrKeyMap();
    else                        km = BuildDefaultUsaKeyMap();
    // Try the shipped path first, then fall back to the user dir if the
    // install dir isn't writable (typical for admin-installed builds running
    // as a normal user, where Program Files\...\KeyMaps\ is read-only).
    km.path = GetShippedKeyMapPath(km.name);
    if (!SaveKeyMap(km.path, km, nullptr)) {
        km.path = GetUserKeyMapPath(km.name);
        SaveKeyMap(km.path, km, nullptr);
    }
    SetActiveKeyMap(km);

    UpdateKeymapLabel(dlg);
    PopulateActionsList(dlg);
    PopulateShortcutsList(dlg);
}

// =============================================================================
// Main dialog proc
// =============================================================================

static INT_PTR CALLBACK ActionsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(dlg);
            PopulateCategoryCombo(dlg);
            PopulateActionsList(dlg);
            PopulateShortcutsList(dlg);
            UpdateKeymapLabel(dlg);
            SetFocus(GetDlgItem(dlg, IDC_ACTIONS_LIST));
            return FALSE;
        }

        case WM_COMMAND: {
            WORD code = HIWORD(wp);
            WORD id   = LOWORD(wp);
            switch (id) {
                case IDC_ACTIONS_CATEGORY:
                    if (code == CBN_SELCHANGE) {
                        int sel = (int)SendDlgItemMessageW(dlg, IDC_ACTIONS_CATEGORY, CB_GETCURSEL, 0, 0);
                        if (sel >= 0) {
                            s_state->currentCategory = (ActionCategory)sel;
                            PopulateActionsList(dlg);
                            PopulateShortcutsList(dlg);
                        }
                    }
                    return TRUE;
                case IDC_ACTIONS_SEARCH:
                    if (code == EN_CHANGE) {
                        wchar_t buf[256] = {0};
                        GetDlgItemTextW(dlg, IDC_ACTIONS_SEARCH, buf, 256);
                        s_state->searchText = WToU8(buf);
                        PopulateActionsList(dlg);
                        PopulateShortcutsList(dlg);
                    }
                    return TRUE;
                case IDC_ACTIONS_LIST:
                    if (code == LBN_SELCHANGE) PopulateShortcutsList(dlg);
                    return TRUE;
                case IDC_ACTIONS_ADD:           OnAdd(dlg);           return TRUE;
                case IDC_ACTIONS_EDIT:          OnEdit(dlg);          return TRUE;
                case IDC_ACTIONS_DELETE:        OnDelete(dlg);        return TRUE;
                case IDC_ACTIONS_LOAD:          OnLoad(dlg);          return TRUE;
                case IDC_ACTIONS_SAVE_AS:       OnSaveAs(dlg);        return TRUE;
                case IDC_ACTIONS_RESET:         OnReset(dlg);         return TRUE;
                case IDC_ACTIONS_FIND_SHORTCUT: OnFindShortcut(dlg);  return TRUE;
                case IDOK:
                case IDCANCEL:
                    EndDialog(dlg, 0);
                    return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(dlg, 0);
            return TRUE;
    }
    return FALSE;
}

// =============================================================================
// Public entry point
// =============================================================================

void ShowActionsWindow(HWND owner)
{
    DialogState st;
    s_state = &st;
    DialogBoxW(GetModuleHandleW(nullptr),
               MAKEINTRESOURCEW(IDD_ACTIONS), owner, ActionsDlgProc);
    s_state = nullptr;
}

} // namespace mediaaccess
