// cd_metadata.cpp — see cd_metadata.h.

#include "cd_metadata.h"
#include "basscd_loader.h"
#include "database.h"
#include "translations.h"
#include "utils.h"
#include "logger.h"
#include "version.h"
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <sstream>

using nlohmann::json;

namespace mediaaccess {

namespace {

const char* kUserAgent = "MediaAccess/" APP_VERSION " ( https://reaperaccessible.fr )";
const char* kVariousArtists = "89ad4ac3-39f7-470e-963a-56509c546377";
// GnuDB identifies each program by this "hello" (annex A of the plan). The
// address is a public relay on reaperaccessible.fr, registered with GnuDB.
const char* kCddbHello = "cddb+reaperaccessible.fr+MediaAccess+" APP_VERSION;

std::wstring W(const std::string& s) { return Utf8ToWide(s); }

std::wstring JStr(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? W(it->get<std::string>()) : std::wstring();
}

// ---- HTTP (WinHTTP) ----

struct HttpReply { DWORD status = 0; std::string body; };

bool HttpGet(const std::wstring& url, HttpReply& reply, size_t maxBytes) {
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = _countof(path);
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = _countof(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    std::wstring target = std::wstring(path) + extra;

    std::wstring agent = W(kUserAgent);
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    if (req && WinHttpSendRequest(req, L"Accept: application/json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        reply.status = status;
        reply.body.clear();
        ok = true;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            if (reply.body.size() + avail > maxBytes) { ok = false; break; }
            size_t old = reply.body.size();
            reply.body.resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &reply.body[old], avail, &got)) { ok = false; break; }
            reply.body.resize(old + got);
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

// MusicBrainz allows one request per second; a 503 means "too fast".
std::mutex g_mbMutex;
ULONGLONG  g_mbLast = 0;

bool MusicBrainzGet(const std::wstring& url, HttpReply& reply) {
    std::lock_guard<std::mutex> lock(g_mbMutex);
    const DWORD backoff[] = { 0, 2000, 4000, 8000 };
    for (DWORD wait : backoff) {
        if (wait) Sleep(wait);
        ULONGLONG now = GetTickCount64();
        if (now - g_mbLast < 1100) Sleep((DWORD)(1100 - (now - g_mbLast)));
        g_mbLast = GetTickCount64();
        if (!HttpGet(url, reply, 8 * 1024 * 1024)) return false;
        if (reply.status != 503) return true;
        LogF("CDMETA", "MusicBrainz 503, retrying");
    }
    return true;
}

// ---- Disc IDs ----

std::string CdId(DWORD drive, DWORD id) {
    const basscd::Api* cd = basscd::Get();
    const char* p = cd ? cd->GetID(drive, id) : nullptr;
    return p ? std::string(p) : std::string();
}

// MusicBrainz "toc" parameter: first+last+leadout+offsets, all LBA + 150.
// A data track at the end (enhanced CD) is left out, and the leadout is then
// the start of that data session minus 11400 sectors.
std::wstring BuildTocParam(DWORD drive) {
    const basscd::Api* cd = basscd::Get();
    BASS_CD_TOC toc;
    ZeroMemory(&toc, sizeof(toc));
    if (!cd || !cd->GetTOC(drive, 0, &toc) || toc.last < toc.first) return std::wstring();
    int count = toc.last - toc.first + 1;          // entries before the leadout
    int lastAudio = count - 1;
    while (lastAudio >= 0 && (toc.tracks[lastAudio].adrcon & BASS_CD_TOC_CON_DATA)) --lastAudio;
    if (lastAudio < 0) return std::wstring();
    DWORD leadout = toc.tracks[count].lba + 150;
    if (lastAudio < count - 1) leadout = toc.tracks[lastAudio + 1].lba + 150 - 11400;
    std::wostringstream o;
    o << toc.first << L'+' << (toc.first + lastAudio) << L'+' << leadout;
    for (int i = 0; i <= lastAudio; ++i) o << L'+' << (toc.tracks[i].lba + 150);
    return o.str();
}

std::wstring JoinCredit(const json& credits) {
    std::wstring out;
    if (!credits.is_array()) return out;
    for (const json& c : credits) {
        out += JStr(c, "name");
        out += JStr(c, "joinphrase");
    }
    return out;
}

std::wstring TopGenre(const json& j) {
    auto it = j.find("genres");
    if (it == j.end() || !it->is_array()) return std::wstring();
    std::wstring best;
    int bestCount = -1;
    for (const json& g : *it) {
        int n = g.value("count", 0);
        if (n > bestCount) { bestCount = n; best = JStr(g, "name"); }
    }
    return best;
}

// One candidate per release, on the medium holding this disc.
std::vector<CdReleaseMeta> ParseMusicBrainz(const std::string& body, const std::string& discId, int audioTracks) {
    std::vector<CdReleaseMeta> out;
    json root = json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.contains("releases")) return out;
    for (const json& r : root["releases"]) {
        const json* medium = nullptr;
        auto media = r.find("media");
        if (media == r.end() || !media->is_array()) continue;
        for (const json& m : *media) {                       // exact Disc ID
            auto discs = m.find("discs");
            if (discs == m.end()) continue;
            for (const json& d : *discs) {
                if (d.value("id", "") == discId) { medium = &m; break; }
            }
            if (medium) break;
        }
        if (!medium) {                                        // close TOC: by track count
            for (const json& m : *media) {
                if (m.value("track-count", 0) == audioTracks) { medium = &m; break; }
            }
        }
        if (!medium) continue;

        CdReleaseMeta meta;
        meta.source = L"MusicBrainz";
        meta.album = JStr(r, "title");
        meta.albumArtist = JoinCredit(r.value("artist-credit", json::array()));
        meta.year = JStr(r, "date").substr(0, 4);
        meta.country = JStr(r, "country");
        meta.barcode = JStr(r, "barcode");
        meta.releaseMbid = JStr(r, "id");
        if (r.contains("release-group") && r["release-group"].is_object()) {
            meta.releaseGroupMbid = JStr(r["release-group"], "id");
        }
        if (r.contains("label-info") && r["label-info"].is_array() && !r["label-info"].empty()) {
            const json& li = r["label-info"][0];
            meta.catalog = JStr(li, "catalog-number");
            if (li.contains("label") && li["label"].is_object()) meta.label = JStr(li["label"], "name");
        }
        meta.genre = TopGenre(r);
        if (meta.genre.empty() && r.contains("release-group") && r["release-group"].is_object()) {
            meta.genre = TopGenre(r["release-group"]);
        }
        auto ac = r.find("artist-credit");
        if (ac != r.end() && ac->is_array() && ac->size() == 1 && (*ac)[0].contains("artist")) {
            meta.compilation = (*ac)[0]["artist"].value("id", "") == kVariousArtists;
        }
        meta.hasFrontCover = r.contains("cover-art-archive") &&
                             r["cover-art-archive"].value("front", false);
        meta.discNumber = medium->value("position", 1);
        meta.discTotal = (int)media->size();
        auto tracks = medium->find("tracks");
        if (tracks != medium->end() && tracks->is_array()) {
            for (const json& t : *tracks) {
                CdTrackMeta tm;
                tm.number = t.value("position", (int)meta.tracks.size() + 1);
                tm.title = JStr(t, "title");
                tm.artist = JoinCredit(t.value("artist-credit", json::array()));
                if (t.contains("recording") && t["recording"].is_object()) {
                    const json& rec = t["recording"];
                    tm.recordingMbid = JStr(rec, "id");
                    if (rec.contains("isrcs") && rec["isrcs"].is_array() && !rec["isrcs"].empty() &&
                        rec["isrcs"][0].is_string()) {
                        tm.isrc = W(rec["isrcs"][0].get<std::string>());
                    }
                }
                if (tm.artist.empty()) tm.artist = meta.albumArtist;
                meta.tracks.push_back(tm);
            }
        }
        out.push_back(meta);
    }
    return out;
}

// ---- CD-Text ----

std::wstring DecodeText(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), nullptr, 0);
    UINT cp = n > 0 ? CP_UTF8 : 1252;   // encoding not documented: UTF-8, else Windows-1252
    n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool ReadCdText(DWORD drive, const DiscContents& c, CdReleaseMeta& meta) {
    const basscd::Api* cd = basscd::Get();
    const char* p = cd ? cd->GetID(drive, BASS_CDID_TEXT) : nullptr;
    if (!p || !*p) return false;
    meta = DefaultRelease(c);
    bool any = false;
    for (; *p; p += strlen(p) + 1) {            // "TITLE0=..." strings, double-null end
        std::string entry(p);
        size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string tag = entry.substr(0, eq);
        std::wstring value = DecodeText(entry.substr(eq + 1));
        size_t digits = tag.find_first_of("0123456789");
        if (digits == std::string::npos || value.empty()) continue;
        std::string name = tag.substr(0, digits);
        int num = atoi(tag.c_str() + digits);    // 0 = the disc, n = track n
        if (num == 0) {
            if (name == "TITLE") meta.album = value;
            else if (name == "PERFORMER") meta.albumArtist = value;
            else if (name == "GENRE") meta.genre = value;
            else continue;
            any = true;
        } else {
            for (CdTrackMeta& t : meta.tracks) {
                if (t.number != num) continue;
                if (name == "TITLE") { t.title = value; any = true; }
                else if (name == "PERFORMER") t.artist = value;
                else if (name == "ISRC") t.isrc = value;
            }
        }
    }
    if (!any) return false;
    for (CdTrackMeta& t : meta.tracks) if (t.artist.empty()) t.artist = meta.albumArtist;
    meta.source = L"CD-Text";
    return true;
}

// ---- GnuDB (CDDB over HTTP, through BASSCD) ----

bool ReadGnuDb(DWORD drive, const DiscContents& c, CdReleaseMeta& meta) {
    BASS_SetConfigPtr(BASS_CONFIG_CD_CDDB_HELLO, kCddbHello);   // always before a query
    std::string query = CdId(drive, BASS_CDID_CDDB_QUERY);
    if (query.empty()) return false;
    // An unregistered program gets a fake match: never take it as a title.
    if (query.find("Unknown application") != std::string::npos) {
        LogF("CDMETA", "GnuDB refused the hello string");
        return false;
    }
    std::string entry = CdId(drive, BASS_CDID_CDDB_READ);       // first match
    if (entry.empty()) return false;
    meta = DefaultRelease(c);
    std::map<int, std::wstring> titles;
    std::wstring dtitle;
    std::istringstream in(entry);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string key = line.substr(0, eq);
        std::wstring value = DecodeText(line.substr(eq + 1));
        if (key == "DTITLE") dtitle += value;               // lines may be split
        else if (key == "DYEAR") meta.year = value;
        else if (key == "DGENRE") meta.genre = value;
        else if (key.compare(0, 6, "TTITLE") == 0) titles[atoi(key.c_str() + 6)] += value;
    }
    size_t slash = dtitle.find(L" / ");
    meta.albumArtist = slash == std::wstring::npos ? dtitle : dtitle.substr(0, slash);
    meta.album = slash == std::wstring::npos ? dtitle : dtitle.substr(slash + 3);
    for (size_t i = 0; i < meta.tracks.size(); ++i) {   // TTITLEn: 0-based
        auto it = titles.find((int)i);
        if (it == titles.end() || it->second.empty()) continue;
        std::wstring t = it->second;
        size_t s2 = t.find(L" / ");                         // "Artist / Title" on compilations
        if (s2 != std::wstring::npos) {
            meta.tracks[i].artist = t.substr(0, s2);
            t = t.substr(s2 + 3);
        } else {
            meta.tracks[i].artist = meta.albumArtist;
        }
        meta.tracks[i].title = t;
    }
    meta.source = L"GnuDB";
    return !meta.album.empty();
}

// ---- Cache (JSON in SQLite) ----

json ToJson(const CdReleaseMeta& m) {
    json j;
    j["album"] = WideToUtf8(m.album);
    j["albumArtist"] = WideToUtf8(m.albumArtist);
    j["year"] = WideToUtf8(m.year);
    j["country"] = WideToUtf8(m.country);
    j["label"] = WideToUtf8(m.label);
    j["catalog"] = WideToUtf8(m.catalog);
    j["barcode"] = WideToUtf8(m.barcode);
    j["genre"] = WideToUtf8(m.genre);
    j["releaseMbid"] = WideToUtf8(m.releaseMbid);
    j["releaseGroupMbid"] = WideToUtf8(m.releaseGroupMbid);
    j["discNumber"] = m.discNumber;
    j["discTotal"] = m.discTotal;
    j["compilation"] = m.compilation;
    j["hasFrontCover"] = m.hasFrontCover;
    j["source"] = WideToUtf8(m.source);
    json tracks = json::array();
    for (const CdTrackMeta& t : m.tracks) {
        tracks.push_back({ { "number", t.number }, { "title", WideToUtf8(t.title) },
                           { "artist", WideToUtf8(t.artist) }, { "isrc", WideToUtf8(t.isrc) },
                           { "recordingMbid", WideToUtf8(t.recordingMbid) } });
    }
    j["tracks"] = tracks;
    return j;
}

bool FromJson(const std::string& text, CdReleaseMeta& m) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    m.album = JStr(j, "album");
    m.albumArtist = JStr(j, "albumArtist");
    m.year = JStr(j, "year");
    m.country = JStr(j, "country");
    m.label = JStr(j, "label");
    m.catalog = JStr(j, "catalog");
    m.barcode = JStr(j, "barcode");
    m.genre = JStr(j, "genre");
    m.releaseMbid = JStr(j, "releaseMbid");
    m.releaseGroupMbid = JStr(j, "releaseGroupMbid");
    m.discNumber = j.value("discNumber", 1);
    m.discTotal = j.value("discTotal", 1);
    m.compilation = j.value("compilation", false);
    m.hasFrontCover = j.value("hasFrontCover", false);
    m.source = JStr(j, "source");
    m.tracks.clear();
    if (j.contains("tracks") && j["tracks"].is_array()) {
        for (const json& t : j["tracks"]) {
            CdTrackMeta tm;
            tm.number = t.value("number", 0);
            tm.title = JStr(t, "title");
            tm.artist = JStr(t, "artist");
            tm.isrc = JStr(t, "isrc");
            tm.recordingMbid = JStr(t, "recordingMbid");
            m.tracks.push_back(tm);
        }
    }
    return true;
}

// The candidate must describe this disc: one entry per audio track.
bool FitsDisc(const CdReleaseMeta& m, const DiscContents& c) {
    return (int)m.tracks.size() == c.AudioTrackCount();
}

} // namespace

CdReleaseMeta DefaultRelease(const DiscContents& contents) {
    CdReleaseMeta meta;
    for (const DiscTrack& t : contents.tracks) {
        if (t.isData) continue;
        CdTrackMeta tm;
        tm.number = t.number;
        wchar_t buf[64];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, T("Track %d"), t.number);
        tm.title = buf;
        meta.tracks.push_back(tm);
    }
    return meta;
}

CdLookupResult LookupDisc(const DiscDrive& drive, const DiscContents& contents, bool useCache) {
    CdLookupResult r;
    r.discId = CdId(drive.bassIndex, BASS_CDID_MUSICBRAINZ);
    r.tocParam = BuildTocParam(drive.bassIndex);
    int audio = contents.AudioTrackCount();

    if (useCache && !r.discId.empty()) {
        std::string cached;
        CdReleaseMeta m;
        if (CdCacheGet(r.discId, cached) && FromJson(cached, m) && FitsDisc(m, contents)) {
            r.candidates.push_back(m);
            r.fromCache = true;
            return r;
        }
    }
    if (!r.discId.empty()) {
        std::wstring url = L"https://musicbrainz.org/ws/2/discid/" + W(r.discId) +
                           L"?toc=" + r.tocParam +
                           L"&cdstubs=no&inc=recordings+artist-credits+labels+release-groups+isrcs+genres&fmt=json";
        HttpReply reply;
        if (MusicBrainzGet(url, reply) && reply.status == 200) {
            for (CdReleaseMeta& m : ParseMusicBrainz(reply.body, r.discId, audio)) {
                if (FitsDisc(m, contents)) r.candidates.push_back(m);
            }
        } else {
            LogF("CDMETA", "MusicBrainz: HTTP %lu", reply.status);
        }
    }
    if (!r.candidates.empty()) return r;

    CdReleaseMeta m;
    if (ReadCdText(drive.bassIndex, contents, m) && FitsDisc(m, contents)) {
        r.candidates.push_back(m);
        return r;
    }
    if (ReadGnuDb(drive.bassIndex, contents, m) && FitsDisc(m, contents)) {
        r.candidates.push_back(m);
    }
    return r;
}

void SaveToCache(const std::string& discId, const CdReleaseMeta& meta) {
    if (discId.empty()) return;
    CdCachePut(discId, WideToUtf8(meta.releaseMbid), ToJson(meta).dump());
}

bool DownloadFrontCover(const CdReleaseMeta& meta, std::vector<BYTE>& image) {
    if (meta.releaseMbid.empty()) return false;
    const std::wstring urls[] = {
        L"https://coverartarchive.org/release/" + meta.releaseMbid + L"/front-500",
        meta.releaseGroupMbid.empty() ? std::wstring()
            : L"https://coverartarchive.org/release-group/" + meta.releaseGroupMbid + L"/front-500",
    };
    for (const std::wstring& url : urls) {
        if (url.empty()) continue;
        HttpReply reply;   // WinHTTP follows the redirects to archive.org
        if (HttpGet(url, reply, 2 * 1024 * 1024) && reply.status == 200 && reply.body.size() > 100) {
            image.assign(reply.body.begin(), reply.body.end());
            return true;
        }
    }
    return false;
}

std::wstring DescribeRelease(const CdReleaseMeta& m) {
    std::vector<std::wstring> parts = { m.album, m.albumArtist, m.year, m.country, m.label, m.barcode };
    std::wstring out;
    for (const std::wstring& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += L", ";
        out += p;
    }
    wchar_t buf[128];
    if (m.discTotal > 1) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, T("CD %d of %d"), m.discNumber, m.discTotal);
        out += L", "; out += buf;
    }
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, T("%d tracks"), (int)m.tracks.size());
    out += L", "; out += buf;
    return out;
}

std::wstring MusicBrainzSubmitUrl(const CdLookupResult& r, int audioTracks) {
    return L"https://musicbrainz.org/cdtoc/attach?id=" + W(r.discId) +
           L"&tracks=" + std::to_wstring(audioTracks) + L"&toc=" + r.tocParam;
}

} // namespace mediaaccess
