#include "ui_internal.h"
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for the radio context menu

// ============================================================================
// Radio Dialog
//
// Three independent search backends, all dispatched from the same dialog:
//
//   1. RadioBrowser  (https://api.radio-browser.info)
//      JSON shape: array of station objects with name, url, url_resolved,
//      country, codec, bitrate, language, tags.
//      Filters: name, country, tag (genre), language, bitrateMin, order.
//
//   2. TuneIn  (http://opml.radiotime.com/Search.ashx)
//      OPML/XML response with <outline> elements; each carries a URL
//      attribute pointing at a .pls/.m3u that must be resolved to a
//      direct stream (ResolveTuneInUrl follows redirects up to 3 deep).
//
//   3. iHeartRadio  (api.iheart.com)
//      JSON response of "hits"; a separate station-detail call resolves
//      the streams list. Returns multiple bitrate/codec variants per
//      station — we surface them in a chooser when the user picks the
//      station.
//
// The unified RadioSearchResult struct flattens all three into a single
// type with a `source` discriminator (0=RadioBrowser, 1=TuneIn, 2=iHeart);
// playback code in ResolveStreamUrl() / PlayRadioResult() switches on it.
// ============================================================================

static std::vector<RadioStation> g_radioStations;

// Refresh radio list from database
static void RefreshRadioList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_radioStations = GetRadioFavorites();

    for (const auto& station : g_radioStations) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(station.name.c_str()));
    }
}

// Add station dialog proc
static INT_PTR CALLBACK RadioAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            // If a URL was passed, pre-fill the URL field
            const wchar_t* prefilledUrl = reinterpret_cast<const wchar_t*>(lParam);
            if (prefilledUrl && wcslen(prefilledUrl) > 0) {
                SetDlgItemTextW(hwnd, IDC_RADIO_URL, prefilledUrl);
            }
            SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t name[256] = {0};
                    wchar_t url[512] = {0};
                    GetDlgItemTextW(hwnd, IDC_RADIO_NAME, name, 256);
                    GetDlgItemTextW(hwnd, IDC_RADIO_URL, url, 512);

                    // Validate
                    if (wcslen(name) == 0) {
                        MessageBoxW(hwnd, T("Please enter a station name."), T("Add Station"), MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
                        return TRUE;
                    }
                    if (wcslen(url) == 0) {
                        MessageBoxW(hwnd, T("Please enter a stream URL."), T("Add Station"), MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_RADIO_URL));
                        return TRUE;
                    }

                    // Add to database
                    int id = AddRadioStation(name, url);
                    if (id >= 0) {
                        EndDialog(hwnd, IDOK);
                    } else {
                        MessageBoxW(hwnd, T("Failed to add station."), T("Add Station"), MB_ICONERROR);
                    }
                    return TRUE;
                }

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Radio search results
struct RadioSearchResult {
    std::wstring name;
    std::wstring url;        // Direct URL for RadioBrowser, playlist URL for TuneIn, empty for iHeart
    std::wstring stationId;  // Station ID for iHeartRadio
    std::wstring country;
    std::wstring codec;
    int bitrate;
    int source;              // 0=RadioBrowser, 1=TuneIn, 2=iHeartRadio
};
static std::vector<RadioSearchResult> g_radioSearchResults;
static int g_radioSearchSource = 0;  // Track which source was last used

// Cached list of RadioBrowser country names (lazily fetched once per session)
static std::vector<std::wstring> g_radioCountries;
static volatile bool g_radioCountriesFetched = false;
static volatile bool g_radioCountriesFetching = false;
static const UINT WM_RADIO_COUNTRIES_READY = WM_APP + 11;

// Extended filter caches (tags and languages, same lazy-fetch pattern as
// countries: one HTTP call per session, results pushed back to the dialog
// via a WM_APP message so the UI thread never blocks).
static std::vector<std::wstring> g_radioTags;
static volatile bool g_radioTagsFetched  = false;
static volatile bool g_radioTagsFetching = false;
static const UINT WM_RADIO_TAGS_READY = WM_APP + 12;

static std::vector<std::wstring> g_radioLanguages;
static volatile bool g_radioLanguagesFetched  = false;
static volatile bool g_radioLanguagesFetching = false;
static const UINT WM_RADIO_LANGUAGES_READY = WM_APP + 13;

// Show/hide the advanced filters section (genre/language/bitrate/sort).
// Persisted in the INI under [Radio] AdvancedSearchOpen.
static bool g_radioAdvancedOpen = false;

// HTTP GET request (reused from youtube.cpp pattern)
static std::wstring RadioHttpGet(const std::wstring& url, const wchar_t* extraHeaders = nullptr) {
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"MediaAccess/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    // Use INTERNET_FLAG_SECURE for HTTPS
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (_wcsnicmp(url.c_str(), L"https://", 8) == 0) {
        flags |= INTERNET_FLAG_SECURE;
    }

    // Build headers
    std::wstring headers = L"Accept: */*\r\n";
    if (extraHeaders) {
        headers += extraHeaders;
    }

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), headers.c_str(),
                                          static_cast<DWORD>(headers.length()), flags, 0);
    if (hConnect) {
        char buffer[4096];
        DWORD bytesRead;
        std::string response;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
        InternetCloseHandle(hConnect);

        // Convert UTF-8 to wide string
        if (!response.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
            if (len > 0) {
                result.resize(len);
                MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, &result[0], len);
            }
        }
    }
    InternetCloseHandle(hInternet);
    return result;
}

// URL encode a string
static std::wstring RadioUrlEncode(const std::wstring& str) {
    std::wstring result;
    for (wchar_t c : str) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            result += c;
        } else if (c == L' ') {
            result += L'+';
        } else {
            // Convert to UTF-8 and percent-encode
            char utf8[8];
            int len = WideCharToMultiByte(CP_UTF8, 0, &c, 1, utf8, sizeof(utf8), nullptr, nullptr);
            for (int i = 0; i < len; i++) {
                wchar_t hex[4];
                swprintf(hex, 4, L"%%%02X", (unsigned char)utf8[i]);
                result += hex;
            }
        }
    }
    return result;
}

// Helper to extract JSON string value
static std::wstring ExtractJsonString(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":\"";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    size_t end = start;
    while (end < obj.length()) {
        if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
        end++;
    }
    return obj.substr(start, end - start);
}

// Helper to extract JSON int value
static int ExtractJsonInt(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return 0;
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    return _wtoi(obj.c_str() + start);
}

// Helper to extract JSON value as string (handles both "key":"value" and "key":123)
static std::wstring ExtractJsonValue(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    if (start >= obj.length()) return L"";

    // Check if it's a quoted string
    if (obj[start] == L'"') {
        start++;
        size_t end = start;
        while (end < obj.length()) {
            if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
            end++;
        }
        return obj.substr(start, end - start);
    }

    // It's a number or other unquoted value
    size_t end = start;
    while (end < obj.length() && obj[end] != L',' && obj[end] != L'}' && obj[end] != L']') {
        end++;
    }
    return obj.substr(start, end - start);
}

// Fill the country combo from g_radioCountries, preserving the user's typed text
static void PopulateCountryCombo(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_RADIO_SEARCH_COUNTRY);
    if (!hCombo) return;
    wchar_t current[256] = {0};
    GetWindowTextW(hCombo, current, 256);

    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(Any)"));
    for (const auto& c : g_radioCountries) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(c.c_str()));
    }

    if (current[0]) {
        SetWindowTextW(hCombo, current);
    } else {
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    }
}

// Background thread: fetch RadioBrowser countries and notify dialog
static DWORD WINAPI FetchRadioCountriesThreadProc(LPVOID param) {
    HWND hwnd = reinterpret_cast<HWND>(param);
    std::wstring url = L"https://de1.api.radio-browser.info/json/countries?hidebroken=true&order=stationcount&reverse=true";
    std::wstring json = RadioHttpGet(url);

    if (!json.empty()) {
        size_t pos = 0;
        while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
            int depth = 1;
            size_t endPos = pos + 1;
            bool inString = false;
            while (endPos < json.length() && depth > 0) {
                wchar_t c = json[endPos];
                if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                    inString = !inString;
                } else if (!inString) {
                    if (c == L'{') depth++;
                    else if (c == L'}') depth--;
                }
                endPos++;
            }
            if (depth != 0) break;
            std::wstring obj = json.substr(pos, endPos - pos);
            std::wstring name = ExtractJsonString(obj, L"name");
            if (!name.empty()) {
                g_radioCountries.push_back(name);
            }
            pos = endPos;
        }
    }

    g_radioCountriesFetched = true;
    g_radioCountriesFetching = false;
    if (IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RADIO_COUNTRIES_READY, 0, 0);
    }
    return 0;
}

static void EnsureRadioCountriesFetched(HWND hwnd) {
    if (g_radioCountriesFetched || g_radioCountriesFetching) return;
    g_radioCountriesFetching = true;
    HANDLE th = CreateThread(nullptr, 0, FetchRadioCountriesThreadProc, hwnd, 0, nullptr);
    if (th) CloseHandle(th);
    else g_radioCountriesFetching = false;
}

// Generic list fetcher for RadioBrowser's /tags and /languages endpoints.
// Same JSON-array parsing logic as countries (each object has a "name").
static void FetchRadioListInto(const std::wstring& url,
                               std::vector<std::wstring>& out) {
    std::wstring json = RadioHttpGet(url);
    if (json.empty()) return;
    size_t pos = 0;
    while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
        int depth = 1;
        size_t endPos = pos + 1;
        bool inString = false;
        while (endPos < json.length() && depth > 0) {
            wchar_t c = json[endPos];
            if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == L'{') depth++;
                else if (c == L'}') depth--;
            }
            endPos++;
        }
        if (depth != 0) break;
        std::wstring obj = json.substr(pos, endPos - pos);
        std::wstring name = ExtractJsonString(obj, L"name");
        if (!name.empty()) out.push_back(name);
        pos = endPos;
    }
}

static DWORD WINAPI FetchRadioTagsThreadProc(LPVOID param) {
    HWND hwnd = reinterpret_cast<HWND>(param);
    // Cap to top 500 by station count — the full list is enormous and most
    // entries are typos / one-station tags that are useless as filters.
    FetchRadioListInto(
        L"https://de1.api.radio-browser.info/json/tags?hidebroken=true"
        L"&order=stationcount&reverse=true&limit=500",
        g_radioTags);
    g_radioTagsFetched  = true;
    g_radioTagsFetching = false;
    if (IsWindow(hwnd)) PostMessageW(hwnd, WM_RADIO_TAGS_READY, 0, 0);
    return 0;
}

static DWORD WINAPI FetchRadioLanguagesThreadProc(LPVOID param) {
    HWND hwnd = reinterpret_cast<HWND>(param);
    FetchRadioListInto(
        L"https://de1.api.radio-browser.info/json/languages?hidebroken=true"
        L"&order=stationcount&reverse=true",
        g_radioLanguages);
    g_radioLanguagesFetched  = true;
    g_radioLanguagesFetching = false;
    if (IsWindow(hwnd)) PostMessageW(hwnd, WM_RADIO_LANGUAGES_READY, 0, 0);
    return 0;
}

static void EnsureRadioTagsFetched(HWND hwnd) {
    if (g_radioTagsFetched || g_radioTagsFetching) return;
    g_radioTagsFetching = true;
    HANDLE th = CreateThread(nullptr, 0, FetchRadioTagsThreadProc, hwnd, 0, nullptr);
    if (th) CloseHandle(th);
    else g_radioTagsFetching = false;
}

static void EnsureRadioLanguagesFetched(HWND hwnd) {
    if (g_radioLanguagesFetched || g_radioLanguagesFetching) return;
    g_radioLanguagesFetching = true;
    HANDLE th = CreateThread(nullptr, 0, FetchRadioLanguagesThreadProc, hwnd, 0, nullptr);
    if (th) CloseHandle(th);
    else g_radioLanguagesFetching = false;
}

// Populate genre + language combos from the cached lists; preserves the
// user's currently-typed text so an in-progress filter isn't clobbered
// when the async fetch completes.
static void PopulateGenreCombo(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_RADIO_SEARCH_GENRE);
    if (!hCombo) return;
    wchar_t current[256] = {0};
    GetWindowTextW(hCombo, current, 256);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)T("(Any)"));
    for (const auto& t : g_radioTags) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
    }
    if (current[0]) SetWindowTextW(hCombo, current);
    else            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

static void PopulateLanguageCombo(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LANGUAGE);
    if (!hCombo) return;
    wchar_t current[256] = {0};
    GetWindowTextW(hCombo, current, 256);
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)T("(Any)"));
    for (const auto& l : g_radioLanguages) {
        SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)l.c_str());
    }
    if (current[0]) SetWindowTextW(hCombo, current);
    else            SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

// Static-list combos: bitrate floor and sort order. Each combo carries a
// hidden item-data integer so the search code can read the chosen value
// without parsing labels (and the labels can be localized freely).
static void PopulateBitrateCombo(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_RADIO_SEARCH_BITRATE);
    if (!hCombo) return;
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    int items[] = {0, 64, 128, 256};
    const wchar_t* labels[] = { T("(Any)"), L"64 kbps+", L"128 kbps+", L"256 kbps+" };
    for (int i = 0; i < 4; ++i) {
        int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)labels[i]);
        SendMessageW(hCombo, CB_SETITEMDATA, idx, items[i]);
    }
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

// Sort options. Item-data is an index into kSortKeys defined in the
// search code below — labels can stay localized.
static void PopulateSortCombo(HWND hwnd) {
    HWND hCombo = GetDlgItem(hwnd, IDC_RADIO_SEARCH_SORT);
    if (!hCombo) return;
    SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
    const wchar_t* labels[] = {
        T("Popularity"),
        T("Name"),
        T("Bitrate"),
        T("Last checked"),
    };
    for (int i = 0; i < 4; ++i) {
        int idx = (int)SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)labels[i]);
        SendMessageW(hCombo, CB_SETITEMDATA, idx, i);
    }
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
}

// Sort keys parallel to the PopulateSortCombo labels above.
// 0 = Popularity (clickcount desc), 1 = Name asc, 2 = Bitrate desc,
// 3 = Last checked (lastchangetime desc).
static const struct { const char* name; bool reverse; } kSortKeys[] = {
    { "clickcount",      true  },
    { "name",            false },
    { "bitrate",         true  },
    { "lastchangetime",  true  },
};

// =========================================================================
// Backend 1: RadioBrowser  (https://api.radio-browser.info)
// =========================================================================
//
// Search RadioBrowser's /stations/search endpoint.
// All named filters are optional. Pass empty / 0 to disable each:
//   tag         genre / category (RadioBrowser tag field)
//   language    language name
//   bitrateMin  in kbps; 0 = no floor
//   sortKey     index into kSortKeys; -1 = default (popularity when query
//               is empty, otherwise let RadioBrowser pick relevance)
//
// JSON shape (one entry per station):
//   { "name": "...", "url": "...", "url_resolved": "...",
//     "country": "...", "codec": "...", "bitrate": 128, ... }
// We prefer url_resolved (already redirect-followed by the API).
static bool SearchRadioBrowser(const std::wstring& query,
                               const std::wstring& country,
                               const std::wstring& tag,
                               const std::wstring& language,
                               int bitrateMin,
                               int sortKey,
                               std::vector<RadioSearchResult>& results) {
    results.clear();

    // Use search endpoint; all named filters are optional.
    std::wstring url = L"https://de1.api.radio-browser.info/json/stations/search?limit=100&hidebroken=true";
    if (!query.empty()) {
        url += L"&name=" + RadioUrlEncode(query);
    }
    if (!country.empty()) {
        url += L"&country=" + RadioUrlEncode(country);
    }
    if (!tag.empty()) {
        url += L"&tag=" + RadioUrlEncode(tag);
    }
    if (!language.empty()) {
        url += L"&language=" + RadioUrlEncode(language);
    }
    if (bitrateMin > 0) {
        wchar_t buf[32];
        swprintf(buf, 32, L"&bitrateMin=%d", bitrateMin);
        url += buf;
    }
    if (sortKey >= 0 && sortKey < (int)(sizeof(kSortKeys)/sizeof(kSortKeys[0]))) {
        url += L"&order=";
        // ASCII keys, no encoding needed.
        for (const char* p = kSortKeys[sortKey].name; *p; ++p) url += (wchar_t)*p;
        if (kSortKeys[sortKey].reverse) url += L"&reverse=true";
    } else if (query.empty() && (!country.empty() || !tag.empty() || !language.empty())) {
        // No explicit sort but a filter is present without a name search —
        // surface the most popular stations first (the country-only
        // browse case used to ship without any explicit sort).
        url += L"&order=clickcount&reverse=true";
    }
    std::wstring json = RadioHttpGet(url);
    if (json.empty()) return false;

    // Simple JSON array parser - find each object by tracking brace depth
    size_t pos = 0;
    while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace by tracking depth
        int depth = 1;
        size_t endPos = pos + 1;
        bool inString = false;
        while (endPos < json.length() && depth > 0) {
            wchar_t c = json[endPos];
            if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == L'{') depth++;
                else if (c == L'}') depth--;
            }
            endPos++;
        }
        if (depth != 0) break;

        std::wstring obj = json.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 0;  // RadioBrowser

        result.name = ExtractJsonString(obj, L"name");
        result.url = ExtractJsonString(obj, L"url_resolved");
        if (result.url.empty()) result.url = ExtractJsonString(obj, L"url");
        result.country = ExtractJsonString(obj, L"country");
        result.codec = ExtractJsonString(obj, L"codec");
        result.bitrate = ExtractJsonInt(obj, L"bitrate");

        // Only add if we have a name and URL
        if (!result.name.empty() && !result.url.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Parse playlist content (M3U or PLS) to extract stream URL
static std::wstring ParsePlaylistContent(const std::string& content) {
    // Make lowercase copy for case-insensitive searching
    std::string lower = content;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Check if it's a PLS file (case-insensitive)
    if (lower.find("[playlist]") != std::string::npos) {
        // Look for File1= (case-insensitive search, then extract from original)
        size_t filePos = lower.find("file1=");
        if (filePos != std::string::npos) {
            filePos += 6;
            size_t endPos = content.find_first_of("\r\n", filePos);
            if (endPos == std::string::npos) endPos = content.length();
            std::string url = content.substr(filePos, endPos - filePos);
            // Trim whitespace
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
            if (!url.empty()) return Utf8ToWide(url);
        }
    }

    // Check if it's an M3U file
    // Look for first non-comment, non-empty line that starts with http
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty() || line[0] == '#') continue;
        if (line.find("http") == 0) {
            return Utf8ToWide(line);
        }
    }

    // Maybe the content itself is a redirect URL
    if (content.find("http") == 0) {
        size_t endPos = content.find_first_of("\r\n \t");
        if (endPos == std::string::npos) endPos = content.length();
        return Utf8ToWide(content.substr(0, endPos));
    }

    return L"";
}

// Check if URL looks like a playlist file
static bool IsPlaylistUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& c : lower) c = towlower(c);

    // Check extension (before any query string)
    size_t queryPos = lower.find(L'?');
    std::wstring path = (queryPos != std::wstring::npos) ? lower.substr(0, queryPos) : lower;

    return path.length() > 4 && (
        path.substr(path.length() - 4) == L".m3u" ||
        path.substr(path.length() - 4) == L".pls" ||
        (path.length() > 5 && path.substr(path.length() - 5) == L".m3u8")
    );
}

// Stream option for multi-URL selection
struct StreamOption {
    std::wstring url;
    std::wstring label;
};

// Parse playlist content (M3U or PLS) to extract ALL stream URLs
static std::vector<StreamOption> ParsePlaylistContentMultiple(const std::string& content) {
    std::vector<StreamOption> urls;

    // Make lowercase copy for case-insensitive searching
    std::string lower = content;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Check if it's a PLS file (case-insensitive)
    if (lower.find("[playlist]") != std::string::npos) {
        // Look for File1=, File2=, etc.
        for (int i = 1; i <= 20; i++) {
            std::string key = "file" + std::to_string(i) + "=";
            size_t filePos = lower.find(key);
            if (filePos == std::string::npos) continue;

            filePos += key.length();
            size_t endPos = content.find_first_of("\r\n", filePos);
            if (endPos == std::string::npos) endPos = content.length();
            std::string url = content.substr(filePos, endPos - filePos);
            // Trim whitespace
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);

            if (!url.empty()) {
                // Look for corresponding Title
                std::string titleKey = "title" + std::to_string(i) + "=";
                size_t titlePos = lower.find(titleKey);
                std::wstring label = L"Stream " + std::to_wstring(i);
                if (titlePos != std::string::npos) {
                    titlePos += titleKey.length();
                    size_t titleEnd = content.find_first_of("\r\n", titlePos);
                    if (titleEnd == std::string::npos) titleEnd = content.length();
                    std::string title = content.substr(titlePos, titleEnd - titlePos);
                    while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();
                    while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) title.erase(0, 1);
                    if (!title.empty()) label = Utf8ToWide(title);
                }
                urls.push_back({Utf8ToWide(url), label});
            }
        }
        if (!urls.empty()) return urls;
    }

    // Check if it's an M3U file - get all http URLs
    std::istringstream iss(content);
    std::string line;
    std::string pendingTitle;
    int streamNum = 1;
    while (std::getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty()) continue;

        // Check for #EXTINF line with title
        if (line.find("#EXTINF:") == 0) {
            size_t commaPos = line.find(',');
            if (commaPos != std::string::npos && commaPos + 1 < line.length()) {
                pendingTitle = line.substr(commaPos + 1);
            }
            continue;
        }

        if (line[0] == '#') continue;

        if (line.find("http") == 0) {
            std::wstring label = pendingTitle.empty() ?
                L"Stream " + std::to_wstring(streamNum) : Utf8ToWide(pendingTitle);
            urls.push_back({Utf8ToWide(line), label});
            pendingTitle.clear();
            streamNum++;
        }
    }

    return urls;
}

// =========================================================================
// Backend 2: TuneIn  (http://opml.radiotime.com/Search.ashx)
// =========================================================================
//
// TuneIn's public search returns OPML (XML) where each <outline> for a
// station carries a URL attribute pointing at a .pls / .m3u file. That
// playlist must then be fetched and parsed to extract one or more direct
// stream URLs. ResolveTuneInUrl walks up to 3 levels of nested playlists
// before giving up.

// Resolve a TuneIn playlist URL to get the actual stream URL
static std::wstring ResolveTuneInUrl(const std::wstring& playlistUrl) {
    std::wstring currentUrl = playlistUrl;

    // Follow up to 3 levels of playlist redirects
    for (int i = 0; i < 3; i++) {
        std::wstring content = RadioHttpGet(currentUrl);
        if (content.empty()) return L"";

        // Convert to narrow string for parsing
        std::string narrow = WideToUtf8(content);

        // Parse the playlist content
        std::wstring streamUrl = ParsePlaylistContent(narrow);
        if (streamUrl.empty()) return L"";

        // If the result is another playlist, fetch and parse it
        if (IsPlaylistUrl(streamUrl)) {
            currentUrl = streamUrl;
            continue;
        }

        // Found a direct stream URL
        return streamUrl;
    }

    // If we've followed too many redirects, return the last URL we got
    return currentUrl;
}

// Resolve a TuneIn playlist URL to get ALL stream URLs
static std::vector<StreamOption> ResolveTuneInUrls(const std::wstring& playlistUrl) {
    std::vector<StreamOption> result;
    std::wstring currentUrl = playlistUrl;

    // First fetch the playlist
    std::wstring content = RadioHttpGet(currentUrl);
    if (content.empty()) return result;

    std::string narrow = WideToUtf8(content);

    // Get all URLs from the playlist
    result = ParsePlaylistContentMultiple(narrow);

    // If we got multiple results, check if any are playlists themselves and resolve them
    if (result.size() == 1 && IsPlaylistUrl(result[0].url)) {
        // Single result is another playlist, follow it
        std::wstring nested = RadioHttpGet(result[0].url);
        if (!nested.empty()) {
            std::string nestedNarrow = WideToUtf8(nested);
            auto nestedUrls = ParsePlaylistContentMultiple(nestedNarrow);
            if (!nestedUrls.empty()) {
                result = nestedUrls;
            }
        }
    }

    // Resolve any entries that are themselves playlists, then deduplicate by the
    // FINAL stream URL. TuneIn's Tune.ashx can return an M3U with several entries
    // (e.g. http + https variants) that each point at a StreamTheWorld .pls — one
    // more resolution hop. Previously these playlist URLs were simply DROPPED
    // here, leaving QUB Radio (and other StreamTheWorld stations) with an empty
    // list and a "Could not get stream URL" error. Resolve them instead.
    std::vector<StreamOption> filtered;
    std::set<std::wstring> seen;
    for (const auto& opt : result) {
        std::wstring url = opt.url;
        if (IsPlaylistUrl(url)) {
            std::wstring resolved = ResolveTuneInUrl(url);
            // ResolveTuneInUrl can fall through still pointing at a playlist when
            // it exhausts its 3-hop budget; skip such unresolvable entries.
            if (resolved.empty() || IsPlaylistUrl(resolved)) continue;
            url = resolved;
        }
        if (seen.find(url) == seen.end()) {
            filtered.push_back({url, opt.label});
            seen.insert(url);
        }
    }

    return filtered;
}

// Search TuneIn API (returns OPML/XML)
static bool SearchTuneIn(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // TuneIn search endpoint
    std::wstring url = L"http://opml.radiotime.com/Search.ashx?query=" + RadioUrlEncode(query);
    std::wstring xml = RadioHttpGet(url);
    if (xml.empty()) return false;

    // Parse OPML - look for <outline> elements with type="audio".
    //
    // We must scope each <outline ...> match to ONLY the opening tag, not to
    // the next `/>` we find — TuneIn often wraps stations in container
    // outlines (`<outline type="link" text="Stations"> ... <outline type="audio"
    // .../> ... </outline>`). Using `find("/>")` from the parent's opening
    // position used to capture the FIRST child's self-closing tag, leaking
    // the child's `type="audio"` into the parent and emitting a phantom row
    // labelled "Search results" or similar that wasn't a real station.
    // Finding the next plain `>` correctly bounds the opening tag and the
    // existing `type="audio"` filter naturally rejects container outlines.
    size_t pos = 0;
    while ((pos = xml.find(L"<outline", pos)) != std::wstring::npos) {
        size_t endPos = xml.find(L'>', pos);
        if (endPos == std::wstring::npos) break;

        std::wstring elem = xml.substr(pos, endPos - pos + 1);

        // Check if it's an audio type (station)
        if (elem.find(L"type=\"audio\"") != std::wstring::npos) {
            RadioSearchResult result;
            result.source = 1;  // TuneIn
            result.bitrate = 0;

            // Extract attributes
            auto extractAttr = [&elem](const std::wstring& attr) -> std::wstring {
                std::wstring search = attr + L"=\"";
                size_t start = elem.find(search);
                if (start == std::wstring::npos) return L"";
                start += search.length();
                size_t end = elem.find(L'"', start);
                if (end == std::wstring::npos) return L"";
                return elem.substr(start, end - start);
            };

            result.name = extractAttr(L"text");
            result.url = extractAttr(L"URL");  // Store TuneIn URL, resolve later
            std::wstring subtext = extractAttr(L"subtext");

            // subtext often contains location info
            if (!subtext.empty()) {
                result.country = subtext;
            }

            // Bitrate might be in bitrate attribute
            std::wstring bitrateStr = extractAttr(L"bitrate");
            if (!bitrateStr.empty()) {
                result.bitrate = _wtoi(bitrateStr.c_str());
            }

            // Decode HTML entities in name
            size_t ampPos;
            while ((ampPos = result.name.find(L"&amp;")) != std::wstring::npos) {
                result.name.replace(ampPos, 5, L"&");
            }
            while ((ampPos = result.name.find(L"&apos;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"'");
            }
            while ((ampPos = result.name.find(L"&quot;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"\"");
            }

            // Only add if we have a name and URL
            if (!result.name.empty() && !result.url.empty()) {
                results.push_back(result);
            }
        }

        pos = endPos + 1;
    }

    return !results.empty();
}

// =========================================================================
// Backend 3: iHeartRadio  (api.iheart.com)
// =========================================================================
//
// Two-step API: /api/v3/search/all for the station list, then a
// per-station call to fetch the streams[] array (each entry has a codec,
// bitrate and a direct URL). We surface every variant when the user
// picks a station so they can choose between, e.g., 64 kbps AAC and
// 128 kbps MP3.

// Get stream URL for an iHeartRadio station by ID
static std::wstring GetIHeartStreamUrl(const std::wstring& stationId) {
    // Use the live station API to get stream URLs
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations/" + stationId;
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");
    if (json.empty()) return L"";

    // Look for streams section
    size_t streamsPos = json.find(L"\"streams\"");
    if (streamsPos == std::wstring::npos) return L"";

    std::wstring streamsSection = json.substr(streamsPos);

    // Try different stream types in order of preference
    std::wstring streamUrl = ExtractJsonString(streamsSection, L"shoutcast_stream");
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"secure_shoutcast_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"pls_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"hls_stream");
    }

    return streamUrl;
}

// Get ALL stream URLs for an iHeartRadio station by ID
static std::vector<StreamOption> GetIHeartStreamUrls(const std::wstring& stationId) {
    std::vector<StreamOption> result;

    // Use the live station API to get stream URLs
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations/" + stationId;
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");
    if (json.empty()) return result;

    // Look for streams section
    size_t streamsPos = json.find(L"\"streams\"");
    if (streamsPos == std::wstring::npos) return result;

    std::wstring streamsSection = json.substr(streamsPos);

    // Get all available stream types
    struct { const wchar_t* key; const wchar_t* label; } streamTypes[] = {
        {L"shoutcast_stream", L"Shoutcast"},
        {L"secure_shoutcast_stream", L"Shoutcast (Secure)"},
        {L"pls_stream", L"PLS"},
        {L"hls_stream", L"HLS"},
        {L"stw_stream", L"STW"},
        {L"flv_stream", L"FLV"},
        {L"secure_pls_stream", L"PLS (Secure)"},
        {L"secure_hls_stream", L"HLS (Secure)"},
    };

    std::set<std::wstring> seenUrls;
    for (const auto& type : streamTypes) {
        std::wstring streamUrl = ExtractJsonString(streamsSection, type.key);
        if (!streamUrl.empty() && seenUrls.find(streamUrl) == seenUrls.end()) {
            result.push_back({streamUrl, type.label});
            seenUrls.insert(streamUrl);
        }
    }

    return result;
}

// Search iHeartRadio API
static bool SearchIHeartRadio(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // Try the v2 live stations search endpoint
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations?countryCode=US&limit=20&q=" + RadioUrlEncode(query);
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");

    // If v2 fails, try v3
    if (json.empty() || json.find(L"\"hits\"") == std::wstring::npos) {
        url = L"https://api.iheart.com/api/v3/search/all?keywords=" + RadioUrlEncode(query) +
              L"&startIndex=0&maxRows=20";
        json = RadioHttpGet(url, L"Accept: application/json\r\n");
    }

    if (json.empty()) return false;

    // Find the array containing stations - try multiple possible locations
    size_t arrayStart = std::wstring::npos;

    // Try "hits" array first (v2 response)
    size_t hitsPos = json.find(L"\"hits\"");
    if (hitsPos != std::wstring::npos) {
        arrayStart = json.find(L'[', hitsPos);
    }

    // Try "stations" -> "results" (v3 response)
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            size_t resultsPos = json.find(L"\"results\"", stationsPos);
            if (resultsPos != std::wstring::npos) {
                arrayStart = json.find(L'[', resultsPos);
            }
        }
    }

    // Try just finding first array after "stations"
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            arrayStart = json.find(L'[', stationsPos);
        }
    }

    if (arrayStart == std::wstring::npos) return false;

    // Find array end
    size_t arrayEnd = arrayStart + 1;
    int depth = 1;
    bool inString = false;
    while (arrayEnd < json.length() && depth > 0) {
        wchar_t c = json[arrayEnd];
        if (c == L'"' && (arrayEnd == 0 || json[arrayEnd-1] != L'\\')) {
            inString = !inString;
        } else if (!inString) {
            if (c == L'[') depth++;
            else if (c == L']') depth--;
        }
        arrayEnd++;
    }

    std::wstring stationsArray = json.substr(arrayStart, arrayEnd - arrayStart);

    // Parse each station object
    size_t pos = 0;
    while ((pos = stationsArray.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace
        int objDepth = 1;
        size_t endPos = pos + 1;
        bool objInString = false;
        while (endPos < stationsArray.length() && objDepth > 0) {
            wchar_t c = stationsArray[endPos];
            if (c == L'"' && (endPos == 0 || stationsArray[endPos-1] != L'\\')) {
                objInString = !objInString;
            } else if (!objInString) {
                if (c == L'{') objDepth++;
                else if (c == L'}') objDepth--;
            }
            endPos++;
        }
        if (objDepth != 0) break;

        std::wstring obj = stationsArray.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 2;  // iHeartRadio
        result.bitrate = 0;

        result.name = ExtractJsonString(obj, L"name");
        if (result.name.empty()) {
            result.name = ExtractJsonString(obj, L"description");
        }

        result.stationId = ExtractJsonValue(obj, L"id");
        result.country = ExtractJsonString(obj, L"city");
        std::wstring state = ExtractJsonString(obj, L"state");
        if (!state.empty()) {
            if (!result.country.empty()) result.country += L", ";
            result.country += state;
        }

        // Get call letters for display
        std::wstring callLetters = ExtractJsonString(obj, L"callLetters");
        if (!callLetters.empty() && result.name.find(callLetters) == std::wstring::npos) {
            result.name = callLetters + L" - " + result.name;
        }

        // Only add if we have a name and station ID (URL resolved later)
        if (!result.name.empty() && !result.stationId.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Resolve stream URL for a radio search result (called when playing/adding)
static std::wstring ResolveRadioStreamUrl(const RadioSearchResult& result) {
    std::wstring url;

    if (result.source == 0) {
        // RadioBrowser - URL is already resolved
        url = result.url;
    } else if (result.source == 1) {
        // TuneIn - resolve playlist URL to get actual stream
        url = ResolveTuneInUrl(result.url);
    } else if (result.source == 2) {
        // iHeartRadio - get stream URL from station ID
        url = GetIHeartStreamUrl(result.stationId);
    } else {
        url = result.url;
    }

    // Safety check: if the resolved URL is still a playlist, try to resolve it
    if (!url.empty() && IsPlaylistUrl(url)) {
        std::wstring resolved = ResolveTuneInUrl(url);
        if (!resolved.empty()) {
            url = resolved;
        }
    }

    return url;
}

// Resolve ALL stream URLs for a radio search result (for multi-URL selection)
static std::vector<StreamOption> ResolveRadioStreamUrls(const RadioSearchResult& result) {
    std::vector<StreamOption> urls;

    if (result.source == 0) {
        // RadioBrowser - URL is already resolved, just return it
        if (!result.url.empty()) {
            urls.push_back({result.url, L"Stream"});
        }
    } else if (result.source == 1) {
        // TuneIn - resolve playlist URL to get all streams
        urls = ResolveTuneInUrls(result.url);
    } else if (result.source == 2) {
        // iHeartRadio - get all stream URLs from station ID
        urls = GetIHeartStreamUrls(result.stationId);
    } else {
        if (!result.url.empty()) {
            urls.push_back({result.url, L"Stream"});
        }
    }

    // Safety check: resolve any remaining playlist URLs
    for (auto& opt : urls) {
        if (IsPlaylistUrl(opt.url)) {
            std::wstring resolved = ResolveTuneInUrl(opt.url);
            if (!resolved.empty()) {
                opt.url = resolved;
            }
        }
    }

    return urls;
}

// Show context menu for stream URL selection
// Returns the selected URL, or empty string if cancelled
static std::wstring ShowStreamSelectionMenu(HWND hwnd, const std::vector<StreamOption>& options) {
    if (options.empty()) return L"";
    if (options.size() == 1) return options[0].url;  // Only one option, no menu needed

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return options[0].url;

    for (size_t i = 0; i < options.size(); i++) {
        AppendMenuW(hMenu, MF_STRING, i + 1, options[i].label.c_str());
    }

    // Get cursor position for menu
    POINT pt;
    GetCursorPos(&pt);

    // Show the menu and wait for selection
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    if (cmd > 0 && cmd <= static_cast<int>(options.size())) {
        return options[cmd - 1].url;
    }

    return L"";  // Cancelled
}

// Show/hide radio dialog controls based on tab
static void UpdateRadioTabVisibility(HWND hwnd, int tab) {
    // Favorites tab controls (tab 0)
    int favCtrls[] = {IDC_RADIO_LIST, IDC_RADIO_ADD, IDC_RADIO_IMPORT, IDC_RADIO_EXPORT};
    // Search tab controls (tab 1)
    int searchCtrls[] = {IDC_RADIO_SEARCH_SOURCE, IDC_RADIO_SEARCH_EDIT, IDC_RADIO_SEARCH_BTN,
                         IDC_RADIO_SEARCH_LIST_LABEL, IDC_RADIO_SEARCH_LIST, IDC_RADIO_SEARCH_ADD};

    for (int id : favCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int id : searchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }

    // Country controls: only visible on search tab with RadioBrowser selected
    int src = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_SOURCE, CB_GETCURSEL, 0, 0));
    bool isRadioBrowser = (tab == 1 && src == 0);
    ShowWindow(GetDlgItem(hwnd, IDC_RADIO_SEARCH_COUNTRY), isRadioBrowser ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_RADIO_SEARCH_COUNTRY_LABEL), isRadioBrowser ? SW_SHOW : SW_HIDE);

    // Advanced-filters checkbox: only meaningful for RadioBrowser.
    // The expanded row below it is shown only when both conditions hold:
    // we're on search/RadioBrowser AND the user ticked the checkbox.
    ShowWindow(GetDlgItem(hwnd, IDC_RADIO_SEARCH_ADVANCED),
               isRadioBrowser ? SW_SHOW : SW_HIDE);

    bool showAdvanced = isRadioBrowser && g_radioAdvancedOpen;
    int advCtrls[] = {
        IDC_RADIO_SEARCH_GENRE,    IDC_RADIO_SEARCH_GENRE_LABEL,
        IDC_RADIO_SEARCH_LANGUAGE, IDC_RADIO_SEARCH_LANGUAGE_LABEL,
        IDC_RADIO_SEARCH_BITRATE,  IDC_RADIO_SEARCH_BITRATE_LABEL,
        IDC_RADIO_SEARCH_SORT,     IDC_RADIO_SEARCH_SORT_LABEL,
    };
    for (int id : advCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), showAdvanced ? SW_SHOW : SW_HIDE);
    }

    // The result-count static lives on the search tab only.
    ShowWindow(GetDlgItem(hwnd, IDC_RADIO_SEARCH_RESULT_COUNT),
               tab == 1 ? SW_SHOW : SW_HIDE);
}

// Radio list subclass proc for keyboard handling
static WNDPROC g_origRadioListProc = nullptr;
static WNDPROC g_origRadioSearchListProc = nullptr;
static WNDPROC g_origRadioSearchEditProc = nullptr;

// Search edit subclass proc - handle Enter to trigger search
static LRESULT CALLBACK RadioSearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        // Trigger search button click
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_RADIO_SEARCH_BTN, BN_CLICKED), 0);
        return 0;
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && pmsg->wParam == VK_RETURN) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioSearchEditProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioSearchEditProc, hwnd, msg, wParam, lParam);
}

// Resolve a search result to a single stream URL, prompting the user with a
// menu when multiple TuneIn / iHeartRadio options are available. Returns the
// chosen URL (empty on failure or cancel). `outHadOptions` reports whether
// any resolved URLs were found at all — callers use this to distinguish
// "user cancelled" (silent) from "resolve failed" (speak an error).
// `menuOwner` is the HWND that should own the popup menu (the dialog).
static std::wstring ResolveAndPickStreamUrl(HWND menuOwner,
                                            const RadioSearchResult& r,
                                            bool& outHadOptions) {
    std::wstring streamUrl;
    outHadOptions = false;

    if (r.source == 1 || r.source == 2) {
        // TuneIn / iHeartRadio may expose several streams — let the user pick
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        auto urls = ResolveRadioStreamUrls(r);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        outHadOptions = !urls.empty();
        if (urls.size() > 1) {
            streamUrl = ShowStreamSelectionMenu(menuOwner, urls);
        } else if (!urls.empty()) {
            streamUrl = urls[0].url;
        }
    } else {
        // RadioBrowser - already a single direct URL
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        streamUrl = ResolveRadioStreamUrl(r);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        outHadOptions = !streamUrl.empty();
    }

    return streamUrl;
}

// Copy a wide string to the clipboard. Returns true on success. Speaks
// nothing — caller decides on the announcement.
static bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    bool ok = false;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
        if (pMem) {
            wcscpy(pMem, text.c_str());
            GlobalUnlock(hMem);
            if (SetClipboardData(CF_UNICODETEXT, hMem)) ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

// Search list subclass proc
static LRESULT CALLBACK RadioSearchListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Play selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                const auto& r = g_radioSearchResults[sel];
                bool hadOptions = false;
                std::wstring streamUrl = ResolveAndPickStreamUrl(GetParent(hwnd), r, hadOptions);

                if (!streamUrl.empty()) {
                    // Preset the station name BEFORE PlayTrack so the
                    // title shows "Station - ..." from the get-go.
                    SetNowPlaying(SourceType::RadioFavorite, r.name, L"");
                    g_playlist.clear();
                    g_playlist.push_back(streamUrl);
                    PlayTrack(0);
                } else if (!hadOptions) {
                    Speak(Ts("Could not get stream URL"));
                }
                // If hadOptions but streamUrl is empty, user cancelled - do nothing
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        } else if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Copy stream URL to clipboard
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                const auto& r = g_radioSearchResults[sel];
                bool hadOptions = false;
                std::wstring streamUrl = ResolveAndPickStreamUrl(GetParent(hwnd), r, hadOptions);

                if (!streamUrl.empty()) {
                    if (CopyToClipboard(hwnd, streamUrl)) {
                        Speak(Ts("URL copied"));
                    }
                } else {
                    Speak(Ts("Could not get stream URL"));
                }
            }
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioSearchListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioSearchListProc, hwnd, msg, wParam, lParam);
}

// Edit station dialog data
static std::wstring g_editStationName;
static std::wstring g_editStationUrl;
static INT_PTR CALLBACK EditStationDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            SetWindowTextW(hwnd, T("Edit Station"));
            SetDlgItemTextW(hwnd, IDC_RADIO_NAME, g_editStationName.c_str());
            SetDlgItemTextW(hwnd, IDC_RADIO_URL, g_editStationUrl.c_str());
            // Select all text in name field
            SendDlgItemMessageW(hwnd, IDC_RADIO_NAME, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_RADIO_NAME));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                wchar_t buf[4096];
                GetDlgItemTextW(hwnd, IDC_RADIO_NAME, buf, 512);
                g_editStationName = buf;
                GetDlgItemTextW(hwnd, IDC_RADIO_URL, buf, 4096);
                g_editStationUrl = buf;
                EndDialog(hwnd, IDOK);
                return TRUE;
            } else if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static LRESULT CALLBACK RadioListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Play selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                // Preset favorite name before PlayTrack so the title bar
                // reads "Station - ..." immediately.
                SetNowPlaying(SourceType::RadioFavorite,
                              g_radioStations[sel].name, L"");
                g_playlist.clear();
                g_playlist.push_back(g_radioStations[sel].url);
                PlayTrack(0);
            }
            return 0;
        } else if (wParam == VK_F2) {
            // Edit selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                g_editStationName = g_radioStations[sel].name;
                g_editStationUrl = g_radioStations[sel].url;
                if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO_ADD),
                               GetParent(hwnd), EditStationDlgProc) == IDOK) {
                    // Trim leading/trailing whitespace from both fields
                    auto trim = [](std::wstring& s) {
                        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(0, 1);
                        while (!s.empty() && (s.back()  == L' ' || s.back()  == L'\t')) s.pop_back();
                    };
                    trim(g_editStationName);
                    trim(g_editStationUrl);
                    if (!g_editStationName.empty() && !g_editStationUrl.empty()) {
                        if (UpdateRadioStation(g_radioStations[sel].id, g_editStationName, g_editStationUrl)) {
                            Speak(Ts("Station updated"));
                            RefreshRadioList(GetParent(hwnd));
                            SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                        }
                    }
                }
            }
            return 0;
        } else if (wParam == VK_DELETE) {
            // Remove selected station
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                if (RemoveRadioStation(g_radioStations[sel].id)) {
                    Speak(Ts("Station removed"));
                    RefreshRadioList(GetParent(hwnd));
                    // Select next item or previous if at end
                    int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
                    if (count > 0) {
                        if (sel >= count) sel = count - 1;
                        SendMessageW(hwnd, LB_SETCURSEL, sel, 0);
                    }
                }
            }
            return 0;
        } else if ((wParam == VK_UP || wParam == VK_DOWN) && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Reorder station with Ctrl+Up/Down
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            int count = static_cast<int>(g_radioStations.size());
            int newSel = (wParam == VK_UP) ? sel - 1 : sel + 1;
            if (sel >= 0 && sel < count && newSel >= 0 && newSel < count) {
                std::swap(g_radioStations[sel], g_radioStations[newSel]);
                UpdateRadioSortOrders(g_radioStations);
                RefreshRadioList(GetParent(hwnd));
                SendMessageW(hwnd, LB_SETCURSEL, newSel, 0);
                // Speak position feedback
                const std::wstring& name = g_radioStations[newSel].name;
                if (newSel == 0)
                    SpeakW(name + T(" moved to top"));
                else
                    SpeakW(name + T(" moved below ") + g_radioStations[newSel - 1].name);
            }
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Close dialog
            SendMessageW(GetParent(hwnd), WM_COMMAND, IDCANCEL, 0);
            return 0;
        } else if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Copy stream URL to clipboard
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                if (CopyToClipboard(hwnd, g_radioStations[sel].url)) {
                    Speak(Ts("URL copied"));
                }
            }
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        // Capture Enter/Escape/Delete/F2/Ctrl+Arrows, let Tab pass through
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && (pmsg->wParam == VK_RETURN || pmsg->wParam == VK_ESCAPE ||
                     pmsg->wParam == VK_DELETE || pmsg->wParam == VK_F2)) {
            return DLGC_WANTMESSAGE;
        }
        if (pmsg && (pmsg->wParam == VK_UP || pmsg->wParam == VK_DOWN) &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origRadioListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origRadioListProc, hwnd, msg, wParam, lParam);
}

// v2.25 — context menu on a radio list (Application key / Shift+F10 / right click).
// One item today ("Copy stream link"); built at runtime so future radio actions are
// trivial to add. isFavorites picks the backing vector and the URL-acquisition path.
// (x,y) is screen-space, anchored by the caller. Mirrors the YouTube context menu.
static void ShowRadioContextMenu(HWND hwnd, HWND hList, bool isFavorites, int x, int y) {
    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
    size_t count = isFavorites ? g_radioStations.size() : g_radioSearchResults.size();
    bool haveRow = (sel >= 0 && static_cast<size_t>(sel) < count);
    UINT f = haveRow ? MF_STRING : (MF_STRING | MF_GRAYED);

    HMENU root = CreatePopupMenu();
    AppendMenuW(root, f, IDM_RADIO_CTX_COPY_LINK, T("&Copy stream link"));
    // FUTURE radio actions (Play, Edit, Remove, Add to favorites...) go on `root` here.

    SetForegroundWindow(hwnd);  // MSDN TrackPopupMenu idiom (dismiss on focus loss)
    int cmd = static_cast<int>(TrackPopupMenu(
        root, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        x, y, 0, hwnd, nullptr));
    DestroyMenu(root);

    if (cmd == IDM_RADIO_CTX_COPY_LINK && haveRow) {
        if (isFavorites) {
            const std::wstring& u = g_radioStations[sel].url;
            if (u.empty()) { Speak(Ts("No stream link")); return; }
            if (CopyToClipboard(hwnd, u)) Speak(Ts("Link copied"));
        } else {
            const auto& r = g_radioSearchResults[sel];
            bool hadOptions = false;
            std::wstring u = ResolveAndPickStreamUrl(hwnd, r, hadOptions);
            if (!u.empty()) {
                if (CopyToClipboard(hwnd, u)) Speak(Ts("Link copied"));
            } else if (!hadOptions) {
                Speak(Ts("Could not get stream URL"));
            }
            // hadOptions && empty => user cancelled the stream picker => stay silent
        }
    }
}

// Radio dialog proc
static INT_PTR CALLBACK RadioDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(T("Favorites"));
            SendMessageW(hTab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tie));
            tie.pszText = const_cast<LPWSTR>(T("Search"));
            SendMessageW(hTab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&tie));

            // Subclass the favorites listbox for keyboard handling
            HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
            g_origRadioListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioListSubclassProc)));

            // Subclass the search listbox for keyboard handling
            HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
            g_origRadioSearchListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hSearchList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioSearchListSubclassProc)));

            // Subclass the search edit for Enter key handling
            HWND hSearchEdit = GetDlgItem(hwnd, IDC_RADIO_SEARCH_EDIT);
            g_origRadioSearchEditProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hSearchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RadioSearchEditSubclassProc)));

            // Initialize search source combo. RadioBrowser is the open-source
            // radio directory at radio-browser.info; in French we localize the
            // label to "Navigateur Radio" since users found "RadioBrowser"
            // opaque. TuneIn and iHeartRadio are brand names — left untouched.
            HWND hSource = GetDlgItem(hwnd, IDC_RADIO_SEARCH_SOURCE);
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T("RadioBrowser")));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"TuneIn"));
            SendMessageW(hSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"iHeartRadio"));
            SendMessageW(hSource, CB_SETCURSEL, 0, 0);

            // Initialize country combo with (Any); populate from cache or start async fetch
            HWND hCountry = GetDlgItem(hwnd, IDC_RADIO_SEARCH_COUNTRY);
            SendMessageW(hCountry, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(Any)"));
            SendMessageW(hCountry, CB_SETCURSEL, 0, 0);
            if (g_radioCountriesFetched) {
                PopulateCountryCombo(hwnd);
            } else {
                EnsureRadioCountriesFetched(hwnd);
            }

            // Initialize the advanced-filters combos. Bitrate and Sort are
            // static; Genre and Language are lazily fetched in the
            // background (population happens when WM_RADIO_*_READY fires).
            PopulateBitrateCombo(hwnd);
            PopulateSortCombo(hwnd);
            HWND hGenre = GetDlgItem(hwnd, IDC_RADIO_SEARCH_GENRE);
            SendMessageW(hGenre, CB_ADDSTRING, 0, (LPARAM)T("(Any)"));
            SendMessageW(hGenre, CB_SETCURSEL, 0, 0);
            HWND hLang = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LANGUAGE);
            SendMessageW(hLang, CB_ADDSTRING, 0, (LPARAM)T("(Any)"));
            SendMessageW(hLang, CB_SETCURSEL, 0, 0);
            if (g_radioTagsFetched)      PopulateGenreCombo(hwnd);
            else                          EnsureRadioTagsFetched(hwnd);
            if (g_radioLanguagesFetched) PopulateLanguageCombo(hwnd);
            else                          EnsureRadioLanguagesFetched(hwnd);

            // Restore "advanced filters" toggle state from INI.
            g_radioAdvancedOpen = GetPrivateProfileIntW(
                L"Radio", L"AdvancedSearchOpen", 0, g_configPath.c_str()) != 0;
            CheckDlgButton(hwnd, IDC_RADIO_SEARCH_ADVANCED,
                           g_radioAdvancedOpen ? BST_CHECKED : BST_UNCHECKED);

            // Load favorites
            RefreshRadioList(hwnd);

            // Show favorites tab, hide search tab
            UpdateRadioTabVisibility(hwnd, 0);

            // Focus on list
            SetFocus(hList);
            if (SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0) {
                SendMessageW(hList, LB_SETCURSEL, 0, 0);
            }
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_RADIO_SEARCH_SOURCE:
                    if (HIWORD(wParam) == CBN_SELCHANGE || HIWORD(wParam) == CBN_CLOSEUP) {
                        HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
                        int curTab = static_cast<int>(SendMessageW(hTab, TCM_GETCURSEL, 0, 0));
                        UpdateRadioTabVisibility(hwnd, curTab);
                        return TRUE;
                    }
                    break;

                case IDC_RADIO_SEARCH_ADVANCED:
                    if (HIWORD(wParam) == BN_CLICKED) {
                        // Flip the global, persist, re-evaluate which
                        // controls are visible. Speak the new state so a
                        // screen-reader user gets immediate feedback.
                        g_radioAdvancedOpen =
                            (IsDlgButtonChecked(hwnd, IDC_RADIO_SEARCH_ADVANCED) == BST_CHECKED);
                        WritePrivateProfileStringW(L"Radio", L"AdvancedSearchOpen",
                            g_radioAdvancedOpen ? L"1" : L"0", g_configPath.c_str());
                        HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
                        int curTab = (int)SendMessageW(hTab, TCM_GETCURSEL, 0, 0);
                        UpdateRadioTabVisibility(hwnd, curTab);
                        Speak(g_radioAdvancedOpen
                              ? Ts("Advanced filters shown")
                              : Ts("Advanced filters hidden"));
                        return TRUE;
                    }
                    break;

                case IDC_RADIO_ADD: {
                    // Check if currently playing a URL stream
                    const wchar_t* currentUrl = nullptr;
                    if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
                        const std::wstring& path = g_playlist[g_currentTrack];
                        if (IsURL(path.c_str())) {
                            currentUrl = path.c_str();
                        }
                    }
                    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO_ADD),
                                        hwnd, RadioAddDlgProc, reinterpret_cast<LPARAM>(currentUrl)) == IDOK) {
                        RefreshRadioList(hwnd);
                        Speak(Ts("Station added"));
                    }
                    return TRUE;
                }

                case IDC_RADIO_IMPORT: {
                    // Open file dialog for playlist files
                    wchar_t szFile[32768] = {0};
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
                    ofn.lpstrFilter = L"Playlist Files\0*.m3u;*.m3u8;*.pls\0"
                                      L"M3U Playlists\0*.m3u;*.m3u8\0"
                                      L"PLS Playlists\0*.pls\0"
                                      L"All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

                    if (GetOpenFileNameW(&ofn)) {
                        std::wstring playlistPath = szFile;
                        int imported = 0;

                        // Determine file type and parse
                        std::wstring ext = playlistPath.substr(playlistPath.find_last_of(L'.'));
                        for (auto& c : ext) c = towlower(c);

                        if (ext == L".pls") {
                            // Parse PLS - has Title entries
                            for (int i = 1; i <= 1000; i++) {
                                wchar_t fileKey[32], titleKey[32];
                                swprintf(fileKey, 32, L"File%d", i);
                                swprintf(titleKey, 32, L"Title%d", i);

                                wchar_t url[4096] = {0}, title[512] = {0};
                                GetPrivateProfileStringW(L"playlist", fileKey, L"", url, 4096, playlistPath.c_str());
                                if (url[0] == L'\0') break;

                                // Only import URLs (not local files)
                                if (_wcsnicmp(url, L"http://", 7) != 0 && _wcsnicmp(url, L"https://", 8) != 0) {
                                    continue;
                                }

                                GetPrivateProfileStringW(L"playlist", titleKey, L"", title, 512, playlistPath.c_str());
                                std::wstring name = title[0] ? title : url;

                                if (AddRadioStation(name, url) >= 0) {
                                    imported++;
                                }
                            }
                        } else {
                            // Parse M3U/M3U8
                            FILE* f = _wfopen(playlistPath.c_str(), L"rb");
                            if (f) {
                                // Check for UTF-8 BOM
                                unsigned char bom[3] = {0};
                                fread(bom, 1, 3, f);
                                if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
                                    fseek(f, 0, SEEK_SET);
                                }

                                char line[4096];
                                std::wstring pendingName;

                                while (fgets(line, sizeof(line), f)) {
                                    // Trim whitespace
                                    char* start = line;
                                    while (*start && (*start == ' ' || *start == '\t')) start++;
                                    size_t slen = strlen(start);
                                    if (slen == 0) continue;
                                    char* end = start + slen - 1;
                                    while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
                                        *end-- = '\0';
                                    }
                                    if (*start == '\0') continue;

                                    // Convert to wide string
                                    std::wstring wline;
                                    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, nullptr, 0);
                                    if (len > 0) {
                                        wline.resize(len);
                                        MultiByteToWideChar(CP_UTF8, 0, start, -1, &wline[0], len);
                                    } else {
                                        len = MultiByteToWideChar(CP_ACP, 0, start, -1, nullptr, 0);
                                        if (len <= 0) continue;
                                        wline.resize(len);
                                        MultiByteToWideChar(CP_ACP, 0, start, -1, &wline[0], len);
                                    }
                                    if (!wline.empty() && wline.back() == L'\0') wline.pop_back();
                                    if (wline.empty()) continue;

                                    // Check for #EXTINF line (contains station name)
                                    if (_wcsnicmp(wline.c_str(), L"#EXTINF:", 8) == 0) {
                                        // Format: #EXTINF:duration,Station Name
                                        const wchar_t* comma = wcschr(wline.c_str() + 8, L',');
                                        if (comma) {
                                            // Skip comma and any leading whitespace
                                            const wchar_t* nameStart = comma + 1;
                                            while (*nameStart == L' ' || *nameStart == L'\t') nameStart++;
                                            pendingName = nameStart;
                                        }
                                        continue;
                                    }

                                    // Skip other comments
                                    if (wline[0] == L'#') continue;

                                    // This should be a URL
                                    if (_wcsnicmp(wline.c_str(), L"http://", 7) == 0 ||
                                        _wcsnicmp(wline.c_str(), L"https://", 8) == 0) {
                                        std::wstring name = pendingName.empty() ? wline : pendingName;
                                        if (AddRadioStation(name, wline) >= 0) {
                                            imported++;
                                        }
                                    }
                                    pendingName.clear();
                                }
                                fclose(f);
                            }
                        }

                        if (imported > 0) {
                            RefreshRadioList(hwnd);
                            wchar_t msg[128];
                            swprintf(msg, 128, T("Imported %d stations"), imported);
                            Speak(WideToUtf8(msg).c_str());
                        } else {
                            Speak(Ts("No stations found to import"));
                        }
                    }
                    return TRUE;
                }

                case IDC_RADIO_EXPORT: {
                    // Export favorites to M3U file
                    if (g_radioStations.empty()) {
                        Speak(Ts("No favorites to export"));
                        return TRUE;
                    }

                    wchar_t szFile[MAX_PATH] = L"radio_favorites.m3u";
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrFilter = L"M3U Playlist\0*.m3u\0All Files (*.*)\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.lpstrDefExt = L"m3u";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

                    if (GetSaveFileNameW(&ofn)) {
                        FILE* f = _wfopen(szFile, L"wb");
                        if (f) {
                            // Write UTF-8 BOM
                            fwrite("\xEF\xBB\xBF", 1, 3, f);
                            // Write M3U header
                            fprintf(f, "#EXTM3U\r\n");

                            for (const auto& station : g_radioStations) {
                                // Write EXTINF with station name
                                std::string name = WideToUtf8(station.name);
                                std::string url = WideToUtf8(station.url);
                                fprintf(f, "#EXTINF:-1,%s\r\n", name.c_str());
                                fprintf(f, "%s\r\n", url.c_str());
                            }
                            fclose(f);

                            wchar_t msg[128];
                            swprintf(msg, 128, T("Exported %d stations"), static_cast<int>(g_radioStations.size()));
                            Speak(WideToUtf8(msg).c_str());
                        } else {
                            Speak(Ts("Failed to write file"));
                        }
                    }
                    return TRUE;
                }

                case IDC_RADIO_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to play
                        HWND hList = GetDlgItem(hwnd, IDC_RADIO_LIST);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_radioStations.size())) {
                            // Preset favorite name before PlayTrack so the title bar
                // reads "Station - ..." immediately.
                            SetNowPlaying(SourceType::RadioFavorite,
                                          g_radioStations[sel].name, L"");
                            g_playlist.clear();
                            g_playlist.push_back(g_radioStations[sel].url);
                            PlayTrack(0);
                        }
                    }
                    return TRUE;

                case IDC_RADIO_SEARCH_BTN: {
                    // Ensure country controls match the current source before we proceed
                    {
                        HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
                        int curTab = static_cast<int>(SendMessageW(hTab, TCM_GETCURSEL, 0, 0));
                        UpdateRadioTabVisibility(hwnd, curTab);
                    }

                    // Get search query
                    wchar_t query[256] = {0};
                    GetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_EDIT, query, 256);

                    // Read country filter (only applies to RadioBrowser)
                    int source = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_SOURCE, CB_GETCURSEL, 0, 0));
                    std::wstring countryFilter;
                    if (source == 0) {
                        wchar_t countryBuf[256] = {0};
                        GetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_COUNTRY, countryBuf, 256);
                        countryFilter = countryBuf;
                        if (countryFilter == L"(Any)") countryFilter.clear();
                    }

                    // Read the advanced filters (only when the section is
                    // open AND RadioBrowser is selected; empty strings /
                    // 0 / -1 disable each filter).
                    std::wstring tagFilter, languageFilter;
                    int bitrateMin = 0;
                    int sortKey = -1;
                    if (source == 0 && g_radioAdvancedOpen) {
                        wchar_t buf[256] = {0};
                        GetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_GENRE, buf, 256);
                        tagFilter = buf;
                        if (tagFilter == T("(Any)")) tagFilter.clear();

                        buf[0] = 0;
                        GetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_LANGUAGE, buf, 256);
                        languageFilter = buf;
                        if (languageFilter == T("(Any)")) languageFilter.clear();

                        int bIdx = (int)SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_BITRATE, CB_GETCURSEL, 0, 0);
                        if (bIdx > 0) {
                            bitrateMin = (int)SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_BITRATE, CB_GETITEMDATA, bIdx, 0);
                        }
                        int sIdx = (int)SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_SORT, CB_GETCURSEL, 0, 0);
                        if (sIdx >= 0) {
                            sortKey = (int)SendDlgItemMessageW(hwnd, IDC_RADIO_SEARCH_SORT, CB_GETITEMDATA, sIdx, 0);
                        }
                    }

                    // Allow empty query when ANY RadioBrowser filter is set
                    // (country, tag, language). Bitrate / sort alone don't
                    // count — they refine but don't define the query.
                    bool anyFilter = !countryFilter.empty() || !tagFilter.empty() ||
                                     !languageFilter.empty();
                    if (wcslen(query) == 0 && !anyFilter) {
                        Speak(Ts("Enter a search term"));
                        return TRUE;
                    }

                    // Clear results
                    HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                    SendMessageW(hSearchList, LB_RESETCONTENT, 0, 0);
                    g_radioSearchResults.clear();
                    SetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_RESULT_COUNT, T("Searching..."));

                    // Show searching message
                    Speak(Ts("Searching"));
                    SetCursor(LoadCursor(nullptr, IDC_WAIT));

                    bool found = false;

                    if (source == 0) {  // RadioBrowser
                        found = SearchRadioBrowser(query, countryFilter,
                                                   tagFilter, languageFilter,
                                                   bitrateMin, sortKey,
                                                   g_radioSearchResults);
                    } else if (source == 1) {  // TuneIn
                        found = SearchTuneIn(query, g_radioSearchResults);
                    } else if (source == 2) {  // iHeartRadio
                        found = SearchIHeartRadio(query, g_radioSearchResults);
                    }

                    SetCursor(LoadCursor(nullptr, IDC_ARROW));

                    if (found) {
                        // Populate list
                        for (const auto& r : g_radioSearchResults) {
                            std::wstring display = r.name;
                            if (!r.country.empty()) {
                                display += L" (" + r.country + L")";
                            }
                            if (r.bitrate > 0) {
                                wchar_t br[32];
                                swprintf(br, 32, L" [%dk]", r.bitrate);
                                display += br;
                            }
                            SendMessageW(hSearchList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
                        }

                        wchar_t msg[128];
                        swprintf(msg, 128, T("Found %d stations"), static_cast<int>(g_radioSearchResults.size()));
                        Speak(WideToUtf8(msg).c_str());
                        // Mirror the count into the static so a sighted
                        // user sees it too, and so the screen reader can
                        // read it back via standard navigation.
                        SetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_RESULT_COUNT, msg);

                        // Select first item
                        if (SendMessageW(hSearchList, LB_GETCOUNT, 0, 0) > 0) {
                            SendMessageW(hSearchList, LB_SETCURSEL, 0, 0);
                            SetFocus(hSearchList);
                        }
                    } else {
                        Speak(Ts("No stations found"));
                        SetDlgItemTextW(hwnd, IDC_RADIO_SEARCH_RESULT_COUNT,
                                        T("No stations found"));
                    }
                    return TRUE;
                }

                case IDC_RADIO_SEARCH_ADD: {
                    // Add selected search result to favorites
                    HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                    int sel = static_cast<int>(SendMessageW(hSearchList, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                        const auto& r = g_radioSearchResults[sel];
                        bool hadOptions = false;
                        std::wstring streamUrl = ResolveAndPickStreamUrl(hwnd, r, hadOptions);

                        if (!streamUrl.empty()) {
                            if (AddRadioStation(r.name, streamUrl) >= 0) {
                                RefreshRadioList(hwnd);
                                Speak(Ts("Added to favorites"));
                            } else {
                                Speak(Ts("Failed to add station"));
                            }
                        } else if (!hadOptions) {
                            Speak(Ts("Could not get stream URL"));
                        }
                        // If hadOptions but streamUrl is empty, user cancelled - do nothing
                    } else {
                        Speak(Ts("Select a station first"));
                    }
                    return TRUE;
                }

                case IDC_RADIO_SEARCH_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click to play
                        HWND hSearchList = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
                        int sel = static_cast<int>(SendMessageW(hSearchList, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_radioSearchResults.size())) {
                            const auto& r = g_radioSearchResults[sel];
                            bool hadOptions = false;
                            std::wstring streamUrl = ResolveAndPickStreamUrl(hwnd, r, hadOptions);

                            if (!streamUrl.empty()) {
                                // Preset the station name from the search
                                // result before PlayTrack so the window
                                // title is correct from the first frame.
                                SetNowPlaying(SourceType::RadioFavorite,
                                              r.name, L"");
                                g_playlist.clear();
                                g_playlist.push_back(streamUrl);
                                PlayTrack(0);
                            } else if (!hadOptions) {
                                Speak(Ts("Could not get stream URL"));
                            }
                            // If hadOptions but streamUrl is empty, user cancelled - do nothing
                        }
                    }
                    return TRUE;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_NOTIFY: {
            NMHDR* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->idFrom == IDC_RADIO_TAB && nmhdr->code == TCN_SELCHANGE) {
                HWND hTab = GetDlgItem(hwnd, IDC_RADIO_TAB);
                int tab = static_cast<int>(SendMessageW(hTab, TCM_GETCURSEL, 0, 0));
                UpdateRadioTabVisibility(hwnd, tab);
                // Don't change focus - let user navigate naturally
            }
            break;
        }

        default:
            if (msg == WM_RADIO_COUNTRIES_READY) {
                PopulateCountryCombo(hwnd);
                return TRUE;
            }
            if (msg == WM_RADIO_TAGS_READY) {
                PopulateGenreCombo(hwnd);
                return TRUE;
            }
            if (msg == WM_RADIO_LANGUAGES_READY) {
                PopulateLanguageCombo(hwnd);
                return TRUE;
            }
            break;

        case WM_CONTEXTMENU: {
            // v2.25 — context menu on either radio list (Application key / Shift+F10
            // / right-click). The subclassed listboxes only intercept WM_KEYDOWN /
            // WM_GETDLGCODE, so their DefWindowProc still forwards/synthesizes
            // WM_CONTEXTMENU to this dialog for all three triggers (same as YouTube).
            HWND src     = reinterpret_cast<HWND>(wParam);
            HWND hFav    = GetDlgItem(hwnd, IDC_RADIO_LIST);
            HWND hSearch = GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST);
            bool isFav;
            if      (src == hFav)    isFav = true;
            else if (src == hSearch) isFav = false;
            else break;                              // only the two radio lists
            HWND hList = src;
            int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (lParam == static_cast<LPARAM>(-1)) {  // keyboard: VK_APPS / Shift+F10
                int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                if (sel < 0 && SendMessageW(hList, LB_GETCOUNT, 0, 0) > 0) {
                    SendMessageW(hList, LB_SETCURSEL, 0, 0); sel = 0;
                }
                RECT rc;
                if (sel >= 0 &&
                    SendMessageW(hList, LB_GETITEMRECT, sel, reinterpret_cast<LPARAM>(&rc)) != LB_ERR) {
                    POINT pt = { rc.left, rc.bottom }; ClientToScreen(hList, &pt); x = pt.x; y = pt.y;
                } else {
                    RECT wr; GetWindowRect(hList, &wr); x = wr.left; y = wr.top;
                }
            } else {                                   // mouse: select row under cursor
                POINT pt = { x, y }; ScreenToClient(hList, &pt);
                DWORD idx = static_cast<DWORD>(SendMessageW(hList, LB_ITEMFROMPOINT, 0,
                                                  MAKELPARAM(pt.x, pt.y)));
                if (HIWORD(idx) == 0) SendMessageW(hList, LB_SETCURSEL, LOWORD(idx), 0);
            }
            ShowRadioContextMenu(hwnd, hList, isFav, x, y);
            return TRUE;
        }

        case WM_SIZE: {
            // Resize list and reposition buttons
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Tab control
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_TAB), nullptr, 7, 7, w - 14, h - 42, SWP_NOZORDER);

            // Favorites tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_LIST), nullptr, 14, 28, w - 28, h - 92, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_ADD), nullptr, w - 228, h - 54, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_IMPORT), nullptr, w - 174, h - 54, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_EXPORT), nullptr, w - 120, h - 54, 50, 14, SWP_NOZORDER);

            // Search tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_EDIT), nullptr, 142, 28, w - 210, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_BTN), nullptr, w - 64, 27, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_LIST), nullptr, 14, 74, w - 28, h - 138, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_RADIO_SEARCH_ADD), nullptr, w - 84, h - 54, 70, 14, SWP_NOZORDER);

            // Close button (common)
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, w - 64, h - 22, 50, 14, SWP_NOZORDER);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }
    }
    return FALSE;
}

// Show radio dialog
void ShowRadioDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO), g_hwnd, RadioDlgProc);
}

// Add the currently playing stream URL to radio favorites
void AddCurrentStreamToFavorites() {
    if (g_currentTrack < 0 || g_currentTrack >= static_cast<int>(g_playlist.size())) {
        Speak(Ts("No stream playing"));
        return;
    }
    const std::wstring& path = g_playlist[g_currentTrack];
    if (!IsURL(path.c_str())) {
        Speak(Ts("Not a stream"));
        return;
    }

    std::vector<RadioStation> existing = GetRadioFavorites();
    for (const auto& s : existing) {
        if (_wcsicmp(s.url.c_str(), path.c_str()) == 0) {
            Speak(Ts("Stream already in favorites"));
            return;
        }
    }

    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_RADIO_ADD),
                        g_hwnd, RadioAddDlgProc, reinterpret_cast<LPARAM>(path.c_str())) == IDOK) {
        Speak(Ts("Station added"));
    }
}
