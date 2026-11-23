// cd_ripper.cpp — see cd_ripper.h.

#include "cd_ripper.h"
#include "basscd_loader.h"
#include "translations.h"
#include "logger.h"
#include "bassenc.h"
#include "bassenc_flac.h"
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
HENCODE StartEncoder(HSTREAM s, RipFormat format, const std::wstring& path) {
    const DWORD flags = BASS_ENCODE_AUTOFREE | BASS_UNICODE;
    if (format == RipFormat::WAV) {
        return BASS_Encode_StartPCMFile(s, flags, path.c_str());
    }
    // FLAC level 5 (the flac tool's default): lossless, the usual size/speed trade.
    return BASS_Encode_FLAC_StartFile(s, L"-5", flags, path.c_str());
}

enum class TrackResult { Done, Cancelled, Failed };

TrackResult RipOneTrack(const basscd::Api* cd, const RipJob& job, const RipTrack& t,
                        size_t pos, double secondsBefore, double secondsTotal,
                        ULONGLONG startTick, HWND notify) {
    const wchar_t* ext = (job.format == RipFormat::WAV) ? L".wav" : L".flac";
    std::wstring finalPath = job.folder + L"\\" + t.fileName + ext;
    std::wstring partPath = finalPath + L".part";

    HSTREAM s = cd->StreamCreate(job.drive.bassIndex, static_cast<DWORD>(t.index), BASS_STREAM_DECODE);
    if (!s) {
        LogF("CDRIP", "StreamCreate track %d failed: error %d", t.index, BASS_ErrorGetCode());
        PostError(notify, FmtInt(T("Track %d cannot be read. Check that the disc is in the drive."), t.number));
        return TrackResult::Failed;
    }
    if (!StartEncoder(s, job.format, partPath)) {
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

void Worker(RipJob job, HWND notify) {
    const basscd::Api* cd = basscd::Get();
    int written = 0;
    bool stopped = false;

    int mk = cd ? SHCreateDirectoryExW(nullptr, job.folder.c_str(), nullptr) : ERROR_INVALID_FUNCTION;
    if (!cd || (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS && mk != ERROR_FILE_EXISTS)) {
        LogF("CDRIP", "cannot create %ls: %d", job.folder.c_str(), mk);
        PostError(notify, Fmt(T("Cannot create the folder %ls. Choose another folder."), job.folder));
        stopped = true;
    } else {
        // The cover first (a few seconds at most), so it is there even if the
        // extraction is stopped. Never replaces a folder.jpg already present.
        std::wstring cover = job.folder + L"\\folder.jpg";
        std::vector<BYTE> image;
        if (job.meta.hasFrontCover && GetFileAttributesW(cover.c_str()) == INVALID_FILE_ATTRIBUTES &&
            DownloadFrontCover(job.meta, image)) {
            HANDLE f = CreateFileW(cover.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(f, image.data(), (DWORD)image.size(), &written, nullptr);
                CloseHandle(f);
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
            TrackResult r = RipOneTrack(cd, job, job.tracks[i], i, secondsBefore, secondsTotal, startTick, notify);
            if (r != TrackResult::Done) {
                stopped = true;
                if (r == TrackResult::Cancelled) Post(notify, WM_CDRIP_CANCELLED, written, 0);
                break;
            }
            ++written;
            secondsBefore += job.tracks[i].seconds;
        }
        cd->Release(job.drive.bassIndex);
    }
    if (!stopped) Post(notify, WM_CDRIP_DONE, written, 0);
    g_running = false;
}

} // namespace

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
