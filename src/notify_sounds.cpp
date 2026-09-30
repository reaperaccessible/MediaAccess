// notify_sounds.cpp — v2.74 download-finished sounds (GitHub issue #16).
//
// A short sound when a user download (YouTube, podcasts) succeeds or fails,
// for people who turn their screen reader's speech off while listening. The
// sound is a separate BASS stream on the current output, so it mixes with
// whatever plays: playback is never paused, and MediaAccess's own volume,
// effects and mute (all applied per stream) do not touch it.

#include "mediaaccess/notify_sounds.h"
#include "mediaaccess/globals.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/utils.h"
#include "bass.h"

#include <windows.h>

namespace {

constexpr double kMaxSeconds = 5.0;   // a notification, not a song
volatile LONG g_sound = 0;            // the one playing (HSTREAM), 0 when none

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// BASS frees an AUTOFREE stream when it ends, is stopped, or when the device
// is freed (output change) — on a BASS thread. Forget the handle then, but
// only if it is still the current one (a newer sound may have replaced it).
void CALLBACK OnSoundFreed(HSYNC, DWORD channel, DWORD, void*) {
    InterlockedCompareExchange(&g_sound, 0, (LONG)channel);
}

// Long file: fade out over 50 ms and stop (-1 = stop when the slide ends).
void CALLBACK OnSoundTooLong(HSYNC, DWORD channel, DWORD, void*) {
    BASS_ChannelSlideAttribute(channel, BASS_ATTRIB_VOL, -1.0f, 50);
}

HSTREAM OpenSound(const std::wstring& file) {
    return BASS_StreamCreateFile(FALSE, file.c_str(), 0, 0,
                                 BASS_UNICODE | BASS_STREAM_AUTOFREE);
}

}  // namespace

std::wstring BuiltinDownloadSoundPath(bool success) {
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir + (success ? L"\\sounds\\download_success.wav" : L"\\sounds\\download_failure.wav");
}

bool PlayNotifySoundFile(const std::wstring& path, bool success) {
    const char* kind = success ? "success" : "failure";
    bool chosenUsable = true;
    std::wstring file = path;
    if (!file.empty() && !FileExists(file)) {
        Log("SOUND", L"chosen sound missing, using the built-in one: " + file);
        file.clear();
        chosenUsable = false;
    }
    if (file.empty()) file = BuiltinDownloadSoundPath(success);

    // A new sound replaces the previous one.
    HSTREAM prev = (HSTREAM)InterlockedExchange(&g_sound, 0);
    if (prev) BASS_ChannelFree(prev);

    HSTREAM s = OpenSound(file);
    if (!s && !path.empty() && chosenUsable) {
        // The chosen file exists but BASS cannot read it (unknown format,
        // damaged): the built-in sound instead of silence.
        LogF("SOUND", "chosen %s sound unreadable (BASS error %d), using the built-in one",
             kind, BASS_ErrorGetCode());
        Log("SOUND", L"  file: " + file);
        chosenUsable = false;
        file = BuiltinDownloadSoundPath(success);
        s = OpenSound(file);
    }
    if (!s) {
        LogF("SOUND", "download %s sound: cannot open (BASS error %d, device %lu)",
             kind, BASS_ErrorGetCode(), (unsigned long)BASS_GetDevice());
        return false;
    }
    BASS_ChannelSetSync(s, BASS_SYNC_FREE, 0, OnSoundFreed, nullptr);
    QWORD len = BASS_ChannelGetLength(s, BASS_POS_BYTE);
    QWORD cut = BASS_ChannelSeconds2Bytes(s, kMaxSeconds);
    if (len != (QWORD)-1 && len > cut)
        BASS_ChannelSetSync(s, BASS_SYNC_POS, cut, OnSoundTooLong, nullptr);
    InterlockedExchange(&g_sound, (LONG)s);
    if (!BASS_ChannelPlay(s, FALSE)) {
        LogF("SOUND", "download %s sound: cannot play (BASS error %d)", kind, BASS_ErrorGetCode());
        InterlockedCompareExchange(&g_sound, 0, (LONG)s);
        BASS_StreamFree(s);
        return false;
    }
    DWORD dev = BASS_GetDevice();
    Log("SOUND", std::wstring(L"download ") + (success ? L"success" : L"failure") +
                 L" sound played (" + file + L")" +
                 (dev == 0 ? L" on the \"no sound\" output" : L""));
    return path.empty() || chosenUsable;
}

void PlayDownloadSound(bool success) {
    if (!g_downloadSoundOnFinish) return;
    PlayNotifySoundFile(success ? g_downloadSoundSuccess : g_downloadSoundFailure, success);
}
