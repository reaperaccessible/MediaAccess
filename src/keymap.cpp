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
#include <unordered_map>

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
    // v1.74 — Keep the empty entry as a tombstone instead of erasing it.
    // MergeMissingDefaults looks at `bindings.find(stringId) != end()` to
    // decide whether to inject the default shortcut for an action that has
    // no binding yet. If we erased the entry here, the next startup (or
    // next combo switch via LoadKeyMapByName) would treat the action as
    // "missing" and re-inject its default — defeating the user's intent
    // when they explicitly removed every binding for that action.
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

        // v1.74 — Always create the entry (possibly empty) so a line like
        // "ACTION_ID =" with no RHS materialises as a tombstone. This is
        // what tells MergeMissingDefaults that the user explicitly cleared
        // every binding for this action and does NOT want the default
        // resurrected at next load. operator[] inserts an empty vector if
        // the key is missing; the loop below adds the shortcuts, if any.
        auto& vec = km.bindings[key];
        std::vector<std::string> parts = SplitCsv(val, ',');
        for (const auto& part : parts) {
            if (part.empty()) continue;
            Shortcut s = ShortcutFromKeymapText(part);
            if (s.valid()) {
                bool dup = false;
                for (const auto& x : vec) if (x == s) { dup = true; break; }
                if (!dup) vec.push_back(s);
            }
        }
    }

    // v1.72 — Always derive km.name from the on-disk filename, even if the
    // file's own NAME= header says something else. The stem is the canonical
    // identity used by ListAvailableKeyMaps, the Actions-dialog combo, and
    // [Actions] CurrentKeyMap in MediaAccess.ini; honouring an out-of-sync
    // NAME= header would cause LoadActiveKeyMapAtStartup to persist a name
    // it cannot resolve back to a file on next launch, silently falling back
    // to USA — exactly the class of bug Sèb reported with v1.71's Save-As
    // letting the user pick an arbitrary path. The NAME= line is still
    // parsed above (cosmetic; honoured at read but immediately overridden
    // here) and is regenerated as NAME=<stem> by SaveKeyMap on the next
    // NotifyKeymapChanged, so any out-of-sync file self-heals on first edit.
    {
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
    struct CatRow { ActionCategory cat; const char* name; };
    static const CatRow kCats[] = {
        { ActionCategory::Main,    "Main"    },
        { ActionCategory::Radio,   "Radio"   },
        { ActionCategory::YouTube, "YouTube" },
        { ActionCategory::Global,  "Global"  },
        { ActionCategory::Books,   "Books"   },
    };
    for (const CatRow& row : kCats) {
        std::vector<const Action*> acts = ActionsInCategory(row.cat);
        if (acts.empty()) continue;

        oss << "# ----- " << row.name << " -----\r\n";

        for (const Action* a : acts) {
            auto it = km.bindings.find(a->stringId);
            // v1.74 — Absent from bindings: skip the line entirely. Present
            // but empty (tombstone, see KeyMap::RemoveShortcut): write
            // "ACTION_ID =" with no RHS, so the next LoadKeyMap recreates
            // the tombstone and MergeMissingDefaults respects the user's
            // explicit decision to leave that action unbound.
            if (it == km.bindings.end()) continue;
            oss << a->stringId << " =";
            for (size_t i = 0; i < it->second.size(); ++i) {
                oss << (i == 0 ? " " : ", ");
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

    // v2.50 — the physical key below Esc (scancode 0x29) sends VK_OEM_7 on the
    // Canadian layouts (Multilingual Standard 00011009 and Canadian French
    // 00000C0C: '#'), not VK_OEM_3 ('`') as on US QWERTY. Remap CLEAR_LOOP so
    // the same physical key clears the loop. (The Shift+[ / Shift+] markers
    // keep the USA VK positions — brackets are unchanged on Canadian layouts.)
    auto remap = [&](const char* actId, UINT oldVk, UINT newVk) {
        Shortcut from{ oldVk, false, false, false };
        Shortcut to  { newVk, false, false, false };
        km.RemoveShortcut(actId, from);
        km.AddShortcut(actId, to);
    };
    remap("CLEAR_LOOP", VK_OEM_3, VK_OEM_7);
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
    // v1.75 — BOOKMARK_ADD intentionally NOT remapped. The original remap
    // (M physical → VK_OEM_COMMA) followed the "physical-position" doctrine
    // used for the Winamp row above, but BOOKMARK_ADD is a letter-semantic
    // binding (M = Mark/Marquer), not a positional one. Worse, VK_OEM_COMMA
    // was already claimed by SEEK_UNIT_DECREASE in the cloned USA defaults,
    // so the remap silently created an intra-category duplicate that the
    // dispatcher resolved in favour of BOOKMARK_ADD (B < S in std::map
    // order). User reported the bug on FR-FR. Solution: leave BOOKMARK_ADD
    // on VK 'M' so a French AZERTY user presses the same logical M key as
    // a US QWERTY user — aligns FR-FR with USA/FR-CA for this action.

    // EFFECT_PREV / EFFECT_NEXT live at physical scancodes 0x1A and 0x1B
    // (right of P). On AZERTY those keys send VK_OEM_6 (^) and VK_OEM_1 ($)
    // respectively — NOT VK_OEM_4 / VK_OEM_6 like QWERTY does. Without this
    // remap, the user's physical [ / ] keys do nothing and instead the
    // top-row ) key (which IS VK_OEM_4 on AZERTY) triggers EFFECT_PREV.
    remap("EFFECT_PREV", VK_OEM_4, VK_OEM_6);  // physical 0x1A: AZERTY ^
    remap("EFFECT_NEXT", VK_OEM_6, VK_OEM_1);  // physical 0x1B: AZERTY $

    // v2.50 — A-B loop on AZERTY. The Shift+[ / Shift+] markers follow the same
    // physical bracket positions as EFFECT_PREV/NEXT (0x1A/0x1B). The `remap`
    // lambda above only swaps no-modifier bindings, so the Shift-qualified
    // marker swaps are written explicitly here.
    {
        Shortcut from{ VK_OEM_4, false, true, false };  // Shift+[ (USA)
        Shortcut to  { VK_OEM_6, false, true, false };  // physical 0x1A on AZERTY
        km.RemoveShortcut("SET_LOOP_START", from);
        km.AddShortcut("SET_LOOP_START", to);
    }
    {
        Shortcut from{ VK_OEM_6, false, true, false };  // Shift+] (USA)
        Shortcut to  { VK_OEM_1, false, true, false };  // physical 0x1B on AZERTY
        km.RemoveShortcut("SET_LOOP_END", from);
        km.AddShortcut("SET_LOOP_END", to);
    }
    // Physical key below Esc (scancode 0x29) is VK_OEM_7 ('²') on AZERTY.
    remap("CLEAR_LOOP", VK_OEM_3, VK_OEM_7);

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
// v1.72 — combo-driven keymap management (used by the Actions dialog)
// =============================================================================
//
// These helpers replace the free file-picker round-trip that v1.71 exposed
// to the user. The Actions dialog now offers a Keymap combo listing every
// available keymap (user dir union shipped dir, dedup, sorted) with Import /
// Delete / New buttons that route through these functions. All disk
// activity is confined to %APPDATA%\MediaAccess\KeyMaps\ so the loader
// always finds what the user just created.

// Returns the on-disk path for `name`, preferring the user dir over the
// shipped dir (matches LoadActiveKeyMapAtStartup precedence). Empty if
// neither file exists.
static std::wstring ResolveKeyMapPath(const std::string& name)
{
    std::wstring p = GetUserKeyMapPath(name);
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    p = GetShippedKeyMapPath(name);
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return std::wstring();
}

// v1.75 — Detect and silently resolve intra-category duplicate shortcuts.
//
// The dispatcher (FindCommandFor / FindActionFor below) is first-match-wins
// on std::map iteration order, so a duplicate shortcut in the same category
// silently shadows whichever action sorts later in lexicographic stringId
// order. Symptom in the wild (user "Seb", FR-FR): pressing ',' triggered
// BOOKMARK_ADD instead of SEEK_UNIT_DECREASE because 'B' < 'S'. We now
// scan every loaded keymap, keep the first occurrence of each (category,
// shortcut) pair, and drop subsequent duplicates in memory. Callers that
// own a writable km.path persist the cleaned version on next save.
//
// Cross-category duplicates remain allowed (e.g. M for BOOKMARK_ADD in
// Main AND BOOK_BOOKMARK_LIST in Books) — the contextual dispatcher
// (a book player vs the audio main loop) decides which to fire.
//
// Tombstones (entries the user explicitly cleared, see KeyMap::RemoveShortcut)
// are preserved untouched: their vec is already empty so no shortcut can
// duplicate from inside it.
//
// Returns true if at least one duplicate was dropped.
static bool ResolveSameCategoryDuplicates(KeyMap& km)
{
    bool changed = false;
    // v1.84 — Snapshot the initial binding-list sizes BEFORE the dedup runs.
    // A key whose vector is empty at this point is an authentic user tombstone
    // (see KeyMap::RemoveShortcut). A key whose vector becomes empty BECAUSE
    // of the dedup below is a phantom tombstone — it would silently block
    // MergeMissingDefaults from re-injecting the registry default on the
    // next load. We erase only the phantom tombstones at the end.
    //
    // Background: v1.75 renamed VIDEO_SUB_CYCLE from Ctrl+Shift+T to
    // Ctrl+Shift+L. Users whose personal keymap still had VIDEO_SUB_CYCLE on
    // Ctrl+Shift+T had that line wiped by this dedup (Ctrl+Shift+T was already
    // claimed by SPEAK_TOTAL), and MergeMissingDefaults then mistook the
    // wiped-but-present key for a deliberate "unbind". Result: Ctrl+Shift+L
    // never reappeared until the user clicked Reset.
    std::unordered_map<std::string, size_t> initialSizes;
    initialSizes.reserve(km.bindings.size());
    for (const auto& kv : km.bindings) initialSizes[kv.first] = kv.second.size();

    // For each category, build a (serialised shortcut → first-claimer stringId)
    // index. Iterating g_actions[] via ActionAt() gives a stable, predictable
    // order; we then traverse km.bindings sorted by stringId for determinism
    // across runs. First-wins: the action that registered the shortcut first
    // (in std::map iteration order = lex on stringId) keeps it.
    for (int catIdx = 0; catIdx < (int)ActionCategory::Count; ++catIdx) {
        ActionCategory cat = (ActionCategory)catIdx;
        std::map<std::string, std::string> firstOwner; // serialised shortcut → stringId
        for (auto& kv : km.bindings) {
            const Action* a = ActionByStringId(kv.first);
            if (!a) continue;                  // unknown action — skip
            if (a->category != cat) continue;  // only this category in this pass
            auto& vec = kv.second;
            std::vector<Shortcut> kept;
            kept.reserve(vec.size());
            for (const Shortcut& s : vec) {
                if (!s.valid()) continue;
                std::string key = ShortcutToKeymapText(s);
                auto own = firstOwner.find(key);
                if (own == firstOwner.end()) {
                    firstOwner.emplace(key, kv.first);
                    kept.push_back(s);
                } else if (own->second == kv.first) {
                    // Same action listing the same shortcut twice (impossible
                    // via AddShortcut but possible if the file was hand-edited).
                    // Silently dedup.
                    changed = true;
                } else {
                    // Another action already owns this shortcut in this
                    // category. Drop this binding and log for diagnostics.
                    wchar_t buf[256];
                    _snwprintf_s(buf, 256, _TRUNCATE,
                        L"[MediaAccess] Keymap dedup: dropped %S from '%S' "
                        L"(already on '%S' in category %d)\n",
                        key.c_str(), kv.first.c_str(),
                        own->second.c_str(), (int)cat);
                    OutputDebugStringW(buf);
                    changed = true;
                }
            }
            vec.swap(kept);
            // Genuine tombstones (kept.empty() && initialSizes was already 0)
            // are preserved untouched; see end-of-function pass below.
        }
    }

    // v1.84 — Erase phantom tombstones: keys whose list was non-empty before
    // dedup but is empty after. These would otherwise block
    // MergeMissingDefaults from reinjecting the registry default.
    for (auto it = km.bindings.begin(); it != km.bindings.end(); ) {
        auto sizeIt = initialSizes.find(it->first);
        size_t originalSize = (sizeIt != initialSizes.end()) ? sizeIt->second : 0;
        if (it->second.empty() && originalSize > 0) {
            it = km.bindings.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

// v1.74 — Extracted from LoadActiveKeyMapAtStartup so the same logic runs
// for every keymap load path (startup AND combo switch via LoadKeyMapByName).
//
// For each registered action with a defaultUsa shortcut:
//   * skip if the keymap already has ANY entry for that action (including
//     an empty tombstone — see KeyMap::RemoveShortcut). An entry means the
//     user has expressed an opinion, even if that opinion is "no binding".
//   * skip if the default shortcut is already claimed by another action
//     in the same category (don't steal the user's reassigned key).
//   * otherwise, add the default. This is what lets actions introduced
//     after a keymap was generated (e.g. Sleep Timer and Books in v1.50+)
//     pick up their default bindings automatically, without requiring a
//     "Reset to defaults" click and without disturbing customisations.
//
// Returns true if any binding was added. Callers decide whether to persist:
// LoadActiveKeyMapAtStartup and LoadKeyMapByName both save when changed
// because they own the file at that point.
static bool MergeMissingDefaults(KeyMap& km)
{
    // v2.50 — region-aware merge. A new action added after this keymap file
    // was created must adopt the binding for THIS region, not the raw USA
    // default. Otherwise an OEM-position key (e.g. FR-CA CLEAR_LOOP) lands on
    // the USA key (accent grave) instead of the regional one (# on FR-CA,
    // ² on FR-FR), and AZERTY loop markers bind to the wrong keys. The
    // BuildDefault*KeyMap functions already encode the per-layout remaps, so
    // we source missing-action shortcuts from the matching regional default.
    //
    // v2.50.1 — key the choice on the REGION tag, not the name. A custom
    // ("perso") keymap cloned from FR-FR keeps region="fr-FR" but has a custom
    // name, so a name-only test fell through to the USA default and AZERTY
    // users got QWERTY loop markers. Name is kept as a fallback for files that
    // somehow lack a REGION line. Reported by a FR-FR user with a perso keymap.
    const bool isFrCa = (km.region == "fr-CA" || km.name == "FR-CA");
    const bool isFrFr = (km.region == "fr-FR" || km.name == "FR-FR");
    KeyMap regionDefault =
        isFrCa ? BuildDefaultFrCaKeyMap() :
        isFrFr ? BuildDefaultFrFrKeyMap() :
                 BuildDefaultUsaKeyMap();
    auto regionShortcut = [&](const char* id, const Shortcut& fallback) -> Shortcut {
        auto rit = regionDefault.bindings.find(id);
        if (rit != regionDefault.bindings.end() && !rit->second.empty())
            return rit->second[0];
        return fallback;
    };

    int total = ActionCount();
    bool changed = false;
    for (int i = 0; i < total; ++i) {
        const Action* a = ActionAt(i);
        if (!a || !a->defaultUsa.valid()) continue;
        // v1.84 — belt-and-braces with the phantom-tombstone fix in
        // ResolveSameCategoryDuplicates. A user who already had a wiped
        // "ACTION_ID =" line written to disk by v1.75-v1.83 will load that
        // file with an empty vec for the action; ResolveSameCategoryDuplicates
        // treats it as a genuine tombstone (initialSize==0) and won't erase
        // it, so the bug would persist without this. Now we also treat a
        // present-but-empty entry as "absent" — the action is eligible for
        // the default merge. Trade-off: a user who deliberately cleared the
        // last binding for an action will see the default reappear on next
        // load. This is the retroactive migration the user explicitly
        // requested to recover Ctrl+Shift+L (VIDEO_SUB_CYCLE) without a
        // manual Reset.
        auto it = km.bindings.find(a->stringId);
        if (it != km.bindings.end() && !it->second.empty()) continue;
        Shortcut def = regionShortcut(a->stringId, a->defaultUsa);
        if (!km.FindActionFor(def, a->category).empty()) continue;
        km.AddShortcut(a->stringId, def);
        changed = true;
    }

    // v2.50 — one-time correction for the A-B loop actions. An early 2.50
    // build ran this merge before it was region-aware, so on FR-CA/FR-FR
    // keymaps the new loop actions were saved with the USA defaults (CLEAR_LOOP
    // on accent grave instead of #/², AZERTY markers on the wrong keys). If a
    // loop action currently holds EXACTLY the USA default while the regional
    // default differs, fix it. Safe: these actions are new in 2.50, so a
    // USA-default value is the merge bug, not a deliberate user choice.
    if (isFrCa || isFrFr) {
        static const char* const kLoopIds[] = {
            "SET_LOOP_START", "SET_LOOP_END", "TOGGLE_LOOP", "CLEAR_LOOP"
        };
        for (const char* id : kLoopIds) {
            const Action* a = ActionByStringId(id);
            if (!a || !a->defaultUsa.valid()) continue;
            Shortcut usaDef = a->defaultUsa;
            Shortcut regDef = regionShortcut(id, usaDef);
            if (regDef == usaDef) continue;            // nothing regional to fix
            auto it = km.bindings.find(id);
            if (it == km.bindings.end()) continue;     // missing → handled above
            if (it->second.size() == 1 && it->second[0] == usaDef &&
                km.FindActionFor(regDef, a->category).empty()) {
                km.RemoveShortcut(id, usaDef);
                km.AddShortcut(id, regDef);
                changed = true;
            }
        }
    }

    return changed;
}

bool LoadKeyMapByName(const std::string& name, std::string* errorOut)
{
    if (name.empty()) {
        if (errorOut) *errorOut = "Empty keymap name.";
        return false;
    }
    std::wstring path = ResolveKeyMapPath(name);
    if (path.empty()) {
        if (errorOut) *errorOut = "Keymap file not found.";
        return false;
    }
    std::string loadErr;
    KeyMap km = LoadKeyMap(path, &loadErr);
    if (km.bindings.empty()) {
        if (errorOut) *errorOut = loadErr.empty() ? "Empty or unparseable keymap." : loadErr;
        return false;
    }
    // v1.67 migration: if we loaded from the install dir, redirect future
    // saves to %APPDATA% so the next installer update cannot wipe the user's
    // edits. SetActiveKeyMap then persists the name to MediaAccess.ini.
    if (path == GetShippedKeyMapPath(name)) {
        km.path = GetUserKeyMapPath(name);
    }
    // v1.75 — drop any intra-category duplicate shortcut (silently keeps
    // the first occurrence). Same step the startup loader runs; without
    // it, a keymap selected through the combo would carry forward any
    // duplicate present in the file and the dispatcher would shadow one
    // of the actions unpredictably.
    bool dedupedAtLoad = ResolveSameCategoryDuplicates(km);

    // v1.74 — same merge step the startup loader runs. Without this, a
    // keymap selected through the combo would silently miss defaults for
    // any action added in a version newer than the file on disk (the user
    // would see the action listed without a shortcut, even though a default
    // is defined). Tombstones — entries the user explicitly cleared — are
    // skipped by MergeMissingDefaults, so a deliberate "unbind" survives.
    bool mergedAtLoad = MergeMissingDefaults(km);
    if ((dedupedAtLoad || mergedAtLoad) && !km.path.empty()) {
        SaveKeyMap(km.path, km, nullptr);
    }
    SetActiveKeyMap(km);
    return true;
}

bool ImportKeyMapFromFile(const std::wstring& sourcePath,
                          std::string* importedName,
                          std::string* errorOut)
{
    if (sourcePath.empty()) {
        if (errorOut) *errorOut = "Empty source path.";
        return false;
    }
    // Extension guard — accept only the canonical .MediaAccessKeyMap suffix
    // (case-insensitive). Other formats (.txt, .ini, .reg, etc.) are
    // rejected to keep the user dir clean and the loader predictable.
    {
        std::wstring ext;
        size_t dot = sourcePath.find_last_of(L'.');
        if (dot != std::wstring::npos) ext = sourcePath.substr(dot);
        std::wstring lower = ext;
        for (wchar_t& c : lower) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (lower != L".mediaaccesskeymap") {
            if (errorOut) *errorOut = "Wrong extension (expected .MediaAccessKeyMap).";
            return false;
        }
    }
    // Parseability probe — refuse to import a file the loader would silently
    // discard on next startup. Cheap (the bindings vector is empty if the
    // header is missing or every line is malformed).
    {
        std::string loadErr;
        KeyMap probe = LoadKeyMap(sourcePath, &loadErr);
        if (probe.bindings.empty()) {
            if (errorOut) *errorOut = loadErr.empty() ? "Source file is empty or invalid." : loadErr;
            return false;
        }
    }
    // Derive the destination stem from the source file name. We deliberately
    // do NOT honour the NAME= header in the file — the on-disk stem is the
    // identity used by ListAvailableKeyMaps and the INI persistence layer.
    std::wstring stem = sourcePath;
    size_t slash = stem.find_last_of(L"\\/");
    if (slash != std::wstring::npos) stem = stem.substr(slash + 1);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    if (stem.empty()) {
        if (errorOut) *errorOut = "Could not derive name from filename.";
        return false;
    }
    std::string stemUtf8 = WideToUtf8(stem);
    std::wstring dest = GetUserKeyMapPath(stemUtf8);
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (errorOut) *errorOut = "A keymap with that name already exists.";
        return false;
    }
    EnsureUserKeyMapsDir();
    // CopyFileW with bFailIfExists=TRUE is redundant with the check above
    // but defends against a TOCTOU race (another process creating the file
    // between the GetFileAttributes and the copy). Cheap insurance.
    if (!CopyFileW(sourcePath.c_str(), dest.c_str(), TRUE)) {
        if (errorOut) *errorOut = "Could not copy the file.";
        return false;
    }
    if (importedName) *importedName = stemUtf8;
    return true;
}

bool DeleteKeyMapByName(const std::string& name, std::string* errorOut)
{
    if (name.empty()) {
        if (errorOut) *errorOut = "Empty keymap name.";
        return false;
    }
    if (name == g_activeKeymap.name) {
        if (errorOut) *errorOut = "Cannot delete the active keymap.";
        return false;
    }
    std::wstring userPath = GetUserKeyMapPath(name);
    if (GetFileAttributesW(userPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // No user-dir copy — the shipped file (if any) stays untouched and
        // the keymap will still appear in the combo via ListAvailableKeyMaps.
        // Treat as success so the caller can refresh the UI uniformly.
        return true;
    }
    if (!DeleteFileW(userPath.c_str())) {
        if (errorOut) *errorOut = "Could not delete the file.";
        return false;
    }
    return true;
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
    bool loadedFromInstallDir = false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        path = GetShippedKeyMapPath(chosen);
        loadedFromInstallDir = true;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Last-ditch: fall back to USA shipped path; if still missing,
        // build defaults in memory.
        path = GetShippedKeyMapPath("USA");
        loadedFromInstallDir = true;
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

    // v1.67 — keymap migration. The installer writes regional defaults
    // to <install>\KeyMaps with ignoreversion, so any user-modification
    // stored at that path gets overwritten on every update. Redirect
    // the in-memory path to the user dir so the very next save (auto
    // after a shortcut edit, or explicit Save) lands in %APPDATA% and
    // survives future installs. Jack reported his Global hotkeys being
    // reset on every release before this fix.
    if (loadedFromInstallDir && !chosen.empty()) {
        km.path = GetUserKeyMapPath(chosen);
    }

    // v1.75 — drop any intra-category duplicate shortcut left over from
    // hand-edited files, imported keymaps, or stale shipped defaults.
    // Silent first-wins resolution; see ResolveSameCategoryDuplicates.
    bool dedupedAtStartup = ResolveSameCategoryDuplicates(km);

    // v1.74 — merge step extracted to MergeMissingDefaults so the combo
    // switch path (LoadKeyMapByName) gets the same self-healing behaviour.
    // Tombstone-aware: actions the user explicitly cleared are skipped.
    bool mergedAtStartup = MergeMissingDefaults(km);
    if ((dedupedAtStartup || mergedAtStartup) && !km.path.empty()) {
        SaveKeyMap(km.path, km, nullptr);
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
