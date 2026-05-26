#pragma once
#ifndef MEDIAACCESS_LOGGER_H
#define MEDIAACCESS_LOGGER_H

#include <string>

void InitLogger();
void FreeLogger();

// Thread-safe append. Each line gets "[YYYY-MM-DD HH:MM:SS] [TAG] message\n"
void Log(const char* tag, const std::string& msg);
void Log(const char* tag, const std::wstring& msg);

// printf-style convenience
void LogF(const char* tag, const char* fmt, ...);

#endif
