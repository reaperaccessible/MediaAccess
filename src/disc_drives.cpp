// disc_drives.cpp — see disc_drives.h.

#include "disc_drives.h"
#include "basscd_loader.h"
#include "logger.h"

namespace mediaaccess {

namespace {
std::wstring Widen(const char* s) {
    std::wstring out;
    if (!s) return out;
    while (*s == ' ') ++s;
    for (; *s; ++s) out += static_cast<wchar_t>(static_cast<unsigned char>(*s));
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// CD audio: 44100 Hz, 16-bit, stereo.
const double kBytesPerSecond = 176400.0;
} // namespace

int DiscContents::AudioTrackCount() const {
    int n = 0;
    for (const DiscTrack& t : tracks) if (!t.isData) ++n;
    return n;
}

double DiscContents::AudioSeconds() const {
    double s = 0.0;
    for (const DiscTrack& t : tracks) if (!t.isData) s += t.seconds;
    return s;
}

std::vector<DiscDrive> EnumerateOpticalDrives() {
    std::vector<DiscDrive> drives;
    const basscd::Api* cd = basscd::Get();
    if (!cd) return drives;
    BASS_CD_INFO info;
    for (DWORD i = 0; i < 26; ++i) {
        if (!cd->GetInfo(i, &info)) break;   // BASS_ERROR_DEVICE: no more drives
        DiscDrive d;
        d.bassIndex = i;
        d.letter = (info.letter >= 0 && info.letter < 26)
                   ? static_cast<wchar_t>(L'A' + info.letter) : L'?';
        std::wstring vendor = Widen(info.vendor);
        std::wstring product = Widen(info.product);
        d.vendorModel = vendor.empty() ? product : vendor + L" " + product;
        d.canOpen = info.canopen != FALSE;
        LogF("CD", "Drive %lu: %lc: %ls", i, d.letter, d.vendorModel.c_str());
        drives.push_back(d);
    }
    return drives;
}

DiscContents ReadDiscContents(const DiscDrive& drive) {
    DiscContents c;
    const basscd::Api* cd = basscd::Get();
    if (!cd || !cd->IsReady(drive.bassIndex)) return c;

    BASS_CD_TOC toc;
    ZeroMemory(&toc, sizeof(toc));
    if (!cd->GetTOC(drive.bassIndex, 0, &toc) || toc.last < toc.first) {
        c.type = DriveMediaType::Unknown;
        return c;
    }
    DWORD count = cd->GetTracks(drive.bassIndex);
    if (count == static_cast<DWORD>(-1)) return c;   // no disc

    for (DWORD i = 0; i < count && i < 99; ++i) {
        DiscTrack t;
        t.index = static_cast<int>(i);
        t.number = toc.tracks[i].track;
        t.isData = (toc.tracks[i].adrcon & BASS_CD_TOC_CON_DATA) != 0;
        DWORD bytes = cd->GetTrackLength(drive.bassIndex, i);
        if (bytes != static_cast<DWORD>(-1)) t.seconds = bytes / kBytesPerSecond;
        c.tracks.push_back(t);
    }
    c.type = c.AudioTrackCount() > 0 ? DriveMediaType::AudioCD : DriveMediaType::DataCD;
    return c;
}

bool IsDriveTrayOpen(const DiscDrive& drive) {
    const basscd::Api* cd = basscd::Get();
    return cd && cd->DoorIsOpen(drive.bassIndex);
}

bool ToggleDriveTray(const DiscDrive& drive, bool* nowOpen) {
    const basscd::Api* cd = basscd::Get();
    if (!cd || !drive.canOpen) return false;
    bool open = cd->DoorIsOpen(drive.bassIndex) != FALSE;
    if (!cd->Door(drive.bassIndex, open ? BASS_CD_DOOR_CLOSE : BASS_CD_DOOR_OPEN)) return false;
    if (nowOpen) *nowOpen = !open;
    return true;
}

void ReleaseDrive(const DiscDrive& drive) {
    const basscd::Api* cd = basscd::Get();
    if (cd) cd->Release(drive.bassIndex);
}

} // namespace mediaaccess
