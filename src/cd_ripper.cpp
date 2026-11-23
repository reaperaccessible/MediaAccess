// cd_ripper.cpp — see cd_ripper.h.

#include "cd_ripper.h"
#include "basscd_loader.h"
#include "translations.h"
#include "utils.h"
#include "logger.h"
#include "version.h"
#include "cd_verify.h"
#include "bassenc.h"
#include "bassenc_flac.h"
#include "bassenc_mp3.h"
#include "bassenc_ogg.h"
#include "loudness.h"
#include <cmath>
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

// Volume gains of a track (ReplayGain 2.0: reference -18 LUFS).
struct GainTags {
    bool   has = false;
    double trackLufs = 0, trackPeak = 0, albumLufs = 0, albumPeak = 0;
};

std::wstring Num(const wchar_t* fmt, double v) {
    wchar_t b[64];
    _snwprintf_s(b, _countof(b), _TRUNCATE, fmt, v);
    std::wstring s = b;
    for (wchar_t& c : s) if (c == L',') c = L'.';   // always a dot, whatever the locale
    return s;
}

std::wstring BuildOptions(const RipJob& job, const RipTrack& t, const std::wstring& coverPath,
                          const GainTags& g = GainTags()) {
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
            if (g.has) {
                add(L"-T", L"REPLAYGAIN_TRACK_GAIN=" + Num(L"%.2f dB", -18.0 - g.trackLufs));
                add(L"-T", L"REPLAYGAIN_TRACK_PEAK=" + Num(L"%.6f", g.trackPeak));
                add(L"-T", L"REPLAYGAIN_ALBUM_GAIN=" + Num(L"%.2f dB", -18.0 - g.albumLufs));
                add(L"-T", L"REPLAYGAIN_ALBUM_PEAK=" + Num(L"%.6f", g.albumPeak));
            }
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
            if (g.has) {
                add(L"--tv", L"TXXX=REPLAYGAIN_TRACK_GAIN=" + Num(L"%.2f dB", -18.0 - g.trackLufs));
                add(L"--tv", L"TXXX=REPLAYGAIN_TRACK_PEAK=" + Num(L"%.6f", g.trackPeak));
                add(L"--tv", L"TXXX=REPLAYGAIN_ALBUM_GAIN=" + Num(L"%.2f dB", -18.0 - g.albumLufs));
                add(L"--tv", L"TXXX=REPLAYGAIN_ALBUM_PEAK=" + Num(L"%.6f", g.albumPeak));
            }
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
            if (g.has) {
                add(L"-c", L"REPLAYGAIN_TRACK_GAIN=" + Num(L"%.2f dB", -18.0 - g.trackLufs));
                add(L"-c", L"REPLAYGAIN_TRACK_PEAK=" + Num(L"%.6f", g.trackPeak));
                add(L"-c", L"REPLAYGAIN_ALBUM_GAIN=" + Num(L"%.2f dB", -18.0 - g.albumLufs));
                add(L"-c", L"REPLAYGAIN_ALBUM_PEAK=" + Num(L"%.6f", g.albumPeak));
            }
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
            if (g.has) {
                // Opus: R128 gains, Q7.8 dB relative to -23 LUFS (RFC 7845).
                auto q78 = [](double lufs) { return std::to_wstring(static_cast<int>(lround((-23.0 - lufs) * 256.0))); };
                add(L"--comment", L"R128_TRACK_GAIN=" + q78(g.trackLufs));
                add(L"--comment", L"R128_ALBUM_GAIN=" + q78(g.albumLufs));
            }
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
                     const std::wstring& path, const std::wstring& coverPath,
                     const GainTags& gain = GainTags()) {
    const DWORD flags = BASS_ENCODE_AUTOFREE | BASS_UNICODE;
    if (job.format == RipFormat::WAV) return BASS_Encode_StartPCMFile(s, flags, path.c_str());
    std::wstring opts = BuildOptions(job, t, coverPath, gain);
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

struct TrackOutcome {
    uint32_t crcTest = 0, crcCopy = 0;   // CRC32 of the PCM (test pass: Secure mode only)
    int      suspicious = 0;             // 1-second blocks never read twice the same (Secure)
    ArCrc    ar;
    ArTrackResult arResult;
    bool     written = false;
    // Gain written (job.writeGain): the PCM waits in pcmTemp until the album
    // is read; its loudness was measured on the way.
    std::wstring pcmTemp;
    bool     measured = false;
    double   lufs = 0, peak = 0, seconds = 0;
};

bool Deferred(const RipJob& job) { return job.writeGain && job.format != RipFormat::WAV; }

void SetProgress(size_t pos, double trackFrac, double secondsBefore, double trackSeconds,
                 double secondsTotal, ULONGLONG startTick) {
    double doneSeconds = secondsBefore + trackFrac * trackSeconds;
    double totalFrac = secondsTotal > 0 ? doneSeconds / secondsTotal : 0.0;
    double elapsed = (GetTickCount64() - startTick) / 1000.0;
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_progress.trackPos = static_cast<int>(pos) + 1;
    g_progress.trackFraction = trackFrac;
    g_progress.totalFraction = totalFrac;
    g_progress.secondsLeft = (totalFrac > 0.02) ? elapsed * (1.0 - totalFrac) / totalFrac : -1.0;
}

// Reads the whole stream into `pcm` (Secure mode). onBytes(done, total).
template <typename F>
TrackResult ReadAll(HSTREAM s, std::vector<BYTE>& pcm, F onBytes) {
    QWORD total = BASS_ChannelGetLength(s, BASS_POS_BYTE);
    pcm.clear();
    if (total != static_cast<QWORD>(-1)) pcm.reserve(static_cast<size_t>(total));
    static BYTE buf[588 * 4 * 75];
    for (;;) {
        if (g_cancel) return TrackResult::Cancelled;
        DWORD got = BASS_ChannelGetData(s, buf, sizeof(buf));
        if (got == static_cast<DWORD>(-1)) {
            return BASS_ErrorGetCode() == BASS_ERROR_ENDED ? TrackResult::Done : TrackResult::Failed;
        }
        pcm.insert(pcm.end(), buf, buf + got);
        onBytes(pcm.size(), total);
    }
}

TrackResult RipOneTrack(const basscd::Api* cd, const RipJob& job, const RipTrack& t,
                        size_t pos, double secondsBefore, double secondsTotal,
                        ULONGLONG startTick, const std::wstring& coverPath, HWND notify,
                        TrackOutcome& out) {
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
    const QWORD total = BASS_ChannelGetLength(s, BASS_POS_BYTE);
    const bool firstTrack = (t.audioPos == 0), lastTrack = (t.audioPos == job.trackTotal - 1);
    ArAccumulator acc;
    acc.Begin(total == static_cast<QWORD>(-1) ? 0 : total / 4, firstTrack, lastTrack);
    static BYTE buf[588 * 4 * 75];   // 75 CD frames = 1 second
    TrackResult result = TrackResult::Done;
    HSTREAM source = s;              // the stream the encoder reads
    std::vector<BYTE> pcm;

    if (job.secure) {
        // Test pass, then copy pass on the SAME stream (BASSCD keeps one per
        // drive), compared block by block. A block read differently is read
        // again until two reads agree (8 at most); a block that never agrees is
        // "suspicious" and the first read is kept. The cache of the drive is not
        // bypassed reliably: AccurateRip remains the proof.
        std::vector<BYTE> second;
        auto progress = [&](double base) {
            return [&, base](size_t done, QWORD tot) {
                double f = (tot && tot != static_cast<QWORD>(-1)) ? double(done) / double(tot) : 0.0;
                SetProgress(pos, base + f / 2.0, secondsBefore, t.seconds, secondsTotal, startTick);
            };
        };
        result = ReadAll(s, pcm, progress(0.0));
        if (result == TrackResult::Done) {
            out.crcTest = Crc32(0, pcm.data(), pcm.size());
            BASS_ChannelSetPosition(s, 0, BASS_POS_BYTE);
            result = ReadAll(s, second, progress(0.5));
        }
        if (result == TrackResult::Done) {
            const size_t block = sizeof(buf);
            size_t n = (std::min)(pcm.size(), second.size());
            for (size_t at = 0; at < n && !g_cancel; at += block) {
                size_t len = (std::min)(block, n - at);
                if (memcmp(&pcm[at], &second[at], len) == 0) continue;
                std::vector<std::vector<BYTE>> reads = {
                    std::vector<BYTE>(pcm.begin() + at, pcm.begin() + at + len),
                    std::vector<BYTE>(second.begin() + at, second.begin() + at + len) };
                bool agreed = false;
                for (int tries = 0; tries < 8 && !agreed && !g_cancel; ++tries) {
                    std::vector<BYTE> again(len);
                    if (!BASS_ChannelSetPosition(s, at, BASS_POS_BYTE)) break;
                    DWORD got = BASS_ChannelGetData(s, again.data(), static_cast<DWORD>(len));
                    if (got != len) continue;
                    for (const std::vector<BYTE>& r : reads) {
                        if (r == again) { memcpy(&pcm[at], again.data(), len); agreed = true; break; }
                    }
                    reads.push_back(again);
                }
                if (!agreed) ++out.suspicious;
            }
            if (g_cancel) result = TrackResult::Cancelled;
        }
        BASS_StreamFree(s);
        s = 0;
        source = 0;
        if (result == TrackResult::Done) {
            // The checked PCM goes to the encoder through a push stream.
            source = BASS_StreamCreate(44100, 2, BASS_STREAM_DECODE, STREAMPROC_PUSH, nullptr);
            if (!source) result = TrackResult::Failed;
        }
        if (result == TrackResult::Failed) {
            PostError(notify, FmtInt(T("Reading track %d failed. The disc may have been removed."), t.number));
        }
        if (result != TrackResult::Done) {
            if (source) BASS_StreamFree(source);
            return result;
        }
    }

    const bool deferred = Deferred(job);
    HANDLE pcmFile = INVALID_HANDLE_VALUE;
    mediaaccess::LoudnessMeter meter;
    std::vector<float> fbuf;
    const std::wstring pcmPath = finalPath + L".pcm.part";
    if (deferred) {
        pcmFile = CreateFileW(pcmPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (pcmFile == INVALID_HANDLE_VALUE) {
            LogF("CDRIP", "cannot create %ls: %lu", pcmPath.c_str(), GetLastError());
            BASS_StreamFree(source);
            PostError(notify, Fmt(T("Cannot write to the folder %ls. Choose another folder."), job.folder));
            return TrackResult::Failed;
        }
        meter.Begin(44100, 2);
    } else if (!StartEncoder(source, job, t, partPath, coverPath)) {
        int err = BASS_ErrorGetCode();
        LogF("CDRIP", "encoder start failed for %ls: error %d", partPath.c_str(), err);
        BASS_StreamFree(source);
        PostError(notify, Fmt(T("Cannot write to the folder %ls. Choose another folder."), job.folder));
        return TrackResult::Failed;
    }

    uint32_t crc = 0;
    size_t fed = 0;
    QWORD done = 0;
    for (;;) {
        if (g_cancel) { result = TrackResult::Cancelled; break; }
        if (job.secure) {
            // Feed one block, then pull it through the encoder.
            if (fed < pcm.size()) {
                size_t len = (std::min)(sizeof(buf), pcm.size() - fed);
                BASS_StreamPutData(source, &pcm[fed], static_cast<DWORD>(len));
                fed += len;
                if (fed == pcm.size()) BASS_StreamPutData(source, nullptr, BASS_STREAMPROC_END);
            }
        }
        DWORD got = BASS_ChannelGetData(source, buf, sizeof(buf));
        if (got == static_cast<DWORD>(-1)) {
            int err = BASS_ErrorGetCode();
            if (err != BASS_ERROR_ENDED) {
                LogF("CDRIP", "read error on track %d: %d", t.index, err);
                PostError(notify, FmtInt(T("Reading track %d failed. The disc may have been removed."), t.number));
                result = TrackResult::Failed;
            }
            break;
        }
        if (deferred) {
            DWORD wrote = 0;
            if (!WriteFile(pcmFile, buf, got, &wrote, nullptr) || wrote != got) {
                LogF("CDRIP", "write failed %ls: %lu", pcmPath.c_str(), GetLastError());
                PostError(notify, Fmt(T("Writing to %ls failed. The disk may be full."), job.folder));
                result = TrackResult::Failed;
                break;
            }
            const short* pcm16 = reinterpret_cast<const short*>(buf);
            fbuf.resize(got / 2);
            for (size_t k = 0; k < fbuf.size(); ++k) fbuf[k] = pcm16[k] / 32768.0f;
            meter.Add(fbuf.data(), fbuf.size() / 2);
        }
        // The encoder stops by itself when it cannot write (disk full...).
        if (!deferred && !BASS_Encode_IsActive(source)) {
            LogF("CDRIP", "encoder stopped while writing %ls", partPath.c_str());
            PostError(notify, Fmt(T("Writing to %ls failed. The disk may be full."), job.folder));
            result = TrackResult::Failed;
            break;
        }
        crc = Crc32(crc, buf, got);
        acc.Add(buf, got);
        done += got;
        if (!job.secure) {
            double f = (total && total != static_cast<QWORD>(-1)) ? double(done) / double(total) : 0.0;
            SetProgress(pos, f, secondsBefore, t.seconds, secondsTotal, startTick);
        }
    }

    if (deferred) {
        CloseHandle(pcmFile);
        BASS_StreamFree(source);
        if (result != TrackResult::Done) {
            DeleteFileW(pcmPath.c_str());
            return result;
        }
        out.crcCopy = crc;
        out.ar = acc.Result();
        out.pcmTemp = pcmPath;
        out.measured = meter.Result(out.lufs, out.peak, out.seconds);
        LogF("CDRIP", "track %d read, %.2f LUFS, peak %.4f", t.index, out.lufs, out.peak);
        return TrackResult::Done;   // encoded once the album is read
    }
    BASS_Encode_Stop(source);   // flushes and closes the file
    BASS_StreamFree(source);
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
    out.crcCopy = crc;
    out.ar = acc.Result();
    out.written = true;
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

std::wstring Hex8(uint32_t v) {
    wchar_t b[16];
    _snwprintf_s(b, _countof(b), _TRUNCATE, L"%08X", v);
    return b;
}

// The extraction log: English, UTF-8 with BOM, a standard technical format.
void WriteLog(const RipJob& job, const std::wstring& path, const std::wstring& offsetSource,
              ArDbState db, size_t pressings, const ArDiscIds& ids,
              const std::vector<TrackOutcome>& outcomes, bool stopped) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[512];
    std::wstring L = L"MediaAccess " + Utf8ToWide(APP_VERSION) + L" - audio CD extraction log\r\n";
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"Date: %04d-%02d-%02d %02d:%02d\r\n\r\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    L += line;
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"Drive: %ls (%lc:)\r\n", job.drive.vendorModel.c_str(), job.drive.letter);
    L += line;
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"Read offset: %+d samples (%ls)\r\n", job.readOffset, offsetSource.c_str());
    L += line;
    L += std::wstring(L"Mode: ") + (job.secure ? L"Secure (two reads compared, blocks re-read until two agree)" : L"Fast (one read)") + L"\r\n";
    L += L"Format: " + RipFormatNameEn(job.format) + L", " + RipQualityLabelEn(job.format, job.quality) + L"\r\n";
    L += L"Metadata: " + (job.meta.source.empty() ? std::wstring(L"none") : job.meta.source) + L"\r\n";
    L += L"Album: " + job.meta.albumArtist + L" - " + job.meta.album +
         (job.meta.year.empty() ? L"" : L" (" + job.meta.year + L")") + L"\r\n";
    if (!job.discId.empty()) L += L"MusicBrainz disc ID: " + job.discId + L"\r\n";
    if (ids.ok) {
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"AccurateRip disc ID: %03d-%08x-%08x-%08x\r\n",
                     ids.audioTracks, ids.id1, ids.id2, ids.cddb);
        L += line;
    }
    switch (db) {
        case ArDbState::Found:
            _snwprintf_s(line, _countof(line), _TRUNCATE, L"AccurateRip: %u pressing(s) in the database\r\n", (unsigned)pressings);
            L += line;
            break;
        case ArDbState::NotInDb:     L += L"AccurateRip: disc not in the database\r\n"; break;
        case ArDbState::Unreachable: L += L"AccurateRip: database unreachable\r\n"; break;
        default:                     L += L"AccurateRip: not checked\r\n"; break;
    }
    L += L"\r\n";

    int accurate = 0, notAccurate = 0, suspicious = 0, writtenCount = 0;
    std::wstring problems;
    for (size_t i = 0; i < job.tracks.size() && i < outcomes.size(); ++i) {
        const RipTrack& t = job.tracks[i];
        const TrackOutcome& o = outcomes[i];
        if (!o.written) continue;
        ++writtenCount;
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"Track %02d  %ls\r\n", t.number, t.title.c_str());
        L += line;
        L += L"  File: " + t.fileName + RipFormatExtension(job.format) + L"\r\n";
        if (job.secure) L += L"  Test CRC32: " + Hex8(o.crcTest) + L"\r\n";
        L += L"  Copy CRC32: " + Hex8(o.crcCopy) + L"\r\n";
        if (job.secure) {
            _snwprintf_s(line, _countof(line), _TRUNCATE, L"  Suspicious blocks: %d\r\n", o.suspicious);
            L += line;
            if (o.suspicious > 0) { ++suspicious; problems += L"  Track " + std::to_wstring(t.number) + L": suspicious blocks\r\n"; }
        }
        switch (o.arResult.status) {
            case ArStatus::Accurate:
                _snwprintf_s(line, _countof(line), _TRUNCATE, L"  AccurateRip: accurate (confidence %d), v1 %08X, v2 %08X\r\n",
                             o.arResult.confidence, o.ar.v1, o.ar.v2);
                ++accurate;
                break;
            case ArStatus::NotAccurate:
                _snwprintf_s(line, _countof(line), _TRUNCATE, L"  AccurateRip: NOT accurate, v1 %08X, v2 %08X\r\n", o.ar.v1, o.ar.v2);
                ++notAccurate;
                problems += L"  Track " + std::to_wstring(t.number) + L": not accurate\r\n";
                break;
            case ArStatus::NotInDb:
                _snwprintf_s(line, _countof(line), _TRUNCATE, L"  AccurateRip: not in the database, v1 %08X, v2 %08X\r\n", o.ar.v1, o.ar.v2);
                break;
            default:
                _snwprintf_s(line, _countof(line), _TRUNCATE, L"  AccurateRip: not verified, v1 %08X, v2 %08X\r\n", o.ar.v1, o.ar.v2);
                break;
        }
        L += line;
        L += L"\r\n";
    }
    if (stopped) L += L"Extraction stopped before the end.\r\n";
    if (db == ArDbState::Found && notAccurate == 0 && suspicious == 0 && writtenCount > 0) {
        L += L"All tracks accurate\r\n";
    } else if (!problems.empty()) {
        L += L"Problems:\r\n" + problems;
    } else if (db != ArDbState::Found) {
        L += L"Not verified with AccurateRip\r\n";
    }
    WriteUtf8(path, L, true);
}

// Second pass (job.writeGain): a track kept as PCM is encoded with its gains,
// through a push stream, as the Secure mode does. False on a failure (said).
bool EncodeDeferred(const RipJob& job, const RipTrack& t, TrackOutcome& out,
                    const std::wstring& coverPath, const GainTags& gain, HWND notify) {
    std::wstring finalPath = job.folder + L"\\" + t.fileName + RipFormatExtension(job.format);
    std::wstring partPath = finalPath + L".part";
    HANDLE f = CreateFileW(out.pcmTemp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    HSTREAM source = (f != INVALID_HANDLE_VALUE)
        ? BASS_StreamCreate(44100, 2, BASS_STREAM_DECODE, STREAMPROC_PUSH, nullptr) : 0;
    bool ok = source && StartEncoder(source, job, t, partPath, coverPath, gain);
    static BYTE buf[588 * 4 * 75];
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(f, buf, sizeof(buf), &read, nullptr)) { ok = false; break; }
        if (read) BASS_StreamPutData(source, buf, read);
        else BASS_StreamPutData(source, nullptr, BASS_STREAMPROC_END);
        for (;;) {   // pull what was pushed through the encoder
            DWORD got = BASS_ChannelGetData(source, buf, sizeof(buf));
            if (got == static_cast<DWORD>(-1) || got == 0) break;
        }
        if (!BASS_Encode_IsActive(source)) { ok = false; break; }
        if (!read) break;
    }
    if (source) {
        BASS_Encode_Stop(source);
        BASS_StreamFree(source);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    DeleteFileW(out.pcmTemp.c_str());
    out.pcmTemp.clear();
    if (ok && !MoveFileExW(partPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) ok = false;
    if (!ok) {
        LogF("CDRIP", "encoding failed for %ls: error %d", partPath.c_str(), BASS_ErrorGetCode());
        DeleteFileW(partPath.c_str());
        PostError(notify, Fmt(T("Writing to %ls failed. The disk may be full."), job.folder));
        return false;
    }
    out.written = true;
    LogF("CDRIP", "track %d written: %ls", t.index, finalPath.c_str());
    return true;
}

void Worker(RipJob job, HWND notify) {
    const basscd::Api* cd = basscd::Get();
    std::vector<const RipTrack*> written;
    std::vector<TrackOutcome> outcomes(job.tracks.size());
    bool stopped = false;
    std::wstring tempCover;
    RipSummary* summary = new RipSummary();
    ArDiscIds ids;
    ArDbState db = ArDbState::NotChecked;
    std::vector<ArPressing> pressings;

    int mk = cd ? SHCreateDirectoryExW(nullptr, job.folder.c_str(), nullptr) : ERROR_INVALID_FUNCTION;
    if (!cd || (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS)) {
        LogF("CDRIP", "cannot create %ls: %d", job.folder.c_str(), mk);
        PostError(notify, Fmt(T("Cannot create the folder %ls. Choose another folder."), job.folder));
        stopped = true;
    } else {
        // The read offset of this drive: saved (detected or typed), else
        // AccurateRip's list (then remembered), else 0.
        std::wstring offsetSource = L"saved";
        if (!job.offsetKnown) {
            bool detected = false;
            int o = 0;
            if (SavedDriveOffset(job.drive.vendorModel, &o, &detected)) {
                job.readOffset = o;
                offsetSource = detected ? L"detected with AccurateRip" : L"typed by the user";
            } else if (ListedDriveOffset(job.drive.vendorModel, &o)) {
                job.readOffset = o;
                offsetSource = L"AccurateRip drive list";
                SaveDriveOffset(job.drive.vendorModel, o, false);
            } else {
                job.readOffset = 0;
                offsetSource = L"unknown drive, 0 used";
            }
        } else {
            offsetSource = L"typed by the user";
        }
        if (job.verify) {
            ids = ComputeArIds(job.drive);
            db = FetchAccurateRip(ids, pressings);
        }
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
        cd->SetOffset(job.drive.bassIndex, job.readOffset);
        // Secure mode: a read error is an error, read again 5 times; Fast
        // mode: BASSCD's defaults (unreadable frames become silence, 2 retries).
        BASS_SetConfig(BASS_CONFIG_CD_SKIPERROR, job.secure ? FALSE : TRUE);
        BASS_SetConfig(BASS_CONFIG_CD_RETRY, job.secure ? 5 : 2);
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
                                        startTick, tempCover, notify, outcomes[i]);
            if (r != TrackResult::Done) {
                stopped = true;
                if (r == TrackResult::Cancelled) Post(notify, WM_CDRIP_CANCELLED, written.size(), 0);
                break;
            }
            TrackOutcome& o = outcomes[i];
            if (job.verify) o.arResult = MatchTrack(pressings, job.tracks[i].audioPos, o.ar);
            if (db != ArDbState::Found && db != ArDbState::NotInDb) o.arResult.status = ArStatus::NotChecked;
            if (o.suspicious > 0) Post(notify, WM_CDRIP_TRACK_DONE, job.tracks[i].number, o.suspicious);
            written.push_back(&job.tracks[i]);
            secondsBefore += job.tracks[i].seconds;
        }
        cd->SetOffset(job.drive.bassIndex, 0);
        BASS_SetConfig(BASS_CONFIG_CD_SKIPERROR, TRUE);
        BASS_SetConfig(BASS_CONFIG_CD_RETRY, 2);
        cd->Release(job.drive.bassIndex);
        if (Deferred(job)) {
            // The album gain: energy average of the tracks read, by duration;
            // its peak: the highest. Then each track is encoded with its gains
            // (also the tracks read before a stop: they are kept, as always).
            double energy = 0, seconds = 0, albumPeak = 0;
            for (size_t i = 0; i < outcomes.size(); ++i) {
                const TrackOutcome& o = outcomes[i];
                if (o.pcmTemp.empty() || !o.measured || o.seconds <= 0) continue;
                energy += o.seconds * pow(10.0, o.lufs / 10.0);
                seconds += o.seconds;
                albumPeak = (std::max)(albumPeak, o.peak);
            }
            const double albumLufs = seconds > 0 ? 10.0 * log10(energy / seconds) : 0.0;
            std::vector<const RipTrack*> encoded;
            for (size_t i = 0; i < job.tracks.size(); ++i) {
                TrackOutcome& o = outcomes[i];
                if (o.pcmTemp.empty()) continue;
                GainTags g;
                g.has = o.measured && seconds > 0;
                g.trackLufs = o.lufs;
                g.trackPeak = o.peak;
                g.albumLufs = albumLufs;
                g.albumPeak = albumPeak;
                if (EncodeDeferred(job, job.tracks[i], o, tempCover, g, notify)) {
                    encoded.push_back(&job.tracks[i]);
                } else {
                    stopped = true;
                    for (size_t k = i + 1; k < outcomes.size(); ++k) {   // nothing half done is left
                        if (!outcomes[k].pcmTemp.empty()) DeleteFileW(outcomes[k].pcmTemp.c_str());
                        outcomes[k].pcmTemp.clear();
                    }
                    break;
                }
            }
            written = encoded;
            LogF("CDRIP", "album gain %.2f dB over %.0f s", -18.0 - albumLufs, seconds);
        }
        WriteLists(job, written);   // the tracks written, even after a stop
        if (job.writeLog && !written.empty() && !job.listName.empty()) {
            summary->logPath = job.folder + L"\\" + job.listName + L".log";
            WriteLog(job, summary->logPath, offsetSource, db, pressings.size(), ids, outcomes, stopped);
        }
    }
    if (!tempCover.empty()) DeleteFileW(tempCover.c_str());

    summary->written = static_cast<int>(written.size());
    summary->verified = (db == ArDbState::Found);
    summary->notInDb = (db == ArDbState::NotInDb);
    summary->folder = job.folder;
    for (size_t i = 0; i < outcomes.size(); ++i) {
        if (!outcomes[i].written) continue;
        if (outcomes[i].arResult.status == ArStatus::Accurate) ++summary->accurate;
        if (outcomes[i].arResult.status == ArStatus::NotAccurate) summary->notAccurate.push_back(job.tracks[i].number);
        if (outcomes[i].suspicious > 0) summary->suspicious.push_back(job.tracks[i].number);
    }
    if (stopped || !IsWindow(notify) || !PostMessageW(notify, WM_CDRIP_DONE, written.size(), reinterpret_cast<LPARAM>(summary))) {
        // The window keeps the folder and the log path even after a stop.
        if (stopped && IsWindow(notify) && PostMessageW(notify, WM_CDRIP_SUMMARY, 0, reinterpret_cast<LPARAM>(summary))) {
            summary = nullptr;
        }
        delete summary;
    }
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

std::wstring RipQualityLabelEn(RipFormat f, int index) {
    int n = 0;
    const Quality* q = Qualities(f, &n);
    return (index >= 0 && index < n) ? Utf8ToWide(q[index].label) : std::wstring();
}

std::wstring RipFormatNameEn(RipFormat f) {
    switch (f) {
        case RipFormat::WAV:  return L"WAV";
        case RipFormat::MP3:  return L"MP3";
        case RipFormat::OGG:  return L"Ogg Vorbis";
        case RipFormat::OPUS: return L"Opus";
        default:              return L"FLAC";
    }
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
