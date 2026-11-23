// basscd_loader.cpp — see basscd_loader.h.

#include "basscd_loader.h"
#include "logger.h"
#include <mutex>

namespace mediaaccess {
namespace basscd {

namespace {
std::once_flag g_once;
Api            g_api = {};
bool           g_ok = false;
std::wstring   g_error;

template <typename T>
bool Bind(HMODULE dll, T& slot, const char* name) {
    slot = reinterpret_cast<T>(GetProcAddress(dll, name));
    if (!slot) {
        g_error = L"basscd.dll: function not found: ";
        for (const char* p = name; *p; ++p) g_error += static_cast<wchar_t>(*p);
        return false;
    }
    return true;
}

void LoadOnce() {
    wchar_t exe[32768];
    DWORD got = GetModuleFileNameW(nullptr, exe, 32768);
    if (got == 0 || got >= 32768) {
        g_error = L"GetModuleFileNameW failed";
        return;
    }
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) {
        g_error = L"Unexpected executable path";
        return;
    }
    *(slash + 1) = L'\0';
    std::wstring full = std::wstring(exe) + L"lib\\basscd.dll";

    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_error = L"File not found: " + full;
        LogF("CD", "basscd.dll not found at %ls", full.c_str());
        return;
    }
    HMODULE dll = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        DWORD err = GetLastError();
        g_error = L"Cannot load " + full + L" (error " + std::to_wstring(err) + L")";
        LogF("CD", "LoadLibraryExW(basscd.dll) failed: 0x%08lX", err);
        return;
    }
    g_ok = Bind(dll, g_api.GetInfo,        "BASS_CD_GetInfo")
        && Bind(dll, g_api.Door,           "BASS_CD_Door")
        && Bind(dll, g_api.DoorIsOpen,     "BASS_CD_DoorIsOpen")
        && Bind(dll, g_api.IsReady,        "BASS_CD_IsReady")
        && Bind(dll, g_api.GetTracks,      "BASS_CD_GetTracks")
        && Bind(dll, g_api.GetTrackLength, "BASS_CD_GetTrackLength")
        && Bind(dll, g_api.GetTOC,         "BASS_CD_GetTOC")
        && Bind(dll, g_api.GetID,          "BASS_CD_GetID")
        && Bind(dll, g_api.Release,        "BASS_CD_Release");
    if (g_ok) {
        LogF("CD", "basscd.dll loaded");
    } else {
        LogF("CD", "basscd.dll incomplete: %ls", g_error.c_str());
    }
}
} // namespace

const Api* Get() {
    std::call_once(g_once, LoadOnce);
    return g_ok ? &g_api : nullptr;
}

std::wstring LastError() {
    std::call_once(g_once, LoadOnce);
    return g_error;
}

} // namespace basscd
} // namespace mediaaccess
