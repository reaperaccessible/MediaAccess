#pragma once
#ifndef MEDIAACCESS_BOOK_TEXT_WINDOW_H
#define MEDIAACCESS_BOOK_TEXT_WINDOW_H

// =============================================================================
// book_text_window.h — modeless window that shows the current paragraph of
// the playing DAISY/EPUB book.
//
// Used by both the audio+text path (sentence/paragraph displayed in sync
// with the playing MP3 clip) and the text-only path (current paragraph
// being read by SAPI).
//
// Three themes: Standard (black on white), HighContrast (yellow on black),
// Large (black on white but big font). User picks in Preferences > Books.
// =============================================================================

#include <windows.h>
#include <string>

namespace mediaaccess {

enum class BookTextTheme {
    Standard     = 0,
    HighContrast = 1,
    Large        = 2,
};

// Show the text window (creating it on first call). owner = main window.
// If the user has "Always hide text window" set in Preferences > Books,
// the window is created but kept hidden — the user can still trigger it
// later with Ctrl+T.
void BookTextWindowShow(HWND owner);

// Hide the text window without destroying it (state is preserved).
void BookTextWindowHide();

// Toggle visibility. Creates the window if not yet created. This is the
// handler for the Ctrl+T action.
void BookTextWindowToggle(HWND owner);

// Replace the displayed text. Typically called when the playing clip
// changes (audio+text) or when TTS advances to the next paragraph
// (text-only). Empty string clears the display.
void BookTextWindowSetText(const std::wstring& text);

// Set the active theme. Persists to INI under [Books] TextTheme.
// Re-paints the window if visible.
void BookTextWindowSetTheme(BookTextTheme theme);
BookTextTheme BookTextWindowGetTheme();

// Tear down the window (called from app shutdown). Safe if not created.
void BookTextWindowDestroy();

// Returns true if the user has the "always hide" preference set
// (read from [Books] HideTextWindow in MediaAccess.ini).
bool BookTextWindowGetAlwaysHide();
void BookTextWindowSetAlwaysHide(bool hide);

} // namespace mediaaccess

#endif // MEDIAACCESS_BOOK_TEXT_WINDOW_H
