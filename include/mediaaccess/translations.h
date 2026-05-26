#pragma once
#ifndef MEDIAACCESS_TRANSLATIONS_H
#define MEDIAACCESS_TRANSLATIONS_H

#include <windows.h>
#include <string>

// Initialize the translation system and register all built-in translations.
// MUST be called before LoadSettings() so SetLanguage() exists when the
// saved language is read from the INI file.
void InitTranslations();

// Set the active language. langCode is "en" or "fr".
void SetLanguage(const char* langCode);

// Returns the active language code ("en" or "fr").
const char* GetCurrentLanguage();

// Translate a key into the active language. The key is the English source
// text (UTF-8). Returns a pointer with the lifetime of the registered
// translation map, except in the fallback case where the key is converted
// to wide chars in a thread-local buffer.
const wchar_t* T(const char* key);

// Auto-detect language from Windows UI language. Returns "en" or "fr".
const char* DetectSystemLanguage();

// Walk every child control of hDlg and replace its text with the localized
// version (the existing English text is used as the lookup key). Also
// localizes the dialog caption. Safe to call on already-localized dialogs.
void LocalizeDialog(HWND hDlg);

// Walk every item of hMenu (recursing into submenus) and replace text via T().
void LocalizeMenu(HMENU hMenu);

// Register a translation pair. lang is "en" or "fr"; key is UTF-8.
void AddTranslation(const char* lang, const char* key, const wchar_t* text);

// Helper for Speak() - returns UTF-8 std::string for the current language.
std::string Ts(const char* key);

#endif // MEDIAACCESS_TRANSLATIONS_H
