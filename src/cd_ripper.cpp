// cd_ripper.cpp — see cd_ripper.h.

#include "cd_ripper.h"
#include "basscd_loader.h"
#include "translations.h"
#include "utils.h"
#include "logger.h"
#include "bassenc.h"
#include "bassenc_flac.h"
#include "bassenc_mp3.h"
#include "bassenc_ogg.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <shlobj.h>

namespace mediaaccess {

namespace {

const double kBytesPerSecond = 176400.0;   // 44100 Hz, 16-bit, stereo

std::atomic<bool> g_running{false};
std::atomic<bool> g_cancel{false};
std::mutex        g_progressMutex;
RipProgress       g_progress;

// ---- bassenc_opus.dll: optional, loaded by its full path (no .lib) ----

typedef HENCODE (WINAPI *OpusStartFileFn)(DWORD handle, const char* options, DWORD flags, const char* filename);

OpusStartFileFn OpusStartFile() {
    static std::once_flag once;
    static OpusStartFileFn fn = nullptr;
    std::call_once(once, []() {
        wchar_t exe[32768];
        DWORD got = GetModuleFileNameW(nullptr, exe, 32768);
        if (got == 0 || got >= 32768) return;
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (!slash) return;
        *(slash + 1) = L'\0';
        std::wstring full = std::wstring(exe) + L"lib\\bassenc_opus.dll";
        if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        HMODULE dll = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (dll) fn = reinterpret_cast<OpusStartFileFn>(GetProcAddress(dll, "BASS_Encode_OPUS_StartFile"));
        LogF("CDRIP", "bassenc_opus.dll %s", fn ? "loaded" : "unusable");
    });
    return fn;
}

// ---- Quality lists (plan 4.5 defaults: FLAC 5, MP3 V0, Ogg 6, Opus 160) ----

struct Quality { const char* label; const wchar_t* option; };

const Quality kFlac[] = {
    { "Level 5 (default)", L"-5" }, { "Level 8 (smallest, slowest)", L"-8" },
    { "Level 0 (fastest)", L"-0" },
};
const Quality kMp3[] = {
    { "VBR V0, about 245 kbps (default)", L"-V 0" }, { "VBR V2, about 190 kbps", L"-V 2" },
    { "VBR V4, about 165 kbps", L"-V 4" }, { "CBR 320 kbps", L"-b 320" },
    { "CBR 256 kbps", L"-b 256" }, { "CBR 192 kbps", L"-b 192" }, { "CBR 128 kbps", L"-b 128" },
};
const Quality kOgg[] = {
    { "Quality 6, about 192 kbps (default)", L"-q 6" }, { "Quality 8, about 256 kbps", L"-q 8" },
    { "Quality 10, about 500 kbps", L"-q 10" }, { "Quality 5, about 160 kbps", L"-q 5" },
    { "Quality 4, about 128 kbps", L"-q 4" },
};
const Quality kOpus[] = {
    { "160 kbps (default)", L"--bitrate 160" }, { "192 kbps", L"--bitrate 192" },
    { "256 kbps", L"--bitrate 256" }, { "128 kbps", L"--bitrate 128" }, { "96 kbps", L"--bitrate 96" },
};
const Quality kWav[] = { { "16-bit, 44.1 kHz (as on the CD)", L"" } };

const Quality* Qualities(RipFormat f, int* count) {
    switch (f) {
        case RipFormat::FLAC: *count = (int)_countof(kFlac); return kFlac;
        case RipFormat::MP3:  *count = (int)_countof(kMp3);  return kMp3;
        case RipFormat::OGG:  *count = (int)_countof(kOgg);  return kOgg;
        case RipFormat::OPUS: *count = (int)_countof(kOpus); return kOpus;
        default:              *count = (int)_countof(kWav);  return kWav;
    }
}

// ---- Encoder options with the tags (plan 4.5) ----

// One quoted value. The encoders read their options like a command line:
// a double quote inside a value is written \" (checked in phase 4).
std::wstring Q(const std::wstring& v) {
    std::wstring out = L"\"";
    for (wchar_t c : v) {
        if (c == L'"') out += L"\\\"";
        else if (c < 32) out += L' ';
        else out += c;
    }
    return out + L"\"";
}

std::wstring BuildOptions(const RipJob& job, const RipTrack& t, const std::wstring& coverPath) {
    int count = 0;
    const Quality* q = Qualities(job.format, &count);
    int qi = (job.quality >= 0 && job.quality < count) ? job.quality : 0;
    std::wstring o = q[qi].option;
    const CdReleaseMeta& m = job.meta;
    std::wstring num = std::to_wstring(t.number), total = std::to_wstring(job.trackTotal);
    std::wstring disc = std::to_wstring(m.discNumber), discs = std::to_wstring(m.discTotal);
    auto add = [&o](const std::wstring& opt, const std::wstring& value) {
        if (!value.empty()) o += L" " + opt + L" " + Q(value);
    };
    switch (job.format) {
        case RipFormat::FLAC:
            add(L"-T", L"TITLE=" + t.title);   add(L"-T", L"ARTIST=" + t.artist);
            add(L"-T", L"ALBUM=" + m.album);   add(L"-T", L"ALBUMARTIST=" + m.albumArtist);
            if (!m.year.empty()) add(L"-T", L"DATE=" + m.year);
            add(L"-T", L"TRACKNUMBER=" + num); add(L"-T", L"TRACKTOTAL=" + total);
            add(L"-T", L"DISCNUMBER=" + disc); add(L"-T", L"DISCTOTAL=" + discs);
            if (!m.genre.empty()) add(L"-T", L"GENRE=" + m.genre);
            if (!t.isrc.empty()) add(L"-T", L"ISRC=" + t.isrc);
            if (m.compilation) add(L"-T", L"COMPILATION=1");
            if (!m.releaseMbid.empty()) add(L"-T", L"MUSICBRAINZ_ALBUMID=" + m.releaseMbid);
            if (!m.releaseGroupMbid.empty()) add(L"-T", L"MUSICBRAINZ_RELEASEGROUPID=" + m.releaseGroupMbid);
            if (!t.recordingMbid.empty()) add(L"-T", L"MUSICBRAINZ_TRACKID=" + t.recordingMbid);
            add(L"--picture", coverPath);
            break;
        case RipFormat::MP3:
            add(L"--tt", t.title);  add(L"--ta", t.artist);  add(L"--tl", m.album);
            add(L"--ty", m.year);   add(L"--tn", num + L"/" + total);  add(L"--tg", m.genre);
            if (!m.albumArtist.empty()) add(L"--tv", L"TPE2=" + m.albumArtist);
            add(L"--tv", L"TPOS=" + disc + L"/" + discs);
            if (!t.isrc.empty()) add(L"--tv", L"TSRC=" + t.isrc);
            if (m.compilation) add(L"--tv", L"TCMP=1");
            if (!m.releaseMbid.empty()) add(L"--tv", L"TXXX=MusicBrainz Album Id=" + m.releaseMbid);
            if (!t.recordingMbid.empty()) add(L"--tv", L"TXXX=MusicBrainz Track Id=" + t.recordingMbid);
            add(L"--ti", coverPath);
            break;
        case RipFormat::OGG:
            add(L"-t", t.title);  add(L"-a", t.artist);  add(L"-l", m.album);
            add(L"-d", m.year);   add(L"-N", num);       add(L"-G", m.genre);
            if (!m.albumArtist.empty()) add(L"-c", L"ALBUMARTIST=" + m.albumArtist);
            add(L"-c", L"TRACKTOTAL=" + total);
            add(L"-c", L"DISCNUMBER=" + disc);  add(L"-c", L"DISCTOTAL=" + discs);
            if (!t.isrc.empty()) add(L"-c", L"ISRC=" + t.isrc);
            if (m.compilation) add(L"-c", L"COMPILATION=1");
            if (!m.releaseMbid.empty()) add(L"-c", L"MUSICBRAINZ_ALBUMID=" + m.releaseMbid);
            if (!t.recordingMbid.empty()) add(L"-c", L"MUSICBRAINZ_TRACKID=" + t.recordingMbid);
            add(L"--picture", coverPath);
            break;
        case RipFormat::OPUS:
            add(L"--title", t.title);  add(L"--artist", t.artist);  add(L"--album", m.album);
            add(L"--date", m.year);    add(L"--tracknumber", num);  add(L"--genre", m.genre);
            if (!m.albumArtist.empty()) add(L"--comment", L"ALBUMARTIST=" + m.albumArtist);
            add(L"--comment", L"TRACKTOTAL=" + total);
            add(L"--comment", L"DISCNUMBER=" + disc);  add(L"--comment", L"DISCTOTAL=" + discs);
            if (!t.isrc.empty()) add(L"--comment", L"ISRC=" + t.isrc);
            if (m.compilation) add(L"--comment", L"COMPILATION=1");
            if (!m.releaseMbid.empty()) add(L"--comment", L"MUSICBRAINZ_ALBUMID=" + m.releaseMbid);
            if (!t.recordingMbid.empty()) add(L"--comment", L"MUSICBRAINZ_TRACKID=" + t.recordingMbid);
            add(L"--picture", coverPath);
            break;
        default:
            break;
    }
    return o;
}

void Post(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (IsWindow(hwnd)) PostMessageW(hwnd, msg, w, l);
}

void PostError(HWND hwnd, const std::wstring& text) {
    std::wstring* heap = new std::wstring(text);
    if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_CDRIP_ERROR, 0, reinterpret_cast<LPARAM>(heap))) {
        delete heap;
    }
}

std::wstring Fmt(const wchar_t* fmt, const std::wstring& a) {
    wchar_t buf[1024];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, a.c_str());
    return buf;
}

std::wstring FmtInt(const wchar_t* fmt, int n) {
    wchar_t buf[512];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, n);
    return buf;
}

// Starts the encoder that writes `path` from the decoding stream.
HENCODE StartEncoder(HSTREAM s, const RipJob& job, const RipTrack& t,
                     const std::wstring& path, const std::wstring& coverPath) {
    const DWORD flags = BASS_ENCODE_AUTOFREE | BASS_UNICODE;
    if (job.format == RipFormat::WAV) return BASS_Encode_StartPCMFile(s, flags, path.c_str());
    std::wstring opts = BuildOptions(job, t, coverPath);
    switch (job.format) {
        case RipFormat::FLAC: return BASS_Encode_FLAC_StartFile(s, opts.c_str(), flags, path.c_str());
        case RipFormat::MP3:  return BASS_Encode_MP3_StartFile(s, opts.c_str(), flags, path.c_str());
        case RipFormat::OGG:  return BASS_Encode_OGG_StartFile(s, opts.c_str(), flags, path.c_str());
        case RipFormat::OPUS: {
            OpusStartFileFn fn = OpusStartFile();   // BASS_UNICODE: options and name are wide
            return fn ? fn(s, reinterpret_cast<const char*>(opts.c_str()), flags,
                           reinterpret_cast<const char*>(path.c_str())) : 0;
        }
        default: return 0;
    }
}

enum class TrackResult { Done, Cancelled, Failed };

TrackResult RipOneTrack(const basscd::Api* cd, const RipJob& job, const RipTrack& t,
                        size_t pos, double secondsBefore, double secondsTotal,
                        ULONGLONG startTick, const std::wstring& coverPath, HWND notify) {
    std::wstring finalPath = job.folder + L"\\" + t.fileName + RipFormatExtension(job.format);
    std::wstring partPath = finalPath + L".part";
    // A file template may hold a sub-folder ("%disc%\%track% - %title%").
    std::wstring parent = finalPath.substr(0, finalPath.find_last_of(L'\\'));
    if (parent != job.folder) SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);

    HSTREAM s = cd->StreamCreate(job.drive.bassIndex, static_cast<DWORD>(t.index), BASS_STREAM_DECODE);
    if (!s) {
        LogF("CDRIP", "StreamCreate track %d failed: error %d", t.index, BASS_ErrorGetCode());
        PostError(notify, FmtInt(T("Track %d cannot be read. Check that the disc is in the drive."), t.number));
        return TrackResult::Failed;
    }
    if (!StartEncoder(s, job, t, partPath, coverPath)) {
        int err = BASS_ErrorGetCode();
        LogF("CDRIP", "encoder start failed for %ls: error %d", partPath.c_str(), err);
        BASS_StreamFree(s);
        PostError(notify, Fmt(T("Cannot write to the folder %ls. Choose another folder."), job.folder));
        return TrackResult::Failed;
    }

    const QWORD total = BASS_ChannelGetLength(s, BASS_POS_BYTE);
    QWORD done = 0;
    static BYTE buf[588 * 4 * 75];   // 75 CD frames = 1 second
    TrackResult result = TrackResult::Done;
    for (;;) {
        if (g_cancel) { result = TrackResult::Cancelled; break; }
        DWORD got = BASS_ChannelGetData(s, buf, sizeof(buf));
        if (got == static_cast<DWORD>(-1)) {
            int err = BASS_ErrorGetCode();
            if (err != BASS_ERROR_ENDED) {
                LogF("CDRIP", "read error on track %d: %d", t.index, err);
                PostError(notify, FmtInt(T("Reading track %d failed. The disc may have been removed."), t.number));
                result = TrackResult::Failed;
            }
            break;
        }
        // The encoder stops by itself when it cannot write (disk full...).
        if (!BASS_Encode_IsActive(s)) {
            LogF("CDRIP", "encoder stopped while writing %ls", partPath.c_str());
            PostError(notify, Fmt(T("Writing to %ls failed. The disk may be full."), job.folder));
            result = TrackResult::Failed;
            break;
        }
        done += got;
        double trackFrac = (total && total != static_cast<QWORD>(-1)) ? double(done) / double(total) : 0.0;
        double doneSeconds = secondsBefore + trackFrac * t.seconds;
        double totalFrac = secondsTotal > 0 ? doneSeconds / secondsTotal : 0.0;
        double elapsed = (GetTickCount64() - startTick) / 1000.0;
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_progress.trackPos = static_cast<int>(pos) + 1;
        g_progress.trackFraction = trackFrac;
        g_progress.totalFraction = totalFrac;
        g_progress.secondsLeft = (totalFrac > 0.02) ? elapsed * (1.0 - totalFrac) / totalFrac : -1.0;
    }

    BASS_Encode_Stop(s);   // flushes and closes the file
    BASS_StreamFree(s);
    if (result != TrackResult::Done) {
        DeleteFileW(partPath.c_str());
        return result;
    }
    if (!MoveFileExW(partPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LogF("CDRIP", "rename failed %ls: %lu", partPath.c_str(), GetLastError());
        DeleteFileW(partPath.c_str());
        PostError(notify, Fmt(T("Cannot write to the folder %ls. Choose another folder."), job.folder));
        return TrackResult::Failed;
    }
    LogF("CDRIP", "track %d written: %ls", t.index, finalPath.c_str());
    return TrackResult::Done;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t size, bool replace) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, replace ? CREATE_ALWAYS : CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, (DWORD)size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

bool WriteUtf8(const std::wstring& path, const std::wstring& text, bool bom) {
    std::string bytes = bom ? std::string("\xEF\xBB\xBF") : std::string();
    bytes += WideToUtf8(text);
    return WriteBytes(path, bytes.data(), bytes.size(), true);
}

// CUE: one FILE per track, readable again by cue_sheet.cpp. Values in double
// quotes, so a double quote inside one becomes a single quote.
std::wstring CueValue(const std::wstring& v) {
    std::wstring out = v;
    for (wchar_t& c : out) if (c == L'"') c = L'\'';
    return L"\"" + out + L"\"";
}

void WriteLists(const RipJob& job, const std::vector<const RipTrack*>& written) {
    if (written.empty() || job.listName.empty()) return;
    const wchar_t* ext = RipFormatExtension(job.format);
    const CdReleaseMeta& m = job.meta;
    if (job.writeCue) {
        const wchar_t* type = (job.format == RipFormat::MP3) ? L"MP3" : L"WAVE";
        std::wstring c;
        if (!m.genre.empty()) c += L"REM GENRE " + CueValue(m.genre) + L"\r\n";
        if (!m.year.empty()) c += L"REM DATE " + m.year + L"\r\n";
        if (!m.albumArtist.empty()) c += L"PERFORMER " + CueValue(m.albumArtist) + L"\r\n";
        if (!m.album.empty()) c += L"TITLE " + CueValue(m.album) + L"\r\n";
        for (const RipTrack* t : written) {
            wchar_t num[8];
            _snwprintf_s(num, _countof(num), _TRUNCATE, L"%02d", t->number);
            c += L"FILE " + CueValue(t->fileName + ext) + L" " + type + L"\r\n";
            c += L"  TRACK " + std::wstring(num) + L" AUDIO\r\n";
            if (!t->title.empty()) c += L"    TITLE " + CueValue(t->title) + L"\r\n";
            if (!t->artist.empty()) c += L"    PERFORMER " + CueValue(t->artist) + L"\r\n";
            if (!t->isrc.empty()) c += L"    ISRC " + t->isrc + L"\r\n";
            c += L"    INDEX 01 00:00:00\r\n";
        }
        WriteUtf8(job.folder + L"\\" + job.listName + L".cue", c, true);
    }
    if (job.writeM3u) {
        std::wstring p = L"#EXTM3U\r\n";
        for (const RipTrack* t : written) {
            std::wstring who = t->artist.empty() ? m.albumArtist : t->artist;
            p += L"#EXTINF:" + std::to_wstring((int)(t->seconds + 0.5)) + L"," +
                 (who.empty() ? t->title : who + L" - " + t->title) + L"\r\n";
            p += t->fileName + ext + L"\r\n";
        }
        WriteUtf8(job.folder + L"\\" + job.listName + L".m3u8", p, false);
    }
}

void Worker(RipJob job, HWND notify) {
    const basscd::Api* cd = basscd::Get();
    std::vector<const RipTrack*> written;
    bool stopped = false;
    std::wstring tempCover;

    int mk = cd ? SHCreateDirectoryExW(nullptr, job.folder.c_str(), nullptr) : ERROR_INVALID_FUNCTION;
    if (!cd || (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS)) {
        LogF("CDRIP", "cannot create %ls: %d", job.folder.c_str(), mk);
        PostError(notify, Fmt(T("Cannot create the folder %ls. Choose another folder."), job.folder));
        stopped = true;
    } else {
        // The cover first (a few seconds at most): folder.jpg (never replaced
        // when already there) and a temporary copy the encoders embed.
        std::vector<BYTE> image;
        bool wantFolder = job.saveFolderJpg &&
                          GetFileAttributesW((job.folder + L"\\folder.jpg").c_str()) == INVALID_FILE_ATTRIBUTES;
        bool wantEmbed = job.embedCover && job.format != RipFormat::WAV;
        if (job.meta.hasFrontCover && (wantFolder || wantEmbed) && DownloadFrontCover(job.meta, image)) {
            if (wantFolder) WriteBytes(job.folder + L"\\folder.jpg", image.data(), image.size(), false);
            if (wantEmbed) {
                wchar_t dir[MAX_PATH] = {};
                GetTempPathW(MAX_PATH, dir);
                tempCover = std::wstring(dir) + L"mediaaccess_cover_" + std::to_wstring(GetTickCount64()) + L".jpg";
                if (!WriteBytes(tempCover, image.data(), image.size(), true)) tempCover.clear();
            }
        }
        cd->SetSpeed(job.drive.bassIndex, static_cast<DWORD>(-1));   // fastest
        double secondsTotal = 0;
        for (const RipTrack& t : job.tracks) secondsTotal += t.seconds;
        double secondsBefore = 0;
        ULONGLONG startTick = GetTickCount64();
        for (size_t i = 0; i < job.tracks.size(); ++i) {
            {
                std::lock_guard<std::mutex> lock(g_progressMutex);
                g_progress.trackPos = static_cast<int>(i) + 1;
                g_progress.trackFraction = 0;
            }
            Post(notify, WM_CDRIP_TRACK_START, i + 1,
                 MAKELPARAM(job.tracks.size(), job.tracks[i].number));
            TrackResult r = RipOneTrack(cd, job, job.tracks[i], i, secondsBefore, secondsTotal,
                                        startTick, tempCover, notify);
            if (r != TrackResult::Done) {
                stopped = true;
                if (r == TrackResult::Cancelled) Post(notify, WM_CDRIP_CANCELLED, written.size(), 0);
                break;
            }
            written.push_back(&job.tracks[i]);
            secondsBefore += job.tracks[i].seconds;
        }
        cd->Release(job.drive.bassIndex);
        WriteLists(job, written);   // the tracks written, even after a stop
    }
    if (!tempCover.empty()) DeleteFileW(tempCover.c_str());
    if (!stopped) Post(notify, WM_CDRIP_DONE, written.size(), 0);
    g_running = false;
}

} // namespace

bool IsOpusAvailable() {
    return OpusStartFile() != nullptr;
}

const wchar_t* RipFormatExtension(RipFormat f) {
    switch (f) {
        case RipFormat::WAV:  return L".wav";
        case RipFormat::MP3:  return L".mp3";
        case RipFormat::OGG:  return L".ogg";
        case RipFormat::OPUS: return L".opus";
        default:              return L".flac";
    }
}

int RipQualityCount(RipFormat f) {
    int n = 0;
    Qualities(f, &n);
    return n;
}

int RipDefaultQuality(RipFormat) {
    return 0;   // every list starts with the plan's default
}

std::wstring RipQualityLabel(RipFormat f, int index) {
    int n = 0;
    const Quality* q = Qualities(f, &n);
    return (index >= 0 && index < n) ? std::wstring(T(q[index].label)) : std::wstring();
}

bool StartRip(const RipJob& job, HWND notify) {
    if (g_running.exchange(true)) return false;
    g_cancel = false;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_progress = RipProgress();
        g_progress.trackCount = static_cast<int>(job.tracks.size());
    }
    // Detached: a joinable std::thread left at exit would abort the process.
    // CancelRipAndWait waits on g_running instead.
    std::thread(Worker, job, notify).detach();
    return true;
}

void CancelRip() {
    g_cancel = true;
}

void CancelRipAndWait() {
    g_cancel = true;
    // A track stops within one read (about a second); 15 s covers a slow drive.
    for (int i = 0; i < 750 && g_running; ++i) Sleep(20);
}

bool IsRipping() {
    return g_running;
}

RipProgress GetRipProgress() {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    return g_progress;
}

} // namespace mediaaccess
