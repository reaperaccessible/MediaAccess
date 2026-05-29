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
// Phase 2: populate textContent on every clip; populate textSegments for
//          text-only books (DAISY/EPUB without audio) so TTS can speak them;
//          read DEFLATE-compressed entries inside .epub via miniz.
// =============================================================================

#include <memory>
#include <string>
#include <vector>

namespace mediaaccess {

// Categories of content that DAISY 3 / NIMAS / EPUB 3 mark as "skippable"
// so the listener can choose to bypass them during continuous reading
// (e.g. skip page numbers, footnotes, sidebars). Phase 4 (v1.55).
enum class SkipKind : uint8_t {
    None      = 0,
    Page      = 1,   // page-number announcement
    Note      = 2,
    Sidebar   = 3,
    Prodnote  = 4,   // producer's note
    Footnote  = 5,
    Reference = 6
};

// One playable audio clip with its position in the book's text.
struct DaisyClip {
    std::wstring audioFile;   // Absolute path to MP3/MP2 file
    double       clipBegin;   // Seconds into audio file where this clip starts
    double       clipEnd;     // Seconds into audio file where this clip ends (0 = to end-of-file)
    std::wstring textFile;    // Absolute path to XHTML file (optional, may be empty)
    std::string  textFragId;  // #fragment id inside the XHTML file (optional)
    std::wstring textContent; // Phase 2: plain text of the XHTML element with id=textFragId.
                              //   Empty if textFile/textFragId is missing or the
                              //   fragment could not be located.
    SkipKind     skipKind = SkipKind::None;  // Phase 4: category for skip-during-reading
};

// A heading / page / navigation target in the book's structure.
struct DaisyNavPoint {
    int          level;       // 1 = top heading, 2 = subheading, ... 0 = page, -1 = unknown
    std::wstring label;       // Display label ("Chapter 3", "Page 47", "Foreword")
    int          clipIndex;   // Index into DaisyBook::clips where this nav point starts
    enum Kind { Heading, Page, Other } kind = Heading;
};

// Phase 2: one paragraph-level text segment for TTS playback in text-only books.
// Populated for DAISY (no SMIL clips) and EPUB without Media Overlays.
struct DaisyTextSegment {
    std::wstring text;          // Whitespace-normalized plain text of one block element
    std::wstring textFile;      // XHTML file this came from (absolute path)
    std::string  textFragId;    // id attribute on the source element (may be empty)
    int          navLevel;      // -1 if not a heading, 1-6 for h1-h6, 0 for page numbers
    SkipKind     skipKind = SkipKind::None;  // Phase 4
};

enum class DaisyFormat { Unknown, Daisy202, Daisy3, Epub3 };

struct DaisyBook {
    std::wstring   path;            // Original path the user opened
    DaisyFormat    format = DaisyFormat::Unknown;
    std::wstring   title;
    std::wstring   author;
    std::wstring   language;
    double         totalDuration = 0.0;  // Sum of all clip durations, seconds

    std::vector<DaisyClip>        clips;        // Linear play sequence (audio books)
    std::vector<DaisyNavPoint>    navPoints;    // Sorted by clipIndex
    std::vector<DaisyTextSegment> textSegments; // Phase 2: text-only / EPUB-no-overlay

    // Phase 3: when EPUB Media Overlays are used we extract the audio
    // entries from inside the .epub to a per-book temp folder so BASS can
    // open them. The folder lives until DaisyClose() removes it.
    std::wstring tempAudioDir;

    // True if the book has no audio clips and must be spoken via TTS.
    bool isTextOnly() const { return clips.empty() || clips[0].audioFile.empty(); }
};

// Open a DAISY book from a path.
// Path can be:
//   - A folder containing NCC.html        -> DAISY 2.02
//   - A folder containing *.opf + *.ncx   -> DAISY 3
//   - A direct .opf file                  -> DAISY 3
//   - A direct .epub file                 -> EPUB 3
// Returns nullptr on failure; sets *errorOut (UTF-8) if non-null.
std::unique_ptr<DaisyBook> OpenDaisyBook(const std::wstring& path,
                                         std::string* errorOut = nullptr);

// Detect the format of a path without fully parsing.
DaisyFormat DetectDaisyFormat(const std::wstring& path);

// Returns a path's containing folder. Helper exposed for the library scanner.
std::wstring ParentFolder(const std::wstring& path);

// Phase 2: case-insensitive substring search over a book's text (clips and
// textSegments). Used by the F3 search dialog. Snippets are short previews
// suitable for a results list.
struct DaisySearchHit {
    int          clipIndex;     // -1 if the hit is in textSegments
    int          segmentIndex;  // -1 if the hit is in clips
    std::wstring snippet;       // ~80 chars around the match, single-line
};
std::vector<DaisySearchHit> DaisySearchBook(const DaisyBook& book,
                                            const std::wstring& needle);

} // namespace mediaaccess

#endif // MEDIAACCESS_DAISY_BOOK_H
