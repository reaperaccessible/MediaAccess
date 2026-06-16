// =============================================================================
// cue_sheet.cpp — .cue sheet parsing + loading (v2.34, tester Séb).
//
// See cue_sheet.h for the architecture. The parser reads the whole .cue as raw
// bytes and decodes it to a single wide string (UTF-8 / UTF-16 LE / UTF-16 BE
// BOM sniff, CP_UTF8 then CP_ACP fallback for BOM-less files), then iterates
// lines. This is deliberately NOT fgets-based: UTF-16 .cue files (some rippers
// emit them) contain embedded NUL bytes that truncate fgets lines.
// =============================================================================

#include "mediaaccess/cue_sheet.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <vector>
#include <string>
#include <algorithm>

#include "globals.h"
#include "player.h"        // SetExternalChapters, LoadFile, PlayTrack, SeekToPosition
#include "ui.h"            // ApplyNowPlayingForCurrentTrack lives in player.h, NotifyPlaylistTrackChanged in ui.h
#include "utils.h"         // WideToUtf8
#include "translations.h"  // Ts / T
#include "accessibility.h" // Speak
#include "resource.h"
#include "video_engine.h"  // IsVideoFile — exotic video-container cue guard (v2.34)

// g_playlist / g_currentTrack live in globals.cpp; declared via globals.h.

// -----------------------------------------------------------------------------
// Small string helpers (file-local).
// -----------------------------------------------------------------------------

static std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) a++;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) b--;
    return s.substr(a, b - a);
}

// Uppercase ASCII copy of the leading keyword (for case-insensitive matching).
static std::wstring UpperW(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towupper(c);
    return out;
}

// Strip surrounding double quotes if present; also unescape doubled "" -> ".
// CUE values are usually quoted; tolerate unquoted single tokens too.
static std::wstring Unquote(const std::wstring& s) {
    std::wstring t = TrimWs(s);
    if (t.size() >= 2 && t.front() == L'"' && t.back() == L'"') {
        t = t.substr(1, t.size() - 2);
    }
    // Unescape doubled quotes.
    std::wstring out;
    out.reserve(t.size());
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'"' && i + 1 < t.size() && t[i + 1] == L'"') {
            out += L'"';
            i++;
        } else {
            out += t[i];
        }
    }
    return out;
}

// Resolve a FILE path against the .cue's directory. Mirrors ParseM3U's logic
// (ui.cpp): absolute (drive-letter / UNC) or URL-like paths pass through;
// everything else is relative to baseDir.
static std::wstring ResolveFilePath(const std::wstring& entry, const std::wstring& baseDir) {
    if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
        _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
        _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
        (entry.length() > 2 && entry[1] == L':') ||              // C:\...
        (entry.length() > 2 && entry[0] == L'\\' && entry[1] == L'\\')) { // \\server\share
        return entry;
    }
    return baseDir + entry;
}

// -----------------------------------------------------------------------------
// Raw-byte read + decode the whole .cue to one wide string.
// -----------------------------------------------------------------------------

static bool ReadCueText(const std::wstring& cuePath, std::wstring& out) {
    FILE* f = _wfopen(cuePath.c_str(), L"rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return false; }

    std::vector<unsigned char> bytes((size_t)size);
    size_t got = fread(bytes.data(), 1, (size_t)size, f);
    fclose(f);
    if (got == 0) return false;
    bytes.resize(got);

    // BOM sniff.
    if (got >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        // UTF-8 BOM
        const char* p = reinterpret_cast<const char*>(bytes.data()) + 3;
        int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(got - 3), nullptr, 0);
        if (n > 0) { out.resize(n); MultiByteToWideChar(CP_UTF8, 0, p, (int)(got - 3), &out[0], n); }
        return n > 0;
    }
    if (got >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        // UTF-16 LE BOM — bytes are already wide chars.
        const wchar_t* w = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        out.assign(w, (got - 2) / sizeof(wchar_t));
        return true;
    }
    if (got >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        // UTF-16 BE BOM — byte-swap to LE then assign.
        size_t count = (got - 2) / 2;
        out.resize(count);
        const unsigned char* p = bytes.data() + 2;
        for (size_t i = 0; i < count; i++) {
            out[i] = (wchar_t)((p[i * 2] << 8) | p[i * 2 + 1]);
        }
        return true;
    }

    // No BOM: try strict UTF-8, then fall back to the ANSI code page.
    const char* p = reinterpret_cast<const char*>(bytes.data());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, (int)got, nullptr, 0);
    if (n > 0) {
        out.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, p, (int)got, &out[0], n);
        return true;
    }
    n = MultiByteToWideChar(CP_ACP, 0, p, (int)got, nullptr, 0);
    if (n <= 0) return false;
    out.resize(n);
    MultiByteToWideChar(CP_ACP, 0, p, (int)got, &out[0], n);
    return true;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool IsCueFile(const std::wstring& path) {
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    return ext == L".cue";
}

bool ParseCueSheet(const std::wstring& cuePath, CueSheet& out) {
    out = CueSheet{};

    std::wstring text;
    if (!ReadCueText(cuePath, text)) return false;

    // baseDir of the .cue, for relative FILE resolution.
    std::wstring baseDir;
    size_t lastSlash = cuePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = cuePath.substr(0, lastSlash + 1);
    }

    int currentFileIndex = -1;
    bool sawTrack = false;       // distinguishes album-level vs track-level TITLE/PERFORMER

    // Iterate lines (handle \n, \r\n, lone \r).
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find_first_of(L"\r\n", pos);
        std::wstring rawLine = (nl == std::wstring::npos)
                                   ? text.substr(pos)
                                   : text.substr(pos, nl - pos);
        if (nl == std::wstring::npos) {
            pos = text.size() + 1;
        } else {
            // Skip a paired \r\n.
            if (text[nl] == L'\r' && nl + 1 < text.size() && text[nl + 1] == L'\n') pos = nl + 2;
            else pos = nl + 1;
        }

        std::wstring line = TrimWs(rawLine);
        if (line.empty()) continue;

        // Split keyword / remainder.
        size_t sp = line.find_first_of(L" \t");
        std::wstring keyword = UpperW(sp == std::wstring::npos ? line : line.substr(0, sp));
        std::wstring rest = (sp == std::wstring::npos) ? std::wstring() : TrimWs(line.substr(sp + 1));

        if (keyword == L"FILE") {
            // FILE "name" TYPE  — strip the trailing TYPE token (WAVE/MP3/...).
            std::wstring fileSpec = rest;
            if (!fileSpec.empty() && fileSpec.front() == L'"') {
                size_t closeq = fileSpec.find(L'"', 1);
                if (closeq != std::wstring::npos) fileSpec = fileSpec.substr(0, closeq + 1);
            } else {
                // Unquoted: take the first whitespace-delimited token.
                size_t tsp = fileSpec.find_first_of(L" \t");
                if (tsp != std::wstring::npos) fileSpec = fileSpec.substr(0, tsp);
            }
            std::wstring name = Unquote(fileSpec);
            if (!name.empty()) {
                out.files.push_back(ResolveFilePath(name, baseDir));
                currentFileIndex = (int)out.files.size() - 1;
            }
        } else if (keyword == L"TRACK") {
            // TRACK nn AUDIO  — ignore non-AUDIO track types.
            std::wstring upRest = UpperW(rest);
            if (upRest.find(L"AUDIO") == std::wstring::npos) continue;
            int num = _wtoi(rest.c_str());
            CueTrack t;
            t.number = num;
            t.fileIndex = (currentFileIndex < 0) ? 0 : currentFileIndex;
            out.tracks.push_back(t);
            sawTrack = true;
        } else if (keyword == L"TITLE") {
            std::wstring val = Unquote(rest);
            if (!sawTrack || out.tracks.empty()) out.albumTitle = val;
            else out.tracks.back().title = val;
        } else if (keyword == L"PERFORMER") {
            std::wstring val = Unquote(rest);
            if (!sawTrack || out.tracks.empty()) out.albumPerformer = val;
            else out.tracks.back().performer = val;
        } else if (keyword == L"INDEX") {
            // INDEX nn mm:ss:ff — use INDEX 01 for the start; fall back to 00
            // only when no 01 appears for the track. INDEX 00 alone is pregap.
            if (out.tracks.empty()) continue;
            size_t isp = rest.find_first_of(L" \t");
            if (isp == std::wstring::npos) continue;
            int indexNum = _wtoi(rest.substr(0, isp).c_str());
            std::wstring ts = TrimWs(rest.substr(isp + 1));
            // mm:ss:ff
            int mm = 0, ss = 0, ff = 0;
            if (swscanf(ts.c_str(), L"%d:%d:%d", &mm, &ss, &ff) >= 2) {
                if (ff < 0) ff = 0;
                if (ff > 74) ff = 74;
                double startSec = mm * 60.0 + ss + ff / 75.0;
                CueTrack& t = out.tracks.back();
                if (indexNum == 1) {
                    t.startSec = startSec;
                } else if (indexNum == 0 && t.startSec == 0.0) {
                    // Provisional pregap start; overwritten if an INDEX 01 follows.
                    t.startSec = startSec;
                }
            }
        }
        // REM and every other keyword: skip.
    }

    return !out.tracks.empty();
}

std::vector<Chapter> CueToChapters(const CueSheet& sheet, int fileIndex) {
    std::vector<Chapter> chapters;
    for (const auto& t : sheet.tracks) {
        if (t.fileIndex != fileIndex) continue;

        // Name: "NN. Title" (zero-padded), with the track performer folded in
        // when it differs from the album performer.
        std::wstring title = t.title;
        if (title.empty()) {
            // "Track N" fallback, localized (T() returns wide text directly).
            title = std::wstring(T("Track ")) + std::to_wstring(t.number);
        }

        wchar_t numbuf[16];
        swprintf(numbuf, 16, L"%02d. ", t.number);
        std::wstring name = std::wstring(numbuf) + title;

        if (!t.performer.empty() && t.performer != sheet.albumPerformer) {
            name += L" - " + t.performer;
        }

        chapters.push_back(Chapter{ t.startSec, name });
    }

    std::sort(chapters.begin(), chapters.end(),
              [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
    return chapters;
}

// Probe for an audio file that exists. Returns true and leaves `path` if the
// referenced file is present. If absent, tries the cue's basename with common
// BASS audio extensions in the same folder (handles a renamed/re-encoded rip).
static bool ResolveExistingAudio(std::wstring& path, const std::wstring& cuePath) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;
    }

    // Fallback: <cue-folder>\<cue-basename>.<ext>
    std::wstring baseDir;
    size_t lastSlash = cuePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) baseDir = cuePath.substr(0, lastSlash + 1);
    std::wstring stem = cuePath.substr(lastSlash == std::wstring::npos ? 0 : lastSlash + 1);
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);

    static const wchar_t* exts[] = { L".flac", L".wav", L".ape", L".wv",
                                     L".mp3", L".m4a", L".ogg", L".opus" };
    for (const wchar_t* ext : exts) {
        std::wstring cand = baseDir + stem + ext;
        DWORD a = GetFileAttributesW(cand.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            path = cand;
            return true;
        }
    }
    return false;
}

bool OpenCueSheet(const std::wstring& cuePath, bool restoreMode) {
    if (g_isShuttingDown) return false;
    // In restore mode (silent startup), load PAUSED like the normal restore path
    // and never speak — a missing cue/audio just falls back to the generic restore.
    const bool autoPlay = !restoreMode;

    CueSheet sheet;
    if (!ParseCueSheet(cuePath, sheet)) {
        if (!restoreMode) Speak(Ts("Could not read this cue sheet."));
        return false;
    }
    if (sheet.files.empty()) {
        if (!restoreMode) Speak(Ts("Audio file for this cue sheet was not found."));
        return false;
    }

    if (!sheet.multiFile()) {
        // ---- Single-FILE: chapters over one stream ----
        std::wstring audio = sheet.files[0];
        if (!ResolveExistingAudio(audio, cuePath)) {
            if (!restoreMode) Speak(Ts("Audio file for this cue sheet was not found."));
            return false;
        }

        // Exotic guard: a .cue whose FILE is a video container goes through the
        // MPV load path, which does NOT consume the external-chapter latch — that
        // would leak our chapters onto the next normal load. So for a video-backed
        // cue we play it plainly, without injecting chapters (no track nav).
        if (IsVideoFile(audio)) {
            g_chaptersAreCueTracks = false;
            g_currentCuePath.clear();
            g_playlist.clear();
            g_playlist.push_back(audio);
            g_currentTrack = 0;
            PlayTrack(0, autoPlay);
            NotifyPlaylistTrackChanged();
            return true;
        }

        std::vector<Chapter> chapters = CueToChapters(sheet, 0);

        // Inject chapters BEFORE the load so ParseChapters() preserves them
        // (one-shot external-chapter latch), then load atomically. SetExternal
        // Chapters resets the cue-track wording flag, so we re-assert it AFTER
        // the load (below) — once ParseChapters has consumed the latch.
        SetExternalChapters(chapters);
        g_currentCuePath = cuePath;

        g_playlist.clear();
        g_playlist.push_back(audio);
        g_currentTrack = 0;
        PlayTrack(0, autoPlay);

        g_chaptersAreCueTracks = !chapters.empty();
        NotifyPlaylistTrackChanged();
        return true;
    }

    // ---- Multi-FILE: each FILE = one playlist entry (file-level split) ----
    // No chapters are injected, so there is nothing cue-specific to restore on
    // restart — the normal [Playlist]/[State] persistence already remembers the
    // file list, current track and position. Clear g_currentCuePath so we don't
    // pointlessly re-open the cue (and reset to track 0) at next launch.
    g_chaptersAreCueTracks = false;
    g_currentCuePath.clear();

    g_playlist.clear();
    for (const auto& f : sheet.files) {
        DWORD a = GetFileAttributesW(f.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            g_playlist.push_back(f);
        }
    }
    if (g_playlist.empty()) {
        if (!restoreMode) Speak(Ts("Audio file for this cue sheet was not found."));
        return false;
    }
    g_currentTrack = 0;
    PlayTrack(0, autoPlay);
    NotifyPlaylistTrackChanged();
    return true;
}

void AnnounceCurrentCueTrack() {
    if (g_chapters.empty()) {
        Speak(Ts("No track"));
        return;
    }
    int idx = GetCurrentChapterIndex();
    if (idx < 0) idx = 0;  // before the first INDEX -> report track 1
    std::string msg = Ts("Track ") + std::to_string(idx + 1) + " " +
                      Ts("of ") + std::to_string(g_chapters.size()) + ": " +
                      WideToUtf8(g_chapters[idx].name);
    Speak(msg);
}
