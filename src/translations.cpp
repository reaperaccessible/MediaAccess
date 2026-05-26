#include "translations.h"
#include "globals.h"
#include <unordered_map>
#include <string>
#include <cstring>

// Per-language translation maps: key (UTF-8 English source) -> translated wide text.
static std::unordered_map<std::string, std::wstring> g_en;
static std::unordered_map<std::string, std::wstring> g_fr;
static std::string g_currentLang = "en";

void AddTranslation(const char* lang, const char* key, const wchar_t* text) {
    if (!lang || !key || !text) return;
    if (strcmp(lang, "en") == 0) {
        g_en[key] = text;
    } else if (strcmp(lang, "fr") == 0) {
        g_fr[key] = text;
    }
}

std::string Ts(const char* key) {
    const wchar_t* w = T(key);
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}

const wchar_t* T(const char* key) {
    if (!key) return L"";
    auto& map = (g_currentLang == "fr") ? g_fr : g_en;
    auto it = map.find(key);
    if (it != map.end()) return it->second.c_str();

    // Fallback: try the other language so we never return raw key when at
    // least one translation exists.
    auto& fallback = (g_currentLang == "fr") ? g_en : g_fr;
    auto fit = fallback.find(key);
    if (fit != fallback.end()) return fit->second.c_str();

    // Last resort: convert UTF-8 key to wide chars in a thread-local buffer.
    static thread_local wchar_t buf[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, key, -1, buf, 512);
    if (n <= 0) {
        buf[0] = L'\0';
    }
    return buf;
}

const char* DetectSystemLanguage() {
    LANGID lang = GetUserDefaultUILanguage();
    WORD primary = PRIMARYLANGID(lang);
    if (primary == LANG_FRENCH) return "fr";
    return "en";
}

void SetLanguage(const char* langCode) {
    if (!langCode) return;
    if (strcmp(langCode, "fr") == 0) {
        g_currentLang = "fr";
    } else {
        g_currentLang = "en";
    }
}

const char* GetCurrentLanguage() {
    return g_currentLang.c_str();
}

extern void RegisterPlayerTranslations();
extern void RegisterRcTranslations();
extern void RegisterUiTranslations();

void InitTranslations() {
    // Core button / common UI translations shared across the whole app.
    // Domain-specific translations should be added by other modules in
    // their own Register*Translations() functions, then invoked from here.
    AddTranslation("en", "BTN_OK",       L"OK");
    AddTranslation("fr", "BTN_OK",       L"OK");
    AddTranslation("en", "BTN_CANCEL",   L"Cancel");
    AddTranslation("fr", "BTN_CANCEL",   L"Annuler");
    AddTranslation("en", "BTN_CLOSE",    L"Close");
    AddTranslation("fr", "BTN_CLOSE",    L"Fermer");
    AddTranslation("en", "BTN_SAVE",     L"Save");
    AddTranslation("fr", "BTN_SAVE",     L"Enregistrer");
    AddTranslation("en", "BTN_DELETE",   L"Delete");
    AddTranslation("fr", "BTN_DELETE",   L"Supprimer");
    AddTranslation("en", "BTN_ADD",      L"Add");
    AddTranslation("fr", "BTN_ADD",      L"Ajouter");
    AddTranslation("en", "BTN_EDIT",     L"Edit");
    AddTranslation("fr", "BTN_EDIT",     L"Modifier");
    AddTranslation("en", "BTN_REMOVE",   L"Remove");
    AddTranslation("fr", "BTN_REMOVE",   L"Retirer");
    AddTranslation("en", "BTN_YES",      L"Yes");
    AddTranslation("fr", "BTN_YES",      L"Oui");
    AddTranslation("en", "BTN_NO",       L"No");
    AddTranslation("fr", "BTN_NO",       L"Non");
    AddTranslation("en", "BTN_BROWSE",   L"Browse...");
    AddTranslation("fr", "BTN_BROWSE",   L"Parcourir...");
    AddTranslation("en", "LANG_ENGLISH", L"English");
    AddTranslation("fr", "LANG_ENGLISH", L"Anglais");
    AddTranslation("en", "LANG_FRENCH",  L"French");
    AddTranslation("fr", "LANG_FRENCH",  L"Français");

    RegisterPlayerTranslations();
    RegisterRcTranslations();
    RegisterUiTranslations();
}

void LocalizeDialog(HWND hDlg) {
    if (!hDlg) return;

    // Walk all top-level child controls and localize their visible text.
    HWND child = GetWindow(hDlg, GW_CHILD);
    while (child) {
        wchar_t text[512];
        int len = GetWindowTextW(child, text, 512);
        if (len > 0) {
            char utf8[1024];
            if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, 1024, nullptr, nullptr) > 0) {
                const wchar_t* translated = T(utf8);
                // Only update if T() returned something different from the
                // current text (avoids needless SetWindowText calls and avoids
                // overwriting with the fallback-converted key).
                if (translated && wcscmp(translated, text) != 0) {
                    SetWindowTextW(child, translated);
                }
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }

    // Localize the dialog caption itself.
    wchar_t cap[512];
    if (GetWindowTextW(hDlg, cap, 512) > 0) {
        char utf8[1024];
        if (WideCharToMultiByte(CP_UTF8, 0, cap, -1, utf8, 1024, nullptr, nullptr) > 0) {
            const wchar_t* translated = T(utf8);
            if (translated && wcscmp(translated, cap) != 0) {
                SetWindowTextW(hDlg, translated);
            }
        }
    }
}

void LocalizeMenu(HMENU hMenu) {
    if (!hMenu) return;
    int count = GetMenuItemCount(hMenu);
    for (int i = 0; i < count; i++) {
        wchar_t buf[256];
        buf[0] = L'\0';
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_STRING | MIIM_SUBMENU;
        mii.dwTypeData = buf;
        mii.cch = 256;
        if (GetMenuItemInfoW(hMenu, i, TRUE, &mii)) {
            if (buf[0] != L'\0') {
                char utf8[1024];
                if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, 1024, nullptr, nullptr) > 0) {
                    const wchar_t* translated = T(utf8);
                    if (translated && wcscmp(translated, buf) != 0) {
                        MENUITEMINFOW set = { sizeof(set) };
                        set.fMask = MIIM_STRING;
                        set.dwTypeData = const_cast<LPWSTR>(translated);
                        SetMenuItemInfoW(hMenu, i, TRUE, &set);
                    }
                }
            }
            if (mii.hSubMenu) {
                LocalizeMenu(mii.hSubMenu);
            }
        }
    }
}
