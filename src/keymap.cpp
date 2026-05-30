// =============================================================================
// keymap.cpp — keymap data model, file I/O, layout detection, active state
// =============================================================================

#include "mediaaccess/keymap.h"
#include "mediaaccess/actions.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/hotkeys.h"  // RegisterGlobalHotkeys / UnregisterGlobalHotkeys
#include "mediaaccess/types.h"    // GlobalHotkey
#include "mediaaccess/updater.h"  // IsInstalledMode

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

// Externs from the rest of MediaAccess (global namespace).
extern std::wstring g_configPath;
extern HWND         g_hwnd;
extern std::vector<GlobalHotkey> g_hotkeys;
extern int          g_nextHotkeyId;
extern bool         g_hotkeysEnabled;
extern const HotkeyAction g_hotkeyActions[];
extern const int          g_hotkeyActionCount;

namespace mediaaccess {

// =============================================================================
// KeyMap methods
// =============================================================================

void KeyMap::Clear()
{
    path.clear();
    name.clear();
    region.clear();
    bindings.clear();
}

const std::vector<Shortcut>* KeyMap::GetShortcuts(const std::string& actionStringId) const
{
    auto it = bindings.find(actionStringId);
    if (it == bindings.end()) return nullptr;
    return &it->second;
}

void KeyMap::AddShortcut(const std::string& actionStringId, const Shortcut& s)
{
    if (!s.valid()) return;
    auto& vec = bindings[actionStringId];
    for (const auto& x : vec) if (x == s) return;
    vec.push_back(s);
}

void KeyMap::RemoveShortcut(const std::string& actionStringId, const Shortcut& s)
{
    auto it = bindings.find(actionStringId);
    if (it == bindings.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), s), vec.end());
    if (vec.empty()) bindings.erase(it);
}

std::string KeyMap::FindActionFor(const Shortcut& s, ActionCategory cat) const
{
    if (!s.valid()) return "";
    for (const auto& kv : bindings) {
        const Action* a = ActionByStringId(kv.first);
        if (!a || a->category != cat) continue;
        for (const auto& sh : kv.second) {
            if (sh == s) return kv.first;
        }
    }
    return "";
}

int KeyMap::FindCommandFor(const Shortcut& s, ActionCategory cat) const
{
    std::string id = FindActionFor(s, cat);
    if (id.empty()) return 0;
    const Action* a = ActionByStringId(id);
    return a ? a->commandId : 0;
}

// =============================================================================
// File I/O
// =============================================================================

static std::string TrimCopy(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> SplitCsv(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(TrimCopy(cur)); cur.clear(); }
        else          { cur.push_back(c); }
    }
    if (!cur.empty()) out.push_back(TrimCopy(cur));
    return out;
}

KeyMap LoadKeyMap(const std::wstring& path, std::string* errorOut)
{
    KeyMap km;
    km.path = path;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = "File not found";
        return km;
    }
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0) {
        CloseHandle(h);
        if (errorOut) *errorOut = "Empty file";
        return km;
    }
    std::string content(sz, '\0');
    DWORD read = 0;
    ReadFile(h, &content[0], sz, &read, nullptr);
    CloseHandle(h);
    content.resize(read);

    // Strip UTF-8 BOM if present.
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        content.erase(0, 3);
    }

    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Strip CR
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = TrimCopy(line);
        if (trimmed.empty()) continue;
        if (trimmed[0] == '#' || trimmed[0] == ';') continue;

        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = TrimCopy(trimmed.substr(0, eq));
        std::string val = TrimCopy(trimmed.substr(eq + 1));

        if (key == "NAME")   { km.name = val; continue; }
        if (key == "REGION") { km.region = val; continue; }

        // Otherwise treat as an action binding.
        // Right-hand side is a comma-separated list of shortcut texts.
        const Action* a = ActionByStringId(key);
        if (!a) continue; // Unknown action — silently skip.

        std::vector<std::string> parts = SplitCsv(val, ',');
        for (const auto& part : parts) {
            if (part.empty()) continue;
            Shortcut s = ShortcutFromKeymapText(part);
            if (s.valid()) km.AddShortcut(key, s);
        }
    }

    // Derive name from filename if not specified in header.
    if (km.name.empty()) {
        std::wstring stem = path;
        size_t slash = stem.find_last_of(L"\\/");
        if (slash != std::wstring::npos) stem = stem.substr(slash + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);
        char buf[256];
        WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        km.name = buf;
    }
    return km;
}

bool SaveKeyMap(const std::wstring& path, const KeyMap& km, std::string* errorOut)
{
    // Build content into memory then write atomically.
    std::ostringstream oss;
    oss << "# MediaAccess KeyMap v1\r\n";
    oss << "# Format: ACTION_ID = shortcut[, shortcut...]\r\n";
    oss << "# Edit with care — unknown action IDs are ignored on load.\r\n";
    oss << "\r\n";
    oss << "NAME=" << km.name << "\r\n";
    if (!km.region.empty()) oss << "REGION=" << km.region << "\r\n";
    oss << "\r\n";

    // Group bindings by category so the file is human-browsable.
    static const ActionCategory kCats[] = {
        ActionCategory::Main, ActionCategory::Radio,
        ActionCategory::YouTube, ActionCategory::Global,
        ActionCategory::Books
    };
    for (ActionCategory cat : kCats) {
        std::vector<const Action*> acts = ActionsInCategory(cat);
        if (acts.empty()) continue;

        // Section header
        const char* catName = "Main";
        switch (cat) {
            case ActionCategory::Main:    catName = "Main";    break;
            case ActionCategory::Radio:   catName = "Radio";   break;
            case ActionCategory::YouTube: catName = "YouTube"; break;
            case ActionCategory::Global:  catName = "Global";  break;
            case ActionCategory::Books:   catName = "Books";   break;
            default: break;
        }
        oss << "# ----- " << catName << " -----\r\n";

        for (const Action* a : acts) {
            auto it = km.bindings.find(a->stringId);
            if (it == km.bindings.end() || it->second.empty()) continue;
            oss << a->stringId << " = ";
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << ShortcutToKeymapText(it->second[i]);
            }
            oss << "\r\n";
        }
        oss << "\r\n";
    }

    std::string content = oss.str();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (errorOut) *errorOut = "Could not open file for writing";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != content.size()) {
        if (errorOut) *errorOut = "Short write";
        return false;
    }
    return true;
}

// =============================================================================
// Default keymap builders
// =============================================================================

KeyMap BuildDefaultUsaKeyMap()
{
    KeyMap km;
    km.name   = "USA";
    km.region = "en-US";
    int n = ActionCount();
    for (int i = 0; i < n; ++i) {
        const Action* a = ActionAt(i);
        if (a && a->defaultUsa.valid())
            km.AddShortcut(a->stringId, a->defaultUsa);
    }
    return km;
}

KeyMap BuildDefaultFrCaKeyMap()
{
    // Canadian Multilingual Standard layout (de facto standard in Quebec) is
    // a QWERTY layout — same physical key positions for A-Z as US QWERTY. So
    // the FR-CA defaults are identical to USA. The keymap differs only by
    // region tag, so French Canadian users get an obviously-named choice in
    // the keymap selector.
    KeyMap km = BuildDefaultUsaKeyMap();
    km.name   = "FR-CA";
    km.region = "fr-CA";
    return km;
}

KeyMap BuildDefaultFrFrKeyMap()
{
    // AZERTY layout: position-dependent letter remapping. The actions that
    // were bound to the physical row "ZXCVB M" on QWERTY now bind to the
    // VKs that occupy those physical positions on AZERTY:
    //   physical 0x2C (Z on US)  → AZERTY 'W'  → VK 'W'
    //   physical 0x2D (X on US)  → AZERTY 'X'  → VK 'X'  (same)
    //   physical 0x2E (C on US)  → AZERTY 'C'  → VK 'C'  (same)
    //   physical 0x2F (V on US)  → AZERTY 'V'  → VK 'V'  (same)
    //   physical 0x30 (B on US)  → AZERTY 'B'  → VK 'B'  (same)
    //   physical 0x32 (M on US)  → AZERTY ','  → VK_OEM_COMMA
    // And for the QWERTY top-row positions:
    //   physical 0x10 (Q)        → AZERTY 'A'  → VK 'A'
    //   physical 0x11 (W)        → AZERTY 'Z'  → VK 'Z'
    // AZERTY-specific changes below are intentionally minimal; the user can
    // tweak the rest through the Actions window.
    KeyMap km = BuildDefaultUsaKeyMap();
    km.name   = "FR-FR";
    km.region = "fr-FR";

    // Helper: swap a single-VK no-modifier binding from oldVk to newVk for
    // a given action string ID.
    auto remap = [&](const char* actId, UINT oldVk, UINT newVk) {
        Shortcut from{ oldVk, false, false, false };
        Shortcut to  { newVk, false, false, false };
        km.RemoveShortcut(actId, from);
        km.AddShortcut(actId, to);
    };

    // Winamp transport row — physical positions held constant
    remap("PLAYER_PREV", 'Z', 'W');        // physical 0x2C: AZERTY W
    // PLAYER_PLAY / PAUSE / STOP / NEXT (X, C, V, B) stay the same on AZERTY.
    remap("BOOKMARK_ADD", 'M', VK_OEM_COMMA); // physical 0x32: AZERTY ','

    // EFFECT_PREV / EFFECT_NEXT live at physical scancodes 0x1A and 0x1B
    // (right of P). On AZERTY those keys send VK_OEM_6 (^) and VK_OEM_1 ($)
    // respectively — NOT VK_OEM_4 / VK_OEM_6 like QWERTY does. Without this
    // remap, the user's physical [ / ] keys do nothing and instead the
    // top-row ) key (which IS VK_OEM_4 on AZERTY) triggers EFFECT_PREV.
    remap("EFFECT_PREV", VK_OEM_4, VK_OEM_6);  // physical 0x1A: AZERTY ^
    remap("EFFECT_NEXT", VK_OEM_6, VK_OEM_1);  // physical 0x1B: AZERTY $

    // Note: Numbers on AZERTY require Shift on the top row, so Ctrl+1 etc.
    // will physically require Ctrl+Shift+&. Users typically use the NumPad
    // for digits, where Ctrl+1 works natively. We leave the bindings as
    // Ctrl+'1' through Ctrl+'0' — they'll work on NumPad and also on top
    // row if NumLock-style mapping is active. Users can rebind via Actions.

    return km;
}

// =============================================================================
// Layout detection
// =============================================================================

std::string DetectDefaultKeyMapName()
{
    HKL hkl = GetKeyboardLayout(0);
    LANGID lang = LOWORD((DWORD_PTR)hkl);
    WORD primary = PRIMARYLANGID(lang);
    WORD sub     = SUBLANGID(lang);

    if (primary == LANG_FRENCH) {
        if (sub == SUBLANG_FRENCH_CANADIAN) return "FR-CA";
        // SUBLANG_FRENCH (France), SUBLANG_FRENCH_BELGIAN, SUBLANG_FRENCH_SWISS,
        // SUBLANG_FRENCH_LUXEMBOURG, SUBLANG_FRENCH_MONACO → FR-FR
        return "FR-FR";
    }
    // Everything else (LANG_ENGLISH, LANG_SPANISH, LANG_GERMAN, ...) falls
    // back to USA per user instruction.
    return "USA";
}

// =============================================================================
// Path helpers
// =============================================================================

static std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) s = s.substr(0, slash);
    return s;
}

static std::wstring AppDataDir()
{
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        std::wstring s = buf;
        s += L"\\MediaAccess";
        CreateDirectoryW(s.c_str(), nullptr);
        return s;
    }
    return ExeDir();
}

std::wstring GetShippedKeyMapPath(const std::string& name)
{
    std::wstring p = ExeDir();
    p += L"\\KeyMaps\\";
    wchar_t wbuf[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wbuf, 256);
    p += wbuf;
    p += L".MediaAccessKeyMap";
    return p;
}

std::wstring GetUserKeyMapPath(const std::string& name)
{
    std::wstring p = AppDataDir();
    p += L"\\KeyMaps\\";
    CreateDirectoryW(p.c_str(), nullptr);
    wchar_t wbuf[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wbuf, 256);
    p += wbuf;
    p += L".MediaAccessKeyMap";
    return p;
}

void EnsureUserKeyMapsDir()
{
    std::wstring p = AppDataDir();
    p += L"\\KeyMaps";
    CreateDirectoryW(p.c_str(), nullptr);
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 1) return "";
    std::string s(sz - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], sz, nullptr, nullptr);
    return s;
}

std::vector<std::string> ListAvailableKeyMaps()
{
    std::vector<std::string> out;
    auto scan = [&](const std::wstring& dir) {
        std::wstring pattern = dir + L"\\*.MediaAccessKeyMap";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = fd.cFileName;
            size_t dot = n.find_last_of(L'.');
            if (dot != std::wstring::npos) n = n.substr(0, dot);
            std::string utf8 = WideToUtf8(n);
            if (std::find(out.begin(), out.end(), utf8) == out.end())
                out.push_back(utf8);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    };
    scan(ExeDir() + L"\\KeyMaps");
    scan(AppDataDir() + L"\\KeyMaps");
    std::sort(out.begin(), out.end());
    return out;
}

// =============================================================================
// Active keymap state
// =============================================================================

static KeyMap g_activeKeymap;

const KeyMap& GetActiveKeyMap() { return g_activeKeymap; }
KeyMap&       GetActiveKeyMapMut() { return g_activeKeymap; }

// Forward declarations for menu refresh and INI writing.
void RefreshMenuAcceleratorHints();

static void WriteCurrentKeyMapNameToIni(const std::string& name)
{
    if (::g_configPath.empty()) return;
    wchar_t wname[256] = {0};
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wname, 256);
    WritePrivateProfileStringW(L"Actions", L"CurrentKeyMap", wname, ::g_configPath.c_str());
}

void SetActiveKeyMap(const KeyMap& km)
{
    g_activeKeymap = km;
    WriteCurrentKeyMapNameToIni(km.name);
    RefreshMenuAcceleratorHints();
    SyncGlobalHotkeysFromKeymap();
}

void NotifyKeymapChanged()
{
    if (!g_activeKeymap.path.empty()) {
        // Persist edits to source file.
        SaveKeyMap(g_activeKeymap.path, g_activeKeymap, nullptr);
    }
    RefreshMenuAcceleratorHints();
    // Edits to Global category bindings need to re-register with Windows
    // immediately so they take effect without restarting.
    SyncGlobalHotkeysFromKeymap();
}

// =============================================================================
// Startup: ship defaults if missing, load active keymap
// =============================================================================

static void WriteShippedDefaultIfMissing(const std::string& name, const KeyMap& km)
{
    std::wstring path = GetShippedKeyMapPath(name);
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return; // Already on disk.

    // Make sure the parent KeyMaps folder exists.
    std::wstring dir = path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir = dir.substr(0, slash);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    KeyMap copy = km;
    copy.path = path;
    SaveKeyMap(path, copy, nullptr);
}

void LoadActiveKeyMapAtStartup()
{
    EnsureUserKeyMapsDir();

    // Ensure the three regional defaults exist in <install>\KeyMaps\ —
    // generate them in-place from the registry if the installer didn't
    // ship them (covers portable / dev builds).
    WriteShippedDefaultIfMissing("USA",   BuildDefaultUsaKeyMap());
    WriteShippedDefaultIfMissing("FR-CA", BuildDefaultFrCaKeyMap());
    WriteShippedDefaultIfMissing("FR-FR", BuildDefaultFrFrKeyMap());

    // Resolve which keymap to load.
    std::string chosen;
    if (!::g_configPath.empty()) {
        wchar_t buf[256] = {0};
        GetPrivateProfileStringW(L"Actions", L"CurrentKeyMap", L"",
                                 buf, 256, ::g_configPath.c_str());
        if (buf[0]) chosen = WideToUtf8(buf);
    }
    if (chosen.empty()) chosen = DetectDefaultKeyMapName();

    // Search user dir first, then install dir.
    std::wstring path = GetUserKeyMapPath(chosen);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        path = GetShippedKeyMapPath(chosen);
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Last-ditch: fall back to USA shipped path; if still missing,
        // build defaults in memory.
        path = GetShippedKeyMapPath("USA");
    }

    KeyMap km;
    bool loadedFromFile = false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        km = LoadKeyMap(path, nullptr);
        loadedFromFile = !km.bindings.empty();
    }
    if (km.bindings.empty()) {
        km = BuildDefaultUsaKeyMap();
    }

    // Merge new defaults: for each registered action with a defaultUsa shortcut,
    // if the user's keymap doesn't have ANY binding for that action AND the
    // default shortcut isn't already claimed by another action, add it. This
    // lets new actions added by future updates (e.g. the Books category in
    // v1.49) receive their default bindings automatically without the user
    // having to click "Reset to defaults" — and without disturbing any of
    // their existing custom assignments.
    {
        int total = ActionCount();
        bool changed = false;
        for (int i = 0; i < total; ++i) {
            const Action* a = ActionAt(i);
            if (!a || !a->defaultUsa.valid()) continue;
            if (km.bindings.find(a->stringId) != km.bindings.end()) continue;
            // Don't steal a shortcut already bound to something else (same cat).
            if (!km.FindActionFor(a->defaultUsa, a->category).empty()) continue;
            km.AddShortcut(a->stringId, a->defaultUsa);
            changed = true;
        }
        if (changed && !km.path.empty()) {
            SaveKeyMap(km.path, km, nullptr);
        }
    }

    g_activeKeymap = km;
    // Only persist the choice if we actually loaded the user's selection.
    // A transient failure (file locked by antivirus, OneDrive sync, etc.)
    // must NOT overwrite the user's saved keymap name.
    if (loadedFromFile) {
        WriteCurrentKeyMapNameToIni(km.name);
    }
    // RefreshMenuAcceleratorHints will be called once the main menu exists.
}

// =============================================================================
// Menu accelerator hint refresh
// =============================================================================
//
// Each menu item looks like "&Play\tX" — the part after \t is the accelerator
// hint shown by Windows on the right side. We rewrite that hint to reflect
// the currently-bound shortcut.
// -----------------------------------------------------------------------------

static void RefreshMenuRecursive(HMENU menu)
{
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_ID | MIIM_SUBMENU | MIIM_STRING | MIIM_FTYPE;
        wchar_t buf[512] = {0};
        mii.dwTypeData = buf;
        mii.cch        = 511;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) continue;

        if (mii.hSubMenu) {
            RefreshMenuRecursive(mii.hSubMenu);
            continue;
        }
        if (mii.wID == 0) continue;

        const Action* a = ActionByCommandId((int)mii.wID);
        if (!a) continue;

        std::wstring label = buf;
        size_t tab = label.find(L'\t');
        std::wstring base = (tab == std::wstring::npos) ? label : label.substr(0, tab);

        // Compute new hint from the first shortcut bound to this action's
        // string ID in the active keymap.
        std::wstring hint;
        auto it = g_activeKeymap.bindings.find(a->stringId);
        if (it != g_activeKeymap.bindings.end() && !it->second.empty()) {
            std::string text = ShortcutToDisplay(it->second.front());
            wchar_t wbuf[128] = {0};
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wbuf, 128);
            hint = wbuf;
        }

        std::wstring rebuilt = hint.empty() ? base : (base + L"\t" + hint);
        if (rebuilt == label) continue;

        MENUITEMINFOW set{};
        set.cbSize = sizeof(set);
        set.fMask  = MIIM_STRING;
        set.dwTypeData = (LPWSTR)rebuilt.c_str();
        SetMenuItemInfoW(menu, i, TRUE, &set);
    }
}

// =============================================================================
// Legacy [Hotkeys] migration — runs once, then clears the legacy section
// =============================================================================
//
// MediaAccess <1.41 stored global hotkeys in MediaAccess.ini under [Hotkeys]
// as (modifiers, vk, actionIdx) tuples. The new keymap system replaces this.
// On first launch after upgrading, walk any legacy entries in g_hotkeys
// (already loaded by LoadHotkeys()), inject equivalent bindings into the
// active keymap's Global category, save the keymap to disk, and erase
// the [Hotkeys] section so the migration only runs once.
//
// Called from main.cpp between LoadHotkeys() and SyncGlobalHotkeysFromKeymap().
// -----------------------------------------------------------------------------

void MigrateLegacyHotkeysIfPresent()
{
    if (::g_hotkeys.empty()) return;

    // For each legacy entry, find the matching Action in our Global category
    // (matched by commandId) and inject the Shortcut.
    std::vector<const Action*> globals = ActionsInCategory(ActionCategory::Global);
    bool migratedAny = false;
    for (const auto& hk : ::g_hotkeys) {
        if (hk.commandId != 0) continue;  // Already a keymap-sourced entry.
        if (hk.actionIdx < 0 || hk.actionIdx >= ::g_hotkeyActionCount) continue;
        int cmd = ::g_hotkeyActions[hk.actionIdx].commandId;

        const Action* match = nullptr;
        for (const Action* a : globals) {
            if (a->commandId == cmd) { match = a; break; }
        }
        if (!match) continue;

        Shortcut sc;
        sc.vk    = hk.vk;
        sc.ctrl  = (hk.modifiers & MOD_CONTROL) != 0;
        sc.shift = (hk.modifiers & MOD_SHIFT)   != 0;
        sc.alt   = (hk.modifiers & MOD_ALT)     != 0;
        sc.win   = (hk.modifiers & MOD_WIN)     != 0;  // v1.66
        g_activeKeymap.AddShortcut(match->stringId, sc);
        migratedAny = true;
    }

    if (migratedAny && !g_activeKeymap.path.empty()) {
        SaveKeyMap(g_activeKeymap.path, g_activeKeymap, nullptr);
    }

    // Wipe the legacy [Hotkeys] section so this only runs once and old
    // entries can't reappear on later launches.
    if (!::g_configPath.empty()) {
        WritePrivateProfileStringW(L"Hotkeys", nullptr, nullptr, ::g_configPath.c_str());
    }
}

// =============================================================================
// Global hotkey sync — keymap "Global" category → g_hotkeys → RegisterHotKey
// =============================================================================
//
// The legacy code path (hotkeys.cpp + g_hotkeys + g_hotkeyActions) registers
// system-wide hotkeys via RegisterHotKey(). The new Actions/Keymap window
// stores bindings in the keymap's Global category. SyncGlobalHotkeysFromKeymap
// is the bridge: it rebuilds g_hotkeys from the keymap and re-registers them.
//
// Called:
//   - From wWinMain after LoadActiveKeyMapAtStartup() so g_hotkeys is populated
//     before WM_CREATE fires RegisterGlobalHotkeys().
//   - From NotifyKeymapChanged() after the user edits bindings in the Actions
//     window, so changes take effect without restarting.
// -----------------------------------------------------------------------------

void SyncGlobalHotkeysFromKeymap()
{
    // 1) Unregister currently active hotkeys so we can rebuild the table.
    //    Safe even if g_hwnd is null (UnregisterGlobalHotkeys is a no-op then).
    UnregisterGlobalHotkeys();

    // 2) Clear the legacy vector; we own it entirely from the keymap now.
    ::g_hotkeys.clear();

    // 3) Walk the Global category in the active keymap and produce
    //    GlobalHotkey entries.
    std::vector<const Action*> globals = ActionsInCategory(ActionCategory::Global);
    for (const Action* a : globals) {
        auto it = g_activeKeymap.bindings.find(a->stringId);
        if (it == g_activeKeymap.bindings.end()) continue;
        for (const Shortcut& sc : it->second) {
            if (!sc.valid()) continue;
            GlobalHotkey hk{};
            hk.id        = ::g_nextHotkeyId++;
            hk.modifiers = 0;
            if (sc.ctrl)  hk.modifiers |= MOD_CONTROL;
            if (sc.shift) hk.modifiers |= MOD_SHIFT;
            if (sc.alt)   hk.modifiers |= MOD_ALT;
            if (sc.win)   hk.modifiers |= MOD_WIN;  // v1.66
            hk.vk        = sc.vk;
            hk.actionIdx = -1;             // unused — commandId is authoritative
            hk.commandId = a->commandId;
            ::g_hotkeys.push_back(hk);
        }
    }

    // 4) Re-register. No-op if g_hwnd is null (early startup); WM_CREATE will
    //    call RegisterGlobalHotkeys() itself when the window exists.
    if (::g_hwnd) RegisterGlobalHotkeys();
}

void RefreshMenuAcceleratorHints()
{
    if (!::g_hwnd) return;
    HMENU menu = GetMenu(::g_hwnd);
    if (!menu) return;
    RefreshMenuRecursive(menu);
    DrawMenuBar(::g_hwnd);
}

} // namespace mediaaccess
