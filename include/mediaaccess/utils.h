#pragma once
#ifndef MEDIAACCESS_UTILS_H
#define MEDIAACCESS_UTILS_H

#include <string>

// String conversion
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);

// Extract filename from path
std::wstring GetFileName(const std::wstring& path);

// Extract filename without its extension (for display)
std::wstring GetFileNameNoExt(const std::wstring& path);

// Format time as M:SS or H:MM:SS
std::wstring FormatTime(double seconds);

#endif // MEDIAACCESS_UTILS_H
