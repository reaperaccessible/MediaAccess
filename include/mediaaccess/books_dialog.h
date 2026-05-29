#pragma once
#ifndef MEDIAACCESS_BOOKS_DIALOG_H
#define MEDIAACCESS_BOOKS_DIALOG_H

#include <windows.h>
#include <string>

namespace mediaaccess {

// Show the Book library window (Tools > Book library, or F4-equivalent).
void ShowBookLibrary(HWND owner);

// File > Open book... — prompts for a path (.opf / .epub / folder), upserts
// into the library and starts playback. Returns true on success.
bool OpenBookFromDialog(HWND owner);

// Open a specific book path (called from library and from File > Open book).
// Parses, registers in DB, loads into DaisyPlayer, starts playback.
bool OpenBookByPath(HWND owner, const std::wstring& path);

// Walks all registered library folders and upserts any DAISY/EPUB books it
// finds. Safe to call from main thread; typical sizes finish in <1 s. Folders
// that have moved or been deleted are skipped silently.
int RescanBookLibrary();

// Prompt for an optional bookmark note then add the bookmark.
// No-op if no book is currently loaded.
void PromptAddBookmark(HWND owner);

// Prompt for a page number then jump there.
// No-op if no book is currently loaded.
void PromptGoToPage(HWND owner);

// F3 search dialog — prompts for text, finds matches in the current book,
// jumps to the first one. Calling again with state still cached jumps to
// the next hit. No-op if no book is currently loaded.
void PromptSearchInBook(HWND owner);

// "Find next" without a dialog — repeats the previous search if any.
// Useful for F3 repeated press: opens the dialog on first F3, jumps to
// next match on subsequent F3 presses.
void FindNextInBook();

} // namespace mediaaccess

#endif // MEDIAACCESS_BOOKS_DIALOG_H
