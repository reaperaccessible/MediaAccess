#pragma once
// cd_metadata — album and track names for an audio CD
// (docs/PLAN_EXTRACTION_CD.md, section 4.2 and annex A).
//
// Lookup order: the local cache (the user's last choice and corrections, by
// MusicBrainz Disc ID) -> MusicBrainz (exact Disc ID, or close table of
// contents) -> the disc's own CD-Text -> GnuDB (only when MusicBrainz does not
// know the disc) -> "Track 1", "Track 2"... for the user to name.
// Every network call is made from a worker thread, never the window's.

#include <windows.h>
#include <string>
#include <vector>
#include "disc_drives.h"

namespace mediaaccess {

struct CdTrackMeta {
    int          number = 0;      // number on the disc
    std::wstring title, artist, isrc, recordingMbid;
};

struct CdReleaseMeta {
    std::wstring album, albumArtist, year, country, label, catalog, barcode, genre;
    std::wstring releaseMbid, releaseGroupMbid;
    int  discNumber = 1, discTotal = 1;
    bool compilation = false;
    bool hasFrontCover = false;          // Cover Art Archive has a front image
    std::vector<CdTrackMeta> tracks;     // audio tracks, in disc order
    std::wstring source;                 // "MusicBrainz", "CD-Text", "GnuDB", "Cache", ""
};

struct CdLookupResult {
    std::string  discId;                 // MusicBrainz Disc ID ("" if unknown)
    std::wstring tocParam;               // "1+11+217407+150+..." (MusicBrainz toc=)
    std::vector<CdReleaseMeta> candidates;   // empty: nothing found anywhere
    bool fromCache = false;
};

// Reads the IDs from the disc, then looks the disc up. useCache=false skips
// the local cache (the user asked for a new search). Blocking: worker thread.
CdLookupResult LookupDisc(const DiscDrive& drive, const DiscContents& contents, bool useCache);

// "Track 1", "Track 2"... for every audio track (nothing was found).
CdReleaseMeta DefaultRelease(const DiscContents& contents);

// Remembers the chosen release and the user's corrections for this disc.
void SaveToCache(const std::string& discId, const CdReleaseMeta& meta);

// Front cover from the Cover Art Archive (500 px JPEG), release then release
// group. False when there is none. Blocking: worker thread.
bool DownloadFrontCover(const CdReleaseMeta& meta, std::vector<BYTE>& image);

// One line describing a candidate, for the edition list:
// "Album, Artist, 2006, CA, Label, barcode, CD 1 of 2, 14 tracks".
std::wstring DescribeRelease(const CdReleaseMeta& meta);

// Plain HTTP(S) GET (WinHTTP, MediaAccess user agent, 10 s timeouts,
// redirects followed). False on a network error; *status holds the HTTP code.
bool HttpGetBytes(const std::wstring& url, DWORD* status, std::string* body, size_t maxBytes);

// The page where the user can add this disc to MusicBrainz.
std::wstring MusicBrainzSubmitUrl(const CdLookupResult& r, int audioTracks);

} // namespace mediaaccess
