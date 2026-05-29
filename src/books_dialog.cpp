// =============================================================================
// books_dialog.cpp — Book library window + Open Book picker + bookmark/page prompts
//
// Three small modal dialogs plus the file-open flow. The actual playback logic
// lives in daisy_player.cpp; this file is glue between Win32 UI and that.
// =============================================================================

#include "mediaaccess/books_dialog.h"
#include "mediaaccess/daisy_book.h"
#include "mediaaccess/daisy_player.h"
#include "mediaaccess/book_text_window.h"
#include "mediaaccess/database.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/accessibility.h"
#include "mediaaccess/logger.h"
#include "resource.h"

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace mediaaccess {

// -----------------------------------------------------------------------------
// Small UTF helpers (duplicated minimally to avoid extra header churn)
// -----------------------------------------------------------------------------
static std::wstring U8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// -----------------------------------------------------------------------------
// Open Book flow
// -----------------------------------------------------------------------------

bool OpenBookByPath(HWND owner, const std::wstring& path) {
    std::string err;
    auto book = OpenDaisyBook(path, &err);
    if (!book) {
        std::wstring msg = std::wstring(T("Could not open book.")) +
                           L"\n" + path +
                           (err.empty() ? L"" : (L"\n\n" + U8(err)));
        MessageBoxW(owner, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONWARNING);
        return false;
    }

    // Book has no recorded audio AND no extracted text — nothing playable.
    if (book->clips.empty() && book->textSegments.empty()) {
        int id = UpsertBook(book->path, book->title, book->author,
                            L"epub3", book->totalDuration);
        (void)id;
        MessageBoxW(owner,
            T("This book has no recorded audio and no extractable text. "
              "It has been added to your library but cannot be played."),
            L"MediaAccess", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const wchar_t* fmt = L"daisy202";
    if (book->format == DaisyFormat::Daisy3) fmt = L"daisy3";
    else if (book->format == DaisyFormat::Epub3) fmt = L"epub3";

    int bookId = UpsertBook(book->path, book->title, book->author, fmt,
                            book->totalDuration);
    if (bookId <= 0) {
        MessageBoxW(owner, T("Could not register book in library."),
                    L"MediaAccess", MB_OK | MB_ICONWARNING);
        return false;
    }

    // Capture text-availability info before move-from.
    bool hasText = !book->textSegments.empty();
    if (!hasText) {
        for (const auto& c : book->clips) {
            if (!c.textContent.empty()) { hasText = true; break; }
        }
    }

    if (!DaisyLoadAndPlay(std::move(book), bookId)) {
        MessageBoxW(owner, T("Could not start book playback."),
                    L"MediaAccess", MB_OK | MB_ICONWARNING);
        return false;
    }
    Speak(Ts("Book opened"));

    // Auto-show text window for books that have text, unless the user opted
    // out via Preferences > Books > "Always hide text window".
    if (hasText && !BookTextWindowGetAlwaysHide()) {
        BookTextWindowShow(owner);
    }
    return true;
}

bool OpenBookFromDialog(HWND owner) {
    OPENFILENAMEW ofn{};
    wchar_t buf[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter =
        L"All book formats\0*.opf;*.ncx;NCC.html;*.epub\0"
        L"DAISY package (*.opf)\0*.opf\0"
        L"DAISY NCC.html\0NCC.html\0"
        L"EPUB (*.epub)\0*.epub\0"
        L"All files\0*.*\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return false;
    return OpenBookByPath(owner, buf);
}

// -----------------------------------------------------------------------------
// Library scanner — walks user-registered library folders
// -----------------------------------------------------------------------------

static void ScanFolderRecursive(const std::wstring& root, int& added) {
    // For each sub-folder, check whether it looks like a DAISY book. We
    // recurse one level deep into folders that aren't books themselves —
    // many publishers organize as Library/SeriesName/BookName/NCC.html.
    WIN32_FIND_DATAW fd;
    std::wstring pat = root + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Check whether THIS folder is a book.
            DaisyFormat f = DetectDaisyFormat(p);
            if (f == DaisyFormat::Daisy202 || f == DaisyFormat::Daisy3) {
                std::string err;
                auto book = OpenDaisyBook(p, &err);
                if (book) {
                    const wchar_t* fmt = (book->format == DaisyFormat::Daisy3) ? L"daisy3" : L"daisy202";
                    if (UpsertBook(book->path, book->title, book->author, fmt,
                                   book->totalDuration) > 0) {
                        ++added;
                    }
                }
            } else {
                // Recurse one level
                ScanFolderRecursive(p, added);
            }
        } else {
            // .epub file in the folder
            std::wstring lower = fd.cFileName;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, L".epub") == 0) {
                std::string err;
                auto book = OpenDaisyBook(p, &err);
                if (book) {
                    if (UpsertBook(book->path, book->title, book->author, L"epub3",
                                   book->totalDuration) > 0) {
                        ++added;
                    }
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int RescanBookLibrary() {
    int added = 0;
    auto folders = GetBookLibraryFolders();
    for (const auto& f : folders) {
        DWORD a = GetFileAttributesW(f.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            ScanFolderRecursive(f, added);
        }
    }
    return added;
}

// -----------------------------------------------------------------------------
// Book library dialog
// -----------------------------------------------------------------------------

struct LibraryDialogState {
    std::vector<BookEntry> books;
};
static LibraryDialogState* s_lib = nullptr;

static void FormatBookListLine(const BookEntry& b, std::wstring& out) {
    out = b.title.empty() ? b.path : b.title;
    if (!b.author.empty()) out += L" — " + b.author;
    if (b.positionOffset > 0 || b.positionClip > 0) {
        // Append "  (resumed)" hint
        out += L"  [";
        out += U8(Ts("in progress"));
        out += L"]";
    }
}

static void PopulateLibraryList(HWND dlg) {
    if (!s_lib) return;
    HWND list = GetDlgItem(dlg, IDC_BOOK_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    s_lib->books = GetAllBooks();
    for (const auto& b : s_lib->books) {
        std::wstring line;
        FormatBookListLine(b, line);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    }
    if (!s_lib->books.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK LibraryDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM /*lp*/) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            PopulateLibraryList(dlg);
            SetFocus(GetDlgItem(dlg, IDC_BOOK_LIST));
            return FALSE;

        case WM_COMMAND: {
            WORD id   = LOWORD(wp);
            WORD code = HIWORD(wp);
            switch (id) {
                case IDC_BOOK_OPEN_SELECTED:
                case IDOK: {
                    if (!s_lib) return TRUE;
                    int sel = (int)SendDlgItemMessageW(dlg, IDC_BOOK_LIST, LB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= (int)s_lib->books.size()) return TRUE;
                    std::wstring path = s_lib->books[sel].path;
                    EndDialog(dlg, IDOK);
                    OpenBookByPath(GetParent(dlg), path);
                    return TRUE;
                }
                case IDC_BOOK_REMOVE_SELECTED: {
                    if (!s_lib) return TRUE;
                    int sel = (int)SendDlgItemMessageW(dlg, IDC_BOOK_LIST, LB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= (int)s_lib->books.size()) return TRUE;
                    if (MessageBoxW(dlg,
                            T("Remove this book from the library? The file on disk is kept."),
                            T("Book library"), MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
                    RemoveBook(s_lib->books[sel].id);
                    PopulateLibraryList(dlg);
                    return TRUE;
                }
                case IDC_BOOK_RESCAN: {
                    int added = RescanBookLibrary();
                    wchar_t buf[128];
                    swprintf(buf, 128, T("Scan complete: %d book(s) added or updated."), added);
                    SpeakW(buf);
                    PopulateLibraryList(dlg);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(dlg, 0);
                    return TRUE;
            }
            if (id == IDC_BOOK_LIST && code == LBN_DBLCLK) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDC_BOOK_OPEN_SELECTED, 0), 0);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(dlg, 0);
            return TRUE;
    }
    return FALSE;
}

void ShowBookLibrary(HWND owner) {
    LibraryDialogState st;
    s_lib = &st;
    DialogBoxW(GetModuleHandleW(nullptr),
               MAKEINTRESOURCEW(IDD_BOOK_LIBRARY), owner, LibraryDlgProc);
    s_lib = nullptr;
}

// -----------------------------------------------------------------------------
// Bookmark with note
// -----------------------------------------------------------------------------

static std::wstring* s_noteOut = nullptr;

static INT_PTR CALLBACK BookmarkNoteDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM /*lp*/) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            SetFocus(GetDlgItem(dlg, IDC_BOOK_BOOKMARK_NOTE_EDIT));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[1024] = {0};
                GetDlgItemTextW(dlg, IDC_BOOK_BOOKMARK_NOTE_EDIT, buf, 1024);
                if (s_noteOut) *s_noteOut = buf;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) {
                EndDialog(dlg, IDCANCEL);
                return TRUE;
            }
            break;
        case WM_CLOSE:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
    }
    return FALSE;
}

void PromptAddBookmark(HWND owner) {
    if (!DaisyIsActive()) return;
    std::wstring note;
    s_noteOut = &note;
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDD_BOOK_BOOKMARK_NOTE),
                           owner, BookmarkNoteDlgProc);
    s_noteOut = nullptr;
    if (r != IDOK) return;
    DaisyAddBookmarkHere(note);
}

// -----------------------------------------------------------------------------
// Go to page
// -----------------------------------------------------------------------------

void PromptGoToPage(HWND owner) {
    if (!DaisyIsActive()) return;
    // Use a tiny inline edit-only dialog. Returns the typed text via the
    // standard DialogBoxW infrastructure — we use a static state pointer.
    static std::wstring s_pageBuf;
    s_pageBuf.clear();
    auto proc = [](HWND dlg, UINT msg, WPARAM wp, LPARAM) -> INT_PTR {
        if (msg == WM_INITDIALOG) {
            LocalizeDialog(dlg);
            SetFocus(GetDlgItem(dlg, IDC_BOOK_GO_TO_PAGE_EDIT));
            return FALSE;
        }
        if (msg == WM_COMMAND) {
            if (LOWORD(wp) == IDOK) {
                wchar_t buf[128] = {0};
                GetDlgItemTextW(dlg, IDC_BOOK_GO_TO_PAGE_EDIT, buf, 128);
                s_pageBuf = buf;
                EndDialog(dlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        }
        if (msg == WM_CLOSE) { EndDialog(dlg, IDCANCEL); return TRUE; }
        return FALSE;
    };
    INT_PTR r = DialogBoxW(GetModuleHandleW(nullptr),
                           MAKEINTRESOURCEW(IDD_BOOK_GO_TO_PAGE), owner, proc);
    if (r != IDOK || s_pageBuf.empty()) return;

    // Phase 1: page lookup goes through DaisyJumpToPageLabel, which walks the
    // book's page-typed nav points and matches by label. Implementation lives
    // in daisy_player.cpp where it can touch the book state directly.
    extern bool DaisyJumpToPageLabel(const std::wstring& q);
    if (!DaisyJumpToPageLabel(s_pageBuf)) {
        std::wstring msg = std::wstring(T("Page not found:")) + L" " + s_pageBuf;
        MessageBoxW(owner, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONINFORMATION);
    }
}

// -----------------------------------------------------------------------------
// F3 search inside the current book
// -----------------------------------------------------------------------------

// Cached search state — the dialog leaves the query and hits intact between
// invocations so "F3 → close dialog → F3 again" advances to the next hit
// without retyping.
struct SearchState {
    std::wstring query;
    std::vector<DaisySearchHit> hits;
    int cursor = -1;  // Index of the last-jumped hit; -1 = none yet
};
static SearchState s_search;
static std::wstring s_searchEditBuf;  // For dialog text exchange

static void JumpToHit(const DaisySearchHit& h) {
    if (h.clipIndex >= 0) {
        DaisyJumpToBookmark(h.clipIndex, 0.0);
    } else if (h.segmentIndex >= 0) {
        DaisyJumpToSegment(h.segmentIndex);
    }
}

static void PerformSearch(HWND owner) {
    const DaisyBook* book = DaisyCurrentBook();
    if (!book) return;
    if (s_search.query.empty()) return;
    s_search.hits = DaisySearchBook(*book, s_search.query);
    s_search.cursor = -1;
    if (s_search.hits.empty()) {
        std::wstring msg = std::wstring(T("No matches found for:")) + L" " + s_search.query;
        MessageBoxW(owner, msg.c_str(), L"MediaAccess", MB_OK | MB_ICONINFORMATION);
        return;
    }
    s_search.cursor = 0;
    JumpToHit(s_search.hits[0]);
    Speak(Ts("Found"));
}

static INT_PTR CALLBACK SearchDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM /*lp*/) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(dlg);
            SetDlgItemTextW(dlg, IDC_BOOK_SEARCH_EDIT, s_search.query.c_str());
            SendDlgItemMessageW(dlg, IDC_BOOK_SEARCH_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, IDC_BOOK_SEARCH_EDIT));
            return FALSE;
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDC_BOOK_SEARCH_NEXT) {
                wchar_t buf[1024] = {0};
                GetDlgItemTextW(dlg, IDC_BOOK_SEARCH_EDIT, buf, 1024);
                std::wstring newQuery = buf;
                if (newQuery.empty()) return TRUE;
                if (newQuery != s_search.query) {
                    s_search.query = newQuery;
                    s_search.hits.clear();
                    s_search.cursor = -1;
                }
                if (s_search.hits.empty()) {
                    PerformSearch(dlg);
                } else {
                    // Advance to next hit
                    s_search.cursor = (s_search.cursor + 1) % (int)s_search.hits.size();
                    JumpToHit(s_search.hits[s_search.cursor]);
                }
                return TRUE;
            }
            if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
            break;
        case WM_CLOSE:
            EndDialog(dlg, 0);
            return TRUE;
    }
    return FALSE;
}

void PromptSearchInBook(HWND owner) {
    if (!DaisyIsActive()) return;
    if (!DaisyHasText()) {
        MessageBoxW(owner, T("This book has no extractable text — search is not available."),
                    L"MediaAccess", MB_OK | MB_ICONINFORMATION);
        return;
    }
    DialogBoxW(GetModuleHandleW(nullptr),
               MAKEINTRESOURCEW(IDD_BOOK_SEARCH), owner, SearchDlgProc);
}

void FindNextInBook() {
    if (!DaisyIsActive()) return;
    if (s_search.hits.empty()) return;
    s_search.cursor = (s_search.cursor + 1) % (int)s_search.hits.size();
    JumpToHit(s_search.hits[s_search.cursor]);
}

} // namespace mediaaccess
