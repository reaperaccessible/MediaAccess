#pragma once
// cd_verify — AccurateRip (docs/PLAN_EXTRACTION_CD.md, section 4.4): the
// disc IDs, the database answer, the track checksums (v1 and v2), the drive
// read offset (official list, saved value, automatic detection).
//
// Checksums: each stereo 16-bit sample is one little-endian 32-bit word s[i],
// at position p = i + 1 in the track; v1 = sum(s*p) mod 2^32, v2 = v1 + the
// sum of the high halves of the 64-bit products. The first track skips its
// first 2939 samples, the last audio track its last 2940.

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "disc_drives.h"

namespace mediaaccess {

struct ArCrc { uint32_t v1 = 0, v2 = 0; };

class ArAccumulator {
public:
    // totalSamples: length of the track; first / last: exclusions apply.
    void Begin(uint64_t totalSamples, bool first, bool last);
    void Add(const BYTE* pcm, size_t bytes);   // 16-bit stereo, in order
    ArCrc Result() const { return { lo_, lo_ + hi_ }; }
private:
    uint64_t pos_ = 0, total_ = 0, skipHead_ = 0, skipTail_ = 0;
    uint32_t lo_ = 0, hi_ = 0;
    BYTE     pending_[4] = {};
    size_t   pendingLen_ = 0;
};

uint32_t Crc32(uint32_t crc, const BYTE* data, size_t len);   // start with 0

struct ArDiscIds {
    uint32_t id1 = 0, id2 = 0, cddb = 0;
    int      audioTracks = 0;
    bool     ok = false;
};
ArDiscIds ComputeArIds(const DiscDrive& drive);

struct ArEntry { uint8_t confidence; uint32_t crc; uint32_t crc450; };
struct ArPressing { std::vector<ArEntry> tracks; };   // one per audio track

enum class ArDbState { NotChecked, NotInDb, Found, Unreachable };
ArDbState FetchAccurateRip(const ArDiscIds& ids, std::vector<ArPressing>& out);   // blocking

enum class ArStatus { NotChecked, NotInDb, Accurate, NotAccurate };
struct ArTrackResult { ArStatus status = ArStatus::NotChecked; int confidence = 0; };
// pos: 0-based position among the audio tracks.
ArTrackResult MatchTrack(const std::vector<ArPressing>& db, int pos, const ArCrc& crc);

// ---- Drive read offset (samples; the value given to BASS_CD_SetOffset) ----

// Saved for this drive model (detected or typed), else none.
bool SavedDriveOffset(const std::wstring& model, int* offset, bool* detected);
void SaveDriveOffset(const std::wstring& model, int offset, bool detected);
void ForgetDriveOffset(const std::wstring& model);   // next extraction: official list again
// AccurateRip's official list (DriveOffsets.bin, read once). Blocking.
bool ListedDriveOffset(const std::wstring& vendorModel, int* offset);

// Reads a middle track with a margin, slides the window over +/- 5880 samples
// and keeps the offset whose v1 checksum is in the database; confirmed on a
// second track. Needs a disc known to AccurateRip. Blocking (about three
// track reads); `cancel` is polled.
enum class OffsetDetect { Found, NotInDb, TooFewTracks, NoMatch, ReadError, Cancelled };
OffsetDetect DetectReadOffset(const DiscDrive& drive, int* offset, const volatile bool* cancel);

} // namespace mediaaccess
