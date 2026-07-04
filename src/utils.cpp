#include "utils.h"
#include "translations.h"   // T() for localized unit words (FormatTimeSpoken)
#include <windows.h>
#include <cstdio>

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string utf8(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring wide(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], size);
    return wide;
}

std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::wstring GetFileNameNoExt(const std::wstring& path) {
    std::wstring name = GetFileName(path);            // basename, no directory
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) {      // keep dotfiles (".cue") and no-dot names intact
        return name.substr(0, dot);
    }
    return name;
}

std::wstring FormatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalSec = static_cast<int>(seconds);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;

    wchar_t buf[32];
    if (h > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf(buf, 32, L"%d:%02d", m, s);
    }
    return buf;
}

// Spoken form: unit words, localized, leading zero components dropped.
//   0:11    -> "11 seconds"          / "11 secondes"
//   2:05    -> "2 minutes 5 seconds" / "2 minutes 5 secondes"
//   1:00    -> "1 minute"            / "1 minute"
//   1:02:03 -> "1 hour 2 minutes 3 seconds"
//   0:00    -> "0 seconds"           / "0 seconde"
std::wstring FormatTimeSpoken(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalSec = static_cast<int>(seconds);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;

    std::wstring out;
    wchar_t buf[64];
    auto add = [&](int value, const char* singularKey, const char* pluralKey) {
        if (!out.empty()) out += L" ";
        swprintf(buf, 64, L"%d %s", value, T(value == 1 ? singularKey : pluralKey));
        out += buf;
    };

    if (h > 0) add(h, "hour", "hours");
    if (m > 0) add(m, "minute", "minutes");
    if (s > 0) add(s, "second", "seconds");
    if (out.empty()) add(0, "second", "seconds");  // "0 seconde"
    return out;
}
