#pragma once
#ifndef MEDIAACCESS_DAISY_BOOK_H
#define MEDIAACCESS_DAISY_BOOK_H

// =============================================================================
// daisy_book.h — DAISY 2.02 / DAISY 3 / EPUB 3 book parser
//
// Loads the structural metadata for an accessible Talking Book and produces a
// linear list of (audio, time-range, text fragment) tuples plus a navigation
// table (headings + page numbers).
//
// Phase 1: parse DAISY 2.02 and DAISY 3 fully (clips + navPoints).
//          EPUB 3 only returns title/author so the library can list it.
// =============================================================================

#include <memory>
#include <string>
#include <vector>

namespace mediaaccess {

// One playable audio clip with its position in the book's text.
struct DaisyClip {
    std::wstring audioFile;   // Absolute path to MP3/MP2 file
    double       clipBegin;   // Seconds into audio file where this clip starts
    double       clipEnd;     // Seconds into audio file where this clip ends (0 = to end-of-file)
    std::wstring textFile;    // Absolute path to XHTML file (optional, may be empty)
    std::string  textFragId;  // #fragment id inside the XHTML file (optional)
};

// A heading / page / navigation target in the book's structure.
struct DaisyNavPoint {
    int          level;       // 1 = top heading, 2 = subheading, ... 0 = page, -1 = unknown
    std::wstring label;       // Display label ("Chapter 3", "Page 47", "Foreword")
    int          clipIndex;   // Index into DaisyBook::clips where this nav point starts
    enum Kind { Heading, Page, Other } kind = Heading;
};

enum class DaisyFormat { Unknown, Daisy202, Daisy3, Epub3 };

struct DaisyBook {
    std::wstring   path;            // Original path the user opened
    DaisyFormat    format = DaisyFormat::Unknown;
    std::wstring   title;
    std::wstring   author;
    std::wstring   language;
    double         totalDuration = 0.0;  // Sum of all clip durations, seconds

    std::vector<DaisyClip>     clips;     // Linear play sequence
    std::vector<DaisyNavPoint> navPoints; // Sorted by clipIndex

    // True if the book is text-only (no audio clips — synthesize via TTS later)
    bool isTextOnly() const { return clips.empty() || clips[0].audioFile.empty(); }
};

// Open a DAISY book from a path.
// Path can be:
//   - A folder containing NCC.html        -> DAISY 2.02
//   - A folder containing *.opf + *.ncx   -> DAISY 3
//   - A direct .opf file                  -> DAISY 3
//   - A direct .epub file                 -> EPUB 3 (returns DaisyFormat::Epub3
//                                            with metadata only; clips empty)
// Returns nullptr on failure; sets *errorOut (UTF-8) if non-null.
std::unique_ptr<DaisyBook> OpenDaisyBook(const std::wstring& path,
                                         std::string* errorOut = nullptr);

// Detect the format of a path without fully parsing.
DaisyFormat DetectDaisyFormat(const std::wstring& path);

// Returns a path's containing folder. Helper exposed for the library scanner.
std::wstring ParentFolder(const std::wstring& path);

} // namespace mediaaccess

#endif // MEDIAACCESS_DAISY_BOOK_H
