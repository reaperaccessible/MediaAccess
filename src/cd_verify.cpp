// cd_verify.cpp — see cd_verify.h.

#include "cd_verify.h"
#include "cd_metadata.h"
#include "basscd_loader.h"
#include "database.h"
#include "utils.h"
#include "logger.h"
#include <mutex>
#include <cwctype>

namespace mediaaccess {

// ---- Checksums ----

void ArAccumulator::Begin(uint64_t totalSamples, bool first, bool last) {
    pos_ = 0;
    total_ = totalSamples;
    skipHead_ = first ? 2939 : 0;           // 5 * 588 - 1
    skipTail_ = last ? 2940 : 0;            // 5 * 588
    lo_ = hi_ = 0;
    pendingLen_ = 0;
}

void ArAccumulator::Add(const BYTE* pcm, size_t bytes) {
    const uint64_t lastCounted = (total_ > skipTail_) ? total_ - skipTail_ : 0;
    auto take = [this, lastCounted](uint32_t s) {
        ++pos_;
        if (pos_ > skipHead_ && pos_ <= lastCounted) {
            uint64_t prod = static_cast<uint64_t>(s) * pos_;
            lo_ += static_cast<uint32_t>(prod);
            hi_ += static_cast<uint32_t>(prod >> 32);
        }
    };
    size_t i = 0;
    while (pendingLen_ > 0 && pendingLen_ < 4 && i < bytes) pending_[pendingLen_++] = pcm[i++];
    if (pendingLen_ == 4) {
        take(pending_[0] | (pending_[1] << 8) | (pending_[2] << 16) | (static_cast<uint32_t>(pending_[3]) << 24));
        pendingLen_ = 0;
    }
    for (; i + 4 <= bytes; i += 4) {
        take(pcm[i] | (pcm[i + 1] << 8) | (pcm[i + 2] << 16) | (static_cast<uint32_t>(pcm[i + 3]) << 24));
    }
    while (i < bytes) pending_[pendingLen_++] = pcm[i++];
}

uint32_t Crc32(uint32_t crc, const BYTE* data, size_t len) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, []() {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    });
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// ---- Disc IDs and database ----

ArDiscIds ComputeArIds(const DiscDrive& drive) {
    ArDiscIds ids;
    const basscd::Api* cd = basscd::Get();
    BASS_CD_TOC toc;
    ZeroMemory(&toc, sizeof(toc));
    if (!cd || !cd->GetTOC(drive.bassIndex, 0, &toc) || toc.last < toc.first) return ids;
    int count = toc.last - toc.first + 1;
    uint32_t leadout = toc.tracks[count].lba;   // after the last track, data track included
    int k = 0;
    for (int i = 0; i < count; ++i) {
        if (toc.tracks[i].adrcon & BASS_CD_TOC_CON_DATA) continue;
        ++k;
        uint32_t lba = toc.tracks[i].lba;
        ids.id1 += lba;
        ids.id2 += (lba > 1 ? lba : 1) * static_cast<uint32_t>(k);
    }
    ids.audioTracks = k;
    ids.id1 += leadout;
    ids.id2 += (leadout > 1 ? leadout : 1) * static_cast<uint32_t>(k + 1);
    const char* cddb = cd->GetID(drive.bassIndex, BASS_CDID_CDDB);   // "850b500b 11 150 ..."
    if (cddb) ids.cddb = static_cast<uint32_t>(strtoul(cddb, nullptr, 16));
    ids.ok = k > 0 && cddb != nullptr;
    return ids;
}

ArDbState FetchAccurateRip(const ArDiscIds& ids, std::vector<ArPressing>& out) {
    out.clear();
    if (!ids.ok) return ArDbState::NotChecked;
    wchar_t url[256];
    _snwprintf_s(url, _countof(url), _TRUNCATE,
                 L"http://www.accuraterip.com/accuraterip/%x/%x/%x/dBAR-%03d-%08x-%08x-%08x.bin",
                 ids.id1 & 0xF, (ids.id1 >> 4) & 0xF, (ids.id1 >> 8) & 0xF,
                 ids.audioTracks, ids.id1, ids.id2, ids.cddb);
    DWORD status = 0;
    std::string body;
    if (!HttpGetBytes(url, &status, &body, 4 * 1024 * 1024)) return ArDbState::Unreachable;
    if (status == 404) return ArDbState::NotInDb;
    if (status != 200) {
        LogF("CDVERIFY", "AccurateRip: HTTP %lu", status);
        return ArDbState::Unreachable;
    }
    const BYTE* p = reinterpret_cast<const BYTE*>(body.data());
    size_t n = body.size(), at = 0;
    auto u32 = [p](size_t o) {
        return p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
    };
    while (at + 13 <= n) {
        int tracks = p[at];
        at += 13;                                // count, id1, id2, cddb
        if (at + static_cast<size_t>(tracks) * 9 > n) break;
        ArPressing pr;
        for (int t = 0; t < tracks; ++t, at += 9) {
            pr.tracks.push_back({ p[at], u32(at + 1), u32(at + 5) });
        }
        out.push_back(pr);
    }
    LogF("CDVERIFY", "AccurateRip: %u pressings", static_cast<unsigned>(out.size()));
    return out.empty() ? ArDbState::NotInDb : ArDbState::Found;
}

ArTrackResult MatchTrack(const std::vector<ArPressing>& db, int pos, const ArCrc& crc) {
    ArTrackResult r;
    if (db.empty()) { r.status = ArStatus::NotInDb; return r; }
    r.status = ArStatus::NotAccurate;
    for (const ArPressing& pr : db) {
        if (pos < 0 || pos >= static_cast<int>(pr.tracks.size())) continue;
        const ArEntry& e = pr.tracks[pos];
        // v1 and v2 checksums share the field, in different pressings.
        if (e.crc != 0 && (e.crc == crc.v1 || e.crc == crc.v2)) {
            r.status = ArStatus::Accurate;
            r.confidence += e.confidence;
        }
    }
    return r;
}

// ---- Drive offset ----

std::wstring Normalize(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) if (iswalnum(c)) out += static_cast<wchar_t>(towupper(c));
    return out;
}

bool SavedDriveOffset(const std::wstring& model, int* offset, bool* detected) {
    return CdOffsetGet(WideToUtf8(model), offset, detected);
}

void SaveDriveOffset(const std::wstring& model, int offset, bool detected) {
    CdOffsetPut(WideToUtf8(model), offset, detected);
}

void ForgetDriveOffset(const std::wstring& model) {
    CdOffsetDelete(WideToUtf8(model));
}

bool ListedDriveOffset(const std::wstring& vendorModel, int* offset) {
    static std::mutex m;
    static bool fetched = false;
    static std::string list;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!fetched) {
            DWORD status = 0;
            if (HttpGetBytes(L"http://www.accuraterip.com/accuraterip/DriveOffsets.bin", &status, &list,
                             4 * 1024 * 1024) && status == 200) {
                fetched = true;
            } else {
                list.clear();
                return false;
            }
        }
    }
    std::wstring want = Normalize(vendorModel);
    const size_t rec = 0x45;   // int16 offset, 33-byte name, 34 bytes of other data
    for (size_t at = 0; at + rec <= list.size(); at += rec) {
        int16_t off = static_cast<int16_t>(static_cast<BYTE>(list[at]) | (static_cast<BYTE>(list[at + 1]) << 8));
        std::string name(list.data() + at + 2, strnlen(list.data() + at + 2, 33));
        if (Normalize(Utf8ToWide(name)) == want) {
            *offset = off;
            return true;
        }
    }
    return false;
}

namespace {

// Reads one track (with the drive offset set to `shift`) into 32-bit words.
bool ReadTrackWords(const basscd::Api* cd, DWORD drive, int track, int shift,
                    std::vector<uint32_t>& out, const volatile bool* cancel) {
    out.clear();
    cd->SetOffset(drive, shift);
    HSTREAM s = cd->StreamCreate(drive, static_cast<DWORD>(track), BASS_STREAM_DECODE);
    if (!s) return false;
    QWORD len = BASS_ChannelGetLength(s, BASS_POS_BYTE);
    if (len != static_cast<QWORD>(-1)) out.reserve(static_cast<size_t>(len / 4));
    static BYTE buf[588 * 4 * 75];
    bool ok = true;
    for (;;) {
        if (cancel && *cancel) { ok = false; break; }
        DWORD got = BASS_ChannelGetData(s, buf, sizeof(buf));
        if (got == static_cast<DWORD>(-1)) { ok = BASS_ErrorGetCode() == BASS_ERROR_ENDED; break; }
        for (DWORD i = 0; i + 4 <= got; i += 4) {
            out.push_back(buf[i] | (buf[i + 1] << 8) | (buf[i + 2] << 16) | (static_cast<uint32_t>(buf[i + 3]) << 24));
        }
    }
    BASS_StreamFree(s);
    cd->SetOffset(drive, 0);
    return ok;
}

} // namespace

OffsetDetect DetectReadOffset(const DiscDrive& drive, int* offset, const volatile bool* cancel) {
    const basscd::Api* cd = basscd::Get();
    if (!cd) return OffsetDetect::ReadError;
    ArDiscIds ids = ComputeArIds(drive);
    std::vector<ArPressing> db;
    if (FetchAccurateRip(ids, db) != ArDbState::Found) return OffsetDetect::NotInDb;
    if (ids.audioTracks < 3) return OffsetDetect::TooFewTracks;

    // Audio track positions -> BASSCD track numbers.
    std::vector<int> audio;
    DiscContents contents = ReadDiscContents(drive);
    for (const DiscTrack& t : contents.tracks) if (!t.isData) audio.push_back(t.index);
    if (static_cast<int>(audio.size()) != ids.audioTracks) return OffsetDetect::ReadError;

    const int M = 5880;                        // 10 frames each way
    const int pos = ids.audioTracks / 2;       // a middle track: no exclusion
    std::vector<uint32_t> early, late;
    if (!ReadTrackWords(cd, drive.bassIndex, audio[pos], -M, early, cancel) ||
        !ReadTrackWords(cd, drive.bassIndex, audio[pos], +M, late, cancel)) {
        return (cancel && *cancel) ? OffsetDetect::Cancelled : OffsetDetect::ReadError;
    }
    const size_t N = early.size();
    if (N < static_cast<size_t>(4 * M) || late.size() != N) return OffsetDetect::ReadError;

    // `first` starts M words before the track, `second` M words after it:
    // span = [start - M, end + M). Window at start + d: C = sum(span[w + j] *
    // (j + 1)), w = M + d, slid one word at a time:
    // C' = C - S + N * span[w + N], S' = S - span[w] + span[w + N] (mod 2^32).
    int best = 0, bestConf = -1;
    auto slide = [&](const std::vector<uint32_t>& first, const std::vector<uint32_t>& second, int sign) {
        std::vector<uint32_t> span(first);
        span.insert(span.end(), second.end() - 2 * M, second.end());
        uint32_t C = 0, S = 0;
        for (size_t j = 0; j < N; ++j) {
            C += span[j] * static_cast<uint32_t>(j + 1);
            S += span[j];
        }
        for (int w = 0; w <= 2 * M; ++w) {
            for (const ArPressing& pr : db) {
                if (pos < static_cast<int>(pr.tracks.size()) && pr.tracks[pos].crc == C &&
                    pr.tracks[pos].confidence > bestConf) {
                    bestConf = pr.tracks[pos].confidence;
                    best = sign * (w - M);
                }
            }
            if (w == 2 * M) break;
            C = C - S + static_cast<uint32_t>(N) * span[w + N];
            S = S - span[w] + span[w + N];
        }
    };
    // SetOffset(-M) read M samples earlier (the usual meaning)...
    slide(early, late, +1);
    // ...or later: then the two reads swap roles and the sign turns.
    if (bestConf < 0) slide(late, early, -1);
    if (bestConf < 0) return OffsetDetect::NoMatch;

    // Confirmed on another middle track, read with that offset (v1 or v2).
    const int pos2 = (pos + 1 < ids.audioTracks - 1) ? pos + 1 : pos - 1;
    std::vector<uint32_t> words;
    if (!ReadTrackWords(cd, drive.bassIndex, audio[pos2], best, words, cancel)) {
        return (cancel && *cancel) ? OffsetDetect::Cancelled : OffsetDetect::ReadError;
    }
    ArAccumulator acc;
    acc.Begin(words.size(), false, false);
    acc.Add(reinterpret_cast<const BYTE*>(words.data()), words.size() * 4);
    if (MatchTrack(db, pos2, acc.Result()).status != ArStatus::Accurate) return OffsetDetect::NoMatch;
    *offset = best;
    LogF("CDVERIFY", "read offset %d detected (tracks %d and %d)", best, pos + 1, pos2 + 1);
    return OffsetDetect::Found;
}

} // namespace mediaaccess
