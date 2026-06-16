// =============================================================================
// edge_tts_client.cpp — native Edge "Read Aloud" online TTS client.
// See edge_tts_client.h for the public contract and tts_latency/EDGE_PROTOCOL.md
// for the validated wire protocol this implements.
// =============================================================================
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00          // WinHTTP websocket APIs need >= Win8 (0x0602)
#endif

#include "mediaaccess/edge_tts_client.h"
#include "mediaaccess/utils.h"        // WideToUtf8
#include "mediaaccess/logger.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <rpc.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "rpcrt4.lib")

namespace mediaaccess {

namespace {

const char*    TRUSTED      = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
const wchar_t* HOST         = L"speech.platform.bing.com";
const wchar_t* WS_PATH      = L"/consumer/speech/synthesize/readaloud/edge/v1";
const wchar_t* VOICES_PATH  = L"/consumer/speech/synthesize/readaloud/voices/list";
const wchar_t* GEC_VERSION  = L"1-143.0.3650.75";
const wchar_t* USER_AGENT   =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko)"
    L" Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0";

// Clock-skew correction (seconds). Adjusted when the service answers 403 with a
// server Date that disagrees with our clock. Guarded for multi-worker safety.
std::atomic<double> g_skewSeconds{0.0};

// ---- small helpers ----------------------------------------------------------

std::string Sha256Upper(const std::string& in) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return "";
    DWORD objLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objLen, sizeof(objLen), &cb, 0);
    std::vector<unsigned char> obj(objLen);
    BYTE digest[32];
    std::string out;
    if (BCryptCreateHash(alg, &h, obj.data(), objLen, nullptr, 0, 0) == 0) {
        BCryptHashData(h, (PUCHAR)in.data(), (ULONG)in.size(), 0);
        if (BCryptFinishHash(h, digest, 32, 0) == 0) {
            static const char* HX = "0123456789ABCDEF";
            out.reserve(64);
            for (int i = 0; i < 32; i++) { out += HX[digest[i] >> 4]; out += HX[digest[i] & 0xF]; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

// FILETIME (100ns since 1601) + skew, rounded down to 5 min, concatenated with
// the trusted client token, SHA-256, uppercase hex. (See EDGE_PROTOCOL.md §2.)
std::string GenToken() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    ULONGLONG ticks = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    ticks += (LONGLONG)(g_skewSeconds.load() * 1e7);
    ticks -= ticks % 3000000000ULL;
    char num[32]; sprintf_s(num, "%llu", ticks);
    return Sha256Upper(std::string(num) + TRUSTED);
}

std::string UuidHex() {
    UUID u; UuidCreate(&u);
    char b[33];
    sprintf_s(b, "%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
              u.Data1, u.Data2, u.Data3,
              u.Data4[0], u.Data4[1], u.Data4[2], u.Data4[3],
              u.Data4[4], u.Data4[5], u.Data4[6], u.Data4[7]);
    return b;
}

std::string UuidHexUpper() {
    std::string s = UuidHex();
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

std::string JsDate() {
    time_t t = time(nullptr); struct tm g; gmtime_s(&g, &t);
    // English C-locale weekday/month abbreviations are required by the service.
    static const char* WD[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* MO[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char d[96];
    sprintf_s(d, "%s %s %02d %04d %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)",
              WD[g.tm_wday], MO[g.tm_mon], g.tm_mday, g.tm_year + 1900,
              g.tm_hour, g.tm_min, g.tm_sec);
    return d;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// Strip control characters the service rejects, then XML-escape, then UTF-8.
std::string SanitizeForSsml(const std::wstring& text) {
    std::wstring cleaned;
    cleaned.reserve(text.size());
    for (wchar_t c : text) {
        if ((c >= 0 && c <= 8) || (c == 11 || c == 12) || (c >= 14 && c <= 31))
            cleaned += L' ';
        else
            cleaned += c;
    }
    std::string utf8 = WideToUtf8(cleaned);
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '\'': out += "&apos;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

struct AutoInternet {
    HINTERNET h{nullptr};
    AutoInternet() = default;
    explicit AutoInternet(HINTERNET x) : h(x) {}
    ~AutoInternet() { if (h) WinHttpCloseHandle(h); }
    AutoInternet(const AutoInternet&) = delete;
    AutoInternet& operator=(const AutoInternet&) = delete;
    operator HINTERNET() const { return h; }
};

enum class Attempt { Ok, Forbidden, Failed };

// One synthesis round trip. On HTTP 403 returns Forbidden after recording the
// server clock skew so the caller can retry with a corrected token.
Attempt SynthAttempt(const std::string& voice, const std::string& ssmlText,
                     const std::string& rate, const std::string& pitch,
                     std::vector<unsigned char>& out, std::string* err) {
    out.clear();

    AutoInternet ses(WinHttpOpen(L"MediaAccess-EdgeTTS", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) { if (err) *err = "WinHttpOpen failed"; return Attempt::Failed; }

    AutoInternet con(WinHttpConnect(ses, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!con.h) { if (err) *err = "WinHttpConnect failed"; return Attempt::Failed; }

    std::wstring path = std::wstring(WS_PATH)
        + L"?TrustedClientToken=" + Widen(TRUSTED)
        + L"&ConnectionId=" + Widen(UuidHex())
        + L"&Sec-MS-GEC=" + Widen(GenToken())
        + L"&Sec-MS-GEC-Version=" + GEC_VERSION;

    AutoInternet req(WinHttpOpenRequest(con, L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!req.h) { if (err) *err = "WinHttpOpenRequest failed"; return Attempt::Failed; }

    std::wstring headers =
        L"Origin: chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold\r\n"
        L"Pragma: no-cache\r\nCache-Control: no-cache\r\n"
        L"User-Agent: " + std::wstring(USER_AGENT) + L"\r\n"
        L"Accept-Encoding: gzip, deflate, br, zstd\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n"
        L"Cookie: muid=" + Widen(UuidHexUpper()) + L";\r\n";
    WinHttpAddRequestHeaders(req, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) {
        if (err) *err = "WinHttpSendRequest failed"; return Attempt::Failed;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        if (err) *err = "WinHttpReceiveResponse failed"; return Attempt::Failed;
    }

    DWORD status = 0, slen = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
    if (status == 403) {
        // Record clock skew from the server Date header for the retry.
        SYSTEMTIME st; DWORD dlen = sizeof(st);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                                WINHTTP_HEADER_NAME_BY_INDEX, &st, &dlen, WINHTTP_NO_HEADER_INDEX)) {
            FILETIME sft; SystemTimeToFileTime(&st, &sft);
            ULONGLONG server = ((ULONGLONG)sft.dwHighDateTime << 32) | sft.dwLowDateTime;
            FILETIME nft; GetSystemTimeAsFileTime(&nft);
            ULONGLONG now = ((ULONGLONG)nft.dwHighDateTime << 32) | nft.dwLowDateTime;
            g_skewSeconds.store(((double)server - (double)now) / 1e7);
        }
        if (err) *err = "HTTP 403 (token rejected)";
        return Attempt::Forbidden;
    }
    if (status != 101) {
        if (err) *err = "unexpected HTTP status " + std::to_string(status);
        return Attempt::Failed;
    }

    HINTERNET wsRaw = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!wsRaw) { if (err) *err = "WebSocket upgrade failed"; return Attempt::Failed; }
    AutoInternet ws(wsRaw);

    // (a) config
    std::string cfg =
        "X-Timestamp:" + JsDate() + "\r\n"
        "Content-Type:application/json; charset=utf-8\r\nPath:speech.config\r\n\r\n"
        "{\"context\":{\"synthesis\":{\"audio\":{\"metadataoptions\":{"
        "\"sentenceBoundaryEnabled\":\"true\",\"wordBoundaryEnabled\":\"false\"},"
        "\"outputFormat\":\"audio-24khz-48kbitrate-mono-mp3\"}}}}\r\n";
    if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)cfg.data(), (DWORD)cfg.size()) != NO_ERROR) {
        if (err) *err = "send config failed"; return Attempt::Failed;
    }

    // (b) SSML
    std::string ssml =
        "X-RequestId:" + UuidHex() + "\r\nContent-Type:application/ssml+xml\r\n"
        "X-Timestamp:" + JsDate() + "Z\r\nPath:ssml\r\n\r\n"
        "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='en-US'>"
        "<voice name='" + voice + "'>"
        "<prosody pitch='" + pitch + "' rate='" + rate + "' volume='+0%'>" + ssmlText +
        "</prosody></voice></speak>";
    if (WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (PVOID)ssml.data(), (DWORD)ssml.size()) != NO_ERROR) {
        if (err) *err = "send ssml failed"; return Attempt::Failed;
    }

    // Receive: assemble fragmented messages; binary frames carry the MP3.
    std::vector<unsigned char> msg;
    for (;;) {
        unsigned char buf[16384]; DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
        if (WinHttpWebSocketReceive(ws, buf, sizeof(buf), &got, &bt) != NO_ERROR) {
            if (err) *err = "websocket receive failed"; break;
        }
        msg.insert(msg.end(), buf, buf + got);
        if (bt == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            bt == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
        if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            std::string t((char*)msg.data(), msg.size());
            if (t.find("Path:turn.end") != std::string::npos) { msg.clear(); break; }
        } else if (bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            if (msg.size() >= 2) {
                size_t hl = ((size_t)msg[0] << 8) | msg[1];
                size_t audioStart = hl + 2;
                if (audioStart <= msg.size()) {
                    std::string hdr((char*)msg.data(), audioStart);
                    if (hdr.find("Path:audio") != std::string::npos)
                        out.insert(out.end(), msg.begin() + audioStart, msg.end());
                }
            }
        }
        msg.clear();
    }

    WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    if (out.empty()) { if (err && err->empty()) *err = "no audio received"; return Attempt::Failed; }
    return Attempt::Ok;
}

// HTTP GET helper for the voices list. Returns the body (UTF-8) or "".
std::string HttpGet(const std::wstring& pathWithQuery) {
    AutoInternet ses(WinHttpOpen(L"MediaAccess-EdgeTTS", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!ses.h) return "";
    AutoInternet con(WinHttpConnect(ses, HOST, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!con.h) return "";
    AutoInternet req(WinHttpOpenRequest(con, L"GET", pathWithQuery.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!req.h) return "";
    // Let WinHTTP advertise only what it can transparently decompress (gzip/
    // deflate) and inflate the body for us — the voices list is served gzipped.
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    std::wstring headers =
        L"User-Agent: " + std::wstring(USER_AGENT) + L"\r\nAccept: */*\r\n"
        L"Accept-Language: en-US,en;q=0.9\r\n";
    WinHttpAddRequestHeaders(req, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0)) return "";
    if (!WinHttpReceiveResponse(req, nullptr)) return "";
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::vector<char> chunk(avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
        body.append(chunk.data(), read);
    }
    return body;
}

// Minimal field extractor for the (pretty-printed) voices JSON. Finds
// "key" : "value" tolerating whitespace around the colon, within [from,to).
std::string JsonStr(const std::string& s, size_t from, size_t to, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat, from);
    if (p == std::string::npos || p >= to) return "";
    p += pat.size();
    auto skipws = [&]() { while (p < to && (s[p]==' '||s[p]=='\t'||s[p]=='\r'||s[p]=='\n')) p++; };
    skipws();
    if (p >= to || s[p] != ':') return "";
    p++; skipws();
    if (p >= to || s[p] != '"') return "";
    p++;
    size_t e = s.find('"', p);
    if (e == std::string::npos || e > to) return "";
    return s.substr(p, e - p);
}

std::vector<EdgeVoice> BuiltinFallbackVoices() {
    // Small offline-safe list so the UI is never empty if the fetch fails.
    return {
        {"fr-FR-DeniseNeural",  L"Denise (fr-FR)",  "fr-FR", "Female"},
        {"fr-FR-HenriNeural",   L"Henri (fr-FR)",   "fr-FR", "Male"},
        {"fr-CA-SylvieNeural",  L"Sylvie (fr-CA)",  "fr-CA", "Female"},
        {"en-US-AriaNeural",    L"Aria (en-US)",    "en-US", "Female"},
        {"en-US-GuyNeural",     L"Guy (en-US)",     "en-US", "Male"},
        {"en-GB-SoniaNeural",   L"Sonia (en-GB)",   "en-GB", "Female"},
    };
}

} // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool EdgeSynthesize(const std::string& voiceShortName, const std::wstring& text,
                    std::vector<unsigned char>& outMp3,
                    const std::string& rate, const std::string& pitch,
                    std::string* err) {
    outMp3.clear();
    if (voiceShortName.empty() || text.empty()) {
        if (err) *err = "empty voice or text";
        return false;
    }
    std::string ssmlText = SanitizeForSsml(text);

    std::string localErr;
    Attempt a = SynthAttempt(voiceShortName, ssmlText, rate, pitch, outMp3, &localErr);
    if (a == Attempt::Forbidden) {
        // Token rejected; clock skew is now recorded — retry once.
        LogF("edge", "403, retrying with skew=%.1fs", g_skewSeconds.load());
        a = SynthAttempt(voiceShortName, ssmlText, rate, pitch, outMp3, &localErr);
    }
    if (a != Attempt::Ok) {
        LogF("edge", "synth failed: %s", localErr.c_str());
        if (err) *err = localErr;
        return false;
    }
    return true;
}

std::vector<EdgeVoice> EdgeListVoices() {
    static std::mutex mtx;
    static std::vector<EdgeVoice> cache;
    static bool cached = false;
    std::lock_guard<std::mutex> lk(mtx);
    if (cached) return cache;

    std::wstring url = std::wstring(VOICES_PATH)
        + L"?trustedclienttoken=" + Widen(TRUSTED)
        + L"&Sec-MS-GEC=" + Widen(GenToken())
        + L"&Sec-MS-GEC-Version=" + GEC_VERSION;
    std::string body = HttpGet(url);

    std::vector<EdgeVoice> out;
    // Each voice object lists ShortName before its Gender/Locale/FriendlyName,
    // so fields between one "ShortName" and the next belong to that voice.
    const std::string marker = "\"ShortName\"";
    size_t p = body.find(marker);
    while (p != std::string::npos) {
        size_t next = body.find(marker, p + marker.size());
        size_t segEnd = (next == std::string::npos) ? body.size() : next;
        EdgeVoice v;
        v.shortName = JsonStr(body, p, segEnd, "ShortName");
        v.locale = JsonStr(body, p, segEnd, "Locale");
        v.gender = JsonStr(body, p, segEnd, "Gender");
        std::string friendly = JsonStr(body, p, segEnd, "FriendlyName");
        v.displayName = Utf8ToWide(!friendly.empty() ? friendly : v.shortName);
        if (!v.shortName.empty()) out.push_back(std::move(v));
        p = next;
    }

    if (out.empty()) {
        LogF("edge", "voices fetch empty/failed (%zu bytes) — using fallback", body.size());
        out = BuiltinFallbackVoices();
    } else {
        LogF("edge", "fetched %zu Edge voices", out.size());
    }
    cache = out;
    cached = true;
    return cache;
}

} // namespace mediaaccess
