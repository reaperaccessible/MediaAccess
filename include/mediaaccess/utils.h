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

// Format time as M:SS or H:MM:SS (for on-screen display)
std::wstring FormatTime(double seconds);

// Like FormatTime, but spelled out in localized units ("2 minutes 5 seconds")
// for SPEECH only. Screen readers (NVDA/JAWS) misread the "0:11" colon form as
// a clock time ("0 hours 11"); the unit form is unambiguous. Use this for every
// spoken time announcement; keep FormatTime for on-screen display.
std::wstring FormatTimeSpoken(double seconds);

#endif // MEDIAACCESS_UTILS_H
