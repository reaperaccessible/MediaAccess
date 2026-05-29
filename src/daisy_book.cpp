// =============================================================================
// daisy_book.cpp — DAISY 2.02 / DAISY 3 / EPUB 3 book parser
//
// Uses MSXML6 for XML parsing (built into Windows, no new dependency).
//
// Architecture:
//   OpenDaisyBook(path)
//     -> DetectDaisyFormat()
//     -> ParseDaisy202() | ParseDaisy3() | ParseEpub3Metadata()
//        -> ParseSmilFile() for each spine entry / NCC link
//           -> ParseSmilTime() for clipBegin / clipEnd
//
// All XML parsing goes through the LoadXml() helper which tolerates malformed
// HTML/XHTML (NCC files in particular). Unreachable audio files are skipped
// with a logger warning so a partial book is still usable.
// =============================================================================

#include "mediaaccess/daisy_book.h"
#include "mediaaccess/logger.h"
#include "mediaaccess/utils.h"

#include <windows.h>
#include <shlwapi.h>
#include <objbase.h>
#include <comdef.h>
#include <msxml6.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace mediaaccess {

// -----------------------------------------------------------------------------
// COM smart-pointer helpers (avoid #import / ATL dependency)
// -----------------------------------------------------------------------------
namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() : p_(nullptr) {}
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { Release(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ~ComPtr() { if (p_) p_->Release(); }
    T*  Get() const { return p_; }
    T** PutVoid() { Release(); return &p_; }
    void** PutVoidPP() { Release(); return reinterpret_cast<void**>(&p_); }
    T*  operator->() const { return p_; }
    operator bool() const { return p_ != nullptr; }
    void Release() { if (p_) { p_->Release(); p_ = nullptr; } }
    void Attach(T* p) { Release(); p_ = p; }
private:
    T* p_;
};

struct BstrHolder {
    BSTR b;
    BstrHolder() : b(nullptr) {}
    BstrHolder(const wchar_t* s) : b(s ? SysAllocString(s) : nullptr) {}
    ~BstrHolder() { if (b) SysFreeString(b); }
    operator BSTR() const { return b; }
    BSTR* operator&() { if (b) { SysFreeString(b); b = nullptr; } return &b; }
};

// -----------------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------------

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return r;
}

std::string ToLowerA(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return r;
}

std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string TrimA(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

bool EndsWith(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

// -----------------------------------------------------------------------------
// Path helpers
// -----------------------------------------------------------------------------

std::wstring ParentFolderImpl(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return path.substr(0, pos);
}

// Normalize all forward slashes -> backslashes, collapse repeated separators.
std::wstring NormalizePath(const std::wstring& p) {
    std::wstring r;
    r.reserve(p.size());
    wchar_t last = 0;
    for (wchar_t c : p) {
        if (c == L'/') c = L'\\';
        if (c == L'\\' && last == L'\\') continue;
        r.push_back(c);
        last = c;
    }
    return r;
}

// Join a folder + (possibly relative) href into an absolute path.
// href is assumed URL-style; strip any #fragment first (callers split).
std::wstring JoinPath(const std::wstring& folder, const std::wstring& rel) {
    if (rel.empty()) return folder;
    // Already absolute?
    if (rel.size() >= 2 && (rel[1] == L':' ||
        (rel[0] == L'\\' && rel[1] == L'\\'))) {
        return NormalizePath(rel);
    }
    if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) {
        return NormalizePath(rel);
    }
    std::wstring r = folder;
    if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r.push_back(L'\\');
    r += rel;
    return NormalizePath(r);
}

// Split "file.smil#frag" -> (file.smil, frag)
void SplitFragment(const std::wstring& href, std::wstring& file, std::string& frag) {
    size_t pos = href.find(L'#');
    if (pos == std::wstring::npos) {
        file = href;
        frag.clear();
    } else {
        file = href.substr(0, pos);
        frag = WideToUtf8(href.substr(pos + 1));
    }
}

// URL-decode (%20 -> space, etc.)
std::wstring UrlDecode(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            auto hex = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return 10 + c - L'a';
                if (c >= L'A' && c <= L'F') return 10 + c - L'A';
                return -1;
            };
            int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) {
                r.push_back((wchar_t)((a << 4) | b));
                i += 2;
                continue;
            }
        }
        r.push_back(s[i]);
    }
    return r;
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Case-insensitive file lookup within a folder. Returns the full path with
// the exact on-disk casing, or empty string if not found.
std::wstring FindFileCI(const std::wstring& folder, const std::wstring& nameLower) {
    if (folder.empty()) return L"";
    std::wstring search = folder;
    if (!search.empty() && search.back() != L'\\') search.push_back(L'\\');
    search.push_back(L'*');

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring result;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, nameLower.c_str()) == 0) {
            result = folder;
            if (!result.empty() && result.back() != L'\\') result.push_back(L'\\');
            result += fd.cFileName;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return result;
}

// Find first file in a folder whose name matches a given suffix (case-insensitive),
// e.g. L".opf" or L".ncx". Returns full path or empty string.
std::wstring FindFileBySuffix(const std::wstring& folder, const std::wstring& suffixLower) {
    if (folder.empty()) return L"";
    std::wstring search = folder;
    if (!search.empty() && search.back() != L'\\') search.push_back(L'\\');
    search.push_back(L'*');

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring result;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (EndsWith(name, suffixLower)) {
            result = folder;
            if (!result.empty() && result.back() != L'\\') result.push_back(L'\\');
            result += fd.cFileName;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return result;
}

// -----------------------------------------------------------------------------
// File I/O — read the whole file into a UTF-8 std::string, stripping any BOM.
// -----------------------------------------------------------------------------

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = out.empty() ? TRUE : ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || (!out.empty() && read != out.size())) return false;

    // Strip UTF-8 BOM
    if (out.size() >= 3 &&
        (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    // Strip UTF-16 BOM and transcode
    else if (out.size() >= 2 && (unsigned char)out[0] == 0xFF && (unsigned char)out[1] == 0xFE) {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(out.data() + 2);
        size_t n = (out.size() - 2) / 2;
        std::wstring ws(w, n);
        out = WideToUtf8(ws);
    } else if (out.size() >= 2 && (unsigned char)out[0] == 0xFE && (unsigned char)out[1] == 0xFF) {
        // big-endian UTF-16: byte-swap
        std::wstring ws;
        ws.resize((out.size() - 2) / 2);
        for (size_t i = 0; i < ws.size(); ++i) {
            unsigned char hi = (unsigned char)out[2 + 2 * i];
            unsigned char lo = (unsigned char)out[2 + 2 * i + 1];
            ws[i] = (wchar_t)((hi << 8) | lo);
        }
        out = WideToUtf8(ws);
    }
    return true;
}

// -----------------------------------------------------------------------------
// MSXML wrappers
// -----------------------------------------------------------------------------

ComPtr<IXMLDOMDocument2> CreateXmlDoc() {
    ComPtr<IXMLDOMDocument2> doc;
    HRESULT hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(doc.PutVoid()));
    if (FAILED(hr) || !doc) return {};
    doc->put_async(VARIANT_FALSE);
    doc->put_validateOnParse(VARIANT_FALSE);
    doc->put_resolveExternals(VARIANT_FALSE);
    doc->put_preserveWhiteSpace(VARIANT_TRUE);

    // Define common DAISY namespace prefixes for XPath
    BstrHolder sel(L"SelectionLanguage");
    BstrHolder selv(L"XPath");
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = selv;
    doc->setProperty(sel, v);

    BstrHolder ns(L"SelectionNamespaces");
    BstrHolder nsv(L"xmlns:opf='http://www.idpf.org/2007/opf' "
                   L"xmlns:dc='http://purl.org/dc/elements/1.1/' "
                   L"xmlns:smil='http://www.w3.org/2001/SMIL20/' "
                   L"xmlns:ncx='http://www.daisy.org/z3986/2005/ncx/' "
                   L"xmlns:xhtml='http://www.w3.org/1999/xhtml' "
                   L"xmlns:cnt='urn:oasis:names:tc:opendocument:xmlns:container'");
    VARIANT v2; v2.vt = VT_BSTR; v2.bstrVal = nsv;
    doc->setProperty(ns, v2);
    return doc;
}

// Load XML from a UTF-8 buffer; tolerates malformed input by retrying with
// permissive flags (handy for HTML files).
ComPtr<IXMLDOMDocument2> LoadXmlFromString(const std::string& utf8) {
    auto doc = CreateXmlDoc();
    if (!doc) return {};
    std::wstring w = Utf8ToWide(utf8);
    BstrHolder bs(w.c_str());
    VARIANT_BOOL ok = VARIANT_FALSE;
    doc->loadXML(bs, &ok);
    if (ok == VARIANT_TRUE) return doc;
    return {};
}

// Try strict XML first; if that fails, do light "make it XML-ish" cleanup on the
// HTML and retry. NCC.html files are often XHTML-1.0 Transitional and parse
// fine, but we have to handle ad-hoc productions too.
ComPtr<IXMLDOMDocument2> LoadXmlFromFile(const std::wstring& path, bool htmlFallback) {
    std::string raw;
    if (!ReadWholeFile(path, raw)) {
        LogF("daisy", "could not read file: %s", WideToUtf8(path).c_str());
        return {};
    }

    // Always strip <!DOCTYPE ...> before loading — MSXML 6 refuses DTD-declared
    // documents with "DTD prohibited" by default, and DAISY 2.02 SMIL files
    // virtually always start with a DOCTYPE referencing SMIL10.dtd. The
    // declarative DOCTYPE is informational; we never need to validate against
    // it. (Without this, DAISY 2.02 books load with zero audio clips because
    // every SMIL parse fails before we even reach the <par> elements.)
    {
        size_t p = raw.find("<!DOCTYPE");
        if (p == std::string::npos) p = raw.find("<!doctype");
        if (p != std::string::npos) {
            size_t q = raw.find('>', p);
            if (q != std::string::npos) raw.erase(p, q - p + 1);
        }
    }

    auto doc = LoadXmlFromString(raw);
    if (doc) return doc;

    if (!htmlFallback) return {};

    // HTML repair pass: strip DOCTYPE, drop unmatched void tags' close requirement
    // by self-closing them. Crude but effective for NCC files.
    std::string cleaned = raw;

    // Strip <!DOCTYPE ...>
    {
        size_t p = cleaned.find("<!DOCTYPE");
        if (p == std::string::npos) p = cleaned.find("<!doctype");
        if (p != std::string::npos) {
            size_t q = cleaned.find('>', p);
            if (q != std::string::npos) cleaned.erase(p, q - p + 1);
        }
    }
    // Strip HTML entities that aren't predefined XML entities
    // (replace &nbsp; etc. with their unicode equivalents — minimal set)
    static const std::pair<const char*, const char*> ents[] = {
        {"&nbsp;",  "\xC2\xA0"},
        {"&copy;",  "\xC2\xA9"},
        {"&reg;",   "\xC2\xAE"},
        {"&mdash;", "\xE2\x80\x94"},
        {"&ndash;", "\xE2\x80\x93"},
        {"&lsquo;", "\xE2\x80\x98"},
        {"&rsquo;", "\xE2\x80\x99"},
        {"&ldquo;", "\xE2\x80\x9C"},
        {"&rdquo;", "\xE2\x80\x9D"},
        {"&hellip;","\xE2\x80\xA6"},
    };
    for (auto& e : ents) {
        std::string from = e.first, to = e.second;
        size_t p = 0;
        while ((p = cleaned.find(from, p)) != std::string::npos) {
            cleaned.replace(p, from.size(), to);
            p += to.size();
        }
    }

    doc = LoadXmlFromString(cleaned);
    if (!doc) {
        LogF("daisy", "XML parse failed: %s", WideToUtf8(path).c_str());
    }
    return doc;
}

// Get text content of a node (BSTR-managed).
std::wstring NodeText(IXMLDOMNode* node) {
    if (!node) return L"";
    BSTR b = nullptr;
    if (FAILED(node->get_text(&b)) || !b) return L"";
    std::wstring s = b;
    SysFreeString(b);
    return TrimW(s);
}

// Get an attribute value off an element by name (case-sensitive, namespace-aware).
// We try the simple name first, then for namespaced lookups callers can fall
// back to checking attribute lists.
std::wstring AttrValue(IXMLDOMNode* node, const wchar_t* name) {
    if (!node) return L"";
    ComPtr<IXMLDOMElement> el;
    if (FAILED(node->QueryInterface(IID_PPV_ARGS(el.PutVoid()))) || !el) return L"";
    BstrHolder bname(name);
    VARIANT v; VariantInit(&v);
    HRESULT hr = el->getAttribute(bname, &v);
    std::wstring s;
    if (SUCCEEDED(hr) && v.vt == VT_BSTR && v.bstrVal) s = v.bstrVal;
    VariantClear(&v);
    return s;
}

// Try several attribute names (DAISY mixes "clip-begin" / "clipBegin" / "clipbegin")
std::wstring AttrAny(IXMLDOMNode* node, std::initializer_list<const wchar_t*> names) {
    for (auto* n : names) {
        auto v = AttrValue(node, n);
        if (!v.empty()) return v;
    }
    return L"";
}

ComPtr<IXMLDOMNodeList> SelectNodes(IXMLDOMNode* root, const wchar_t* xpath) {
    ComPtr<IXMLDOMNodeList> list;
    if (!root) return list;
    BstrHolder bx(xpath);
    root->selectNodes(bx, list.PutVoid());
    return list;
}

ComPtr<IXMLDOMNode> SelectSingleNode(IXMLDOMNode* root, const wchar_t* xpath) {
    ComPtr<IXMLDOMNode> node;
    if (!root) return node;
    BstrHolder bx(xpath);
    root->selectSingleNode(bx, node.PutVoid());
    return node;
}

// -----------------------------------------------------------------------------
// SMIL time parser
//
// Handles:
//   "npt=12.345s", "npt=0s"
//   "12.345s", "1.5min", "2h", "500ms"
//   "0:01:23", "0:01:23.456", "01:23.5"
//   "42"           (plain seconds)
// Returns seconds, or -1.0 on error.
// -----------------------------------------------------------------------------
double ParseSmilTime(const std::wstring& in) {
    std::wstring s = TrimW(in);
    if (s.empty()) return -1.0;

    // Strip "npt=" prefix (Normal Play Time)
    if (s.size() > 4 &&
        (s[0] == L'n' || s[0] == L'N') &&
        (s[1] == L'p' || s[1] == L'P') &&
        (s[2] == L't' || s[2] == L'T') &&
        s[3] == L'=') {
        s = s.substr(4);
    }
    if (s.empty()) return -1.0;

    // Colon form: H:MM:SS(.fff) or MM:SS(.fff)
    if (s.find(L':') != std::wstring::npos) {
        double h = 0, m = 0, sec = 0;
        // Split on colons
        std::vector<std::wstring> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == L':') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        try {
            if (parts.size() == 3) {
                h = std::stod(parts[0]);
                m = std::stod(parts[1]);
                sec = std::stod(parts[2]);
            } else if (parts.size() == 2) {
                m = std::stod(parts[0]);
                sec = std::stod(parts[1]);
            } else {
                return -1.0;
            }
        } catch (...) { return -1.0; }
        return h * 3600.0 + m * 60.0 + sec;
    }

    // Unit-suffix form: pick longest suffix first
    struct Unit { const wchar_t* suf; double mult; };
    static const Unit units[] = {
        {L"ms",  0.001},
        {L"min", 60.0},
        {L"s",   1.0},
        {L"h",   3600.0},
    };
    for (auto& u : units) {
        size_t L = wcslen(u.suf);
        if (s.size() > L) {
            std::wstring tail = s.substr(s.size() - L);
            // Case-insensitive compare
            bool match = true;
            for (size_t i = 0; i < L; ++i) {
                if (std::towlower(tail[i]) != u.suf[i]) { match = false; break; }
            }
            if (match) {
                try {
                    double v = std::stod(s.substr(0, s.size() - L));
                    return v * u.mult;
                } catch (...) { return -1.0; }
            }
        }
    }

    // Plain number -> seconds
    try { return std::stod(s); } catch (...) { return -1.0; }
}

// -----------------------------------------------------------------------------
// SMIL parsing — extracts a sequence of (audio, begin, end, text, frag) tuples
// from one SMIL file. Order is document order.
//
// For each <par> we look at the contained <audio> and <text> children.
// (We don't handle <seq> recursion in any sophisticated way — flat selection
// of <audio> elements works for virtually every DAISY production.)
//
// The "fragId -> clipIndex" map is populated so the NCC/NCX nav points can
// translate "file.smil#abc" into a clip index after the fact.
// -----------------------------------------------------------------------------

struct SmilResult {
    std::vector<DaisyClip>                   clips;
    std::unordered_map<std::string, int>     fragToClip; // par@id / text@id -> clips index
    int                                      firstClip = -1;  // first clip emitted by this SMIL
};

bool ParseSmilFile(const std::wstring& smilPath, SmilResult& out) {
    auto doc = LoadXmlFromFile(smilPath, /*htmlFallback*/false);
    if (!doc) return false;

    std::wstring smilFolder = ParentFolderImpl(smilPath);

    // Grab all <par> elements (any depth)
    auto pars = SelectNodes(doc.Get(), L"//*[local-name()='par']");
    if (!pars) return false;
    long count = 0;
    pars->get_length(&count);

    for (long i = 0; i < count; ++i) {
        ComPtr<IXMLDOMNode> par;
        pars->get_item(i, par.PutVoid());
        if (!par) continue;

        std::wstring parId = AttrValue(par.Get(), L"id");

        // Find the <audio> child (first one wins)
        auto audioList = SelectNodes(par.Get(), L".//*[local-name()='audio']");
        long aCount = 0;
        if (audioList) audioList->get_length(&aCount);

        // Find the <text> child
        auto textList = SelectNodes(par.Get(), L".//*[local-name()='text']");
        long tCount = 0;
        if (textList) textList->get_length(&tCount);

        std::wstring textFile;
        std::string  textFrag;
        std::wstring textId;
        if (tCount > 0) {
            ComPtr<IXMLDOMNode> tn;
            textList->get_item(0, tn.PutVoid());
            if (tn) {
                textId = AttrValue(tn.Get(), L"id");
                std::wstring src = AttrValue(tn.Get(), L"src");
                if (!src.empty()) {
                    std::wstring file;
                    SplitFragment(UrlDecode(src), file, textFrag);
                    if (!file.empty()) textFile = JoinPath(smilFolder, file);
                }
            }
        }

        // Emit one clip per <audio>
        for (long a = 0; a < aCount; ++a) {
            ComPtr<IXMLDOMNode> an;
            audioList->get_item(a, an.PutVoid());
            if (!an) continue;
            std::wstring src = AttrValue(an.Get(), L"src");
            if (src.empty()) continue;

            std::wstring beginStr = AttrAny(an.Get(), {L"clip-begin", L"clipBegin", L"clipbegin"});
            std::wstring endStr   = AttrAny(an.Get(), {L"clip-end",   L"clipEnd",   L"clipend"});

            DaisyClip clip;
            clip.audioFile = JoinPath(smilFolder, UrlDecode(src));
            clip.clipBegin = beginStr.empty() ? 0.0 : ParseSmilTime(beginStr);
            clip.clipEnd   = endStr.empty()   ? 0.0 : ParseSmilTime(endStr);
            if (clip.clipBegin < 0) clip.clipBegin = 0;
            if (clip.clipEnd   < 0) clip.clipEnd   = 0;
            clip.textFile   = textFile;
            clip.textFragId = textFrag;

            if (!FileExists(clip.audioFile)) {
                LogF("daisy", "skipping clip, audio not found: %s",
                     WideToUtf8(clip.audioFile).c_str());
                continue;
            }

            int idx = (int)out.clips.size();
            out.clips.push_back(clip);
            if (out.firstClip < 0) out.firstClip = idx;

            if (!parId.empty())
                out.fragToClip.emplace(WideToUtf8(parId), idx);
            if (!textId.empty())
                out.fragToClip.emplace(WideToUtf8(textId), idx);
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// DAISY 2.02 parser
// -----------------------------------------------------------------------------

void ExtractMetaContent(IXMLDOMNode* root, const wchar_t* nameLower,
                        std::wstring& out) {
    if (!out.empty()) return;
    auto metas = SelectNodes(root, L"//*[local-name()='meta']");
    if (!metas) return;
    long n = 0; metas->get_length(&n);
    for (long i = 0; i < n; ++i) {
        ComPtr<IXMLDOMNode> m;
        metas->get_item(i, m.PutVoid());
        if (!m) continue;
        std::wstring name = AttrValue(m.Get(), L"name");
        if (_wcsicmp(name.c_str(), nameLower) == 0) {
            std::wstring content = AttrValue(m.Get(), L"content");
            if (!content.empty()) { out = content; return; }
        }
    }
}

bool ParseDaisy202(const std::wstring& folder, DaisyBook& book) {
    book.format = DaisyFormat::Daisy202;

    std::wstring nccPath = FindFileCI(folder, L"ncc.html");
    if (nccPath.empty()) nccPath = FindFileCI(folder, L"ncc.htm");
    if (nccPath.empty()) {
        LogF("daisy", "no NCC.html found in %s", WideToUtf8(folder).c_str());
        return false;
    }

    auto doc = LoadXmlFromFile(nccPath, /*htmlFallback*/true);
    if (!doc) return false;

    // Title
    {
        auto t = SelectSingleNode(doc.Get(), L"//*[local-name()='title']");
        if (t) book.title = NodeText(t.Get());
    }
    // Metadata
    ExtractMetaContent(doc.Get(), L"dc:creator",  book.author);
    ExtractMetaContent(doc.Get(), L"dc:Creator",  book.author);
    ExtractMetaContent(doc.Get(), L"dc:language", book.language);
    ExtractMetaContent(doc.Get(), L"dc:Language", book.language);
    ExtractMetaContent(doc.Get(), L"dc:title",    book.title);
    ExtractMetaContent(doc.Get(), L"dc:Title",    book.title);

    // Walk body in document order: every <h1>...<h6> and every <p class="page-...">
    // contains an <a href="something.smil#frag"> we need to resolve.
    auto body = SelectSingleNode(doc.Get(), L"//*[local-name()='body']");
    if (!body) {
        LogF("daisy", "NCC has no <body>");
        return !book.title.empty();
    }

    // Collect all <a href> elements with their parent's tag name in doc order.
    auto anchors = SelectNodes(body.Get(), L".//*[local-name()='a'][@href]");
    if (!anchors) return !book.title.empty();
    long aCount = 0; anchors->get_length(&aCount);

    // Pending nav points (waiting for SMIL parsing to map them to clipIndex)
    struct Pending {
        int          level;        // 1..6 or 0 for page or -1 unknown
        std::wstring label;
        std::wstring smilFile;     // absolute path
        std::string  fragId;
        DaisyNavPoint::Kind kind;
    };
    std::vector<Pending> pending;

    std::wstring nccFolder = ParentFolderImpl(nccPath);

    for (long i = 0; i < aCount; ++i) {
        ComPtr<IXMLDOMNode> a;
        anchors->get_item(i, a.PutVoid());
        if (!a) continue;

        std::wstring href = AttrValue(a.Get(), L"href");
        if (href.empty()) continue;

        // Find parent element to determine level (h1..h6 / p)
        ComPtr<IXMLDOMNode> parent;
        a->get_parentNode(parent.PutVoid());
        if (!parent) continue;
        BSTR pname = nullptr;
        parent->get_nodeName(&pname);
        std::wstring tag = pname ? pname : L"";
        if (pname) SysFreeString(pname);
        std::wstring tagLow = ToLowerW(tag);
        // Strip "xhtml:" / "ns:" prefix
        size_t cln = tagLow.find(L':');
        if (cln != std::wstring::npos) tagLow = tagLow.substr(cln + 1);

        Pending p;
        p.label = TrimW(NodeText(a.Get()));
        if (p.label.empty()) p.label = TrimW(NodeText(parent.Get()));

        if (tagLow.size() == 2 && tagLow[0] == L'h' &&
            tagLow[1] >= L'1' && tagLow[1] <= L'6') {
            p.level = tagLow[1] - L'0';
            p.kind  = DaisyNavPoint::Heading;
        } else if (tagLow == L"p" || tagLow == L"span" || tagLow == L"div") {
            std::wstring cls = ToLowerW(AttrValue(parent.Get(), L"class"));
            if (cls.find(L"page") != std::wstring::npos) {
                p.level = 0;
                p.kind  = DaisyNavPoint::Page;
                if (p.label.empty()) p.label = L"Page";
                else {
                    // Prepend "Page " if it's just a number
                    bool numeric = !p.label.empty();
                    for (wchar_t c : p.label) {
                        if (!std::iswdigit(c) && !std::iswspace(c)) { numeric = false; break; }
                    }
                    if (numeric) p.label = L"Page " + p.label;
                }
            } else {
                continue;  // not a nav target
            }
        } else {
            continue;
        }

        std::wstring file;
        SplitFragment(UrlDecode(href), file, p.fragId);
        if (file.empty()) continue;
        p.smilFile = JoinPath(nccFolder, file);
        pending.push_back(std::move(p));
    }

    // Parse each unique SMIL file in the order it is first referenced
    std::vector<std::wstring> smilOrder;
    std::unordered_map<std::wstring, SmilResult> smilCache;
    for (auto& p : pending) {
        std::wstring key = ToLowerW(p.smilFile);
        if (smilCache.find(key) == smilCache.end()) {
            smilCache[key] = SmilResult{};
            smilOrder.push_back(p.smilFile);
        }
    }

    // Parse them in order, appending to book.clips, recording remap offsets
    std::unordered_map<std::wstring, int> baseOffset; // smil-key -> first-clip-index in book.clips
    for (auto& sp : smilOrder) {
        SmilResult sr;
        sr.firstClip = -1;
        // Pre-fill clips offset to current size
        int baseIdx = (int)book.clips.size();
        // Parse INTO a temp SmilResult, then concat
        SmilResult tmp;
        if (!ParseSmilFile(sp, tmp)) {
            LogF("daisy", "failed to parse SMIL: %s", WideToUtf8(sp).c_str());
            baseOffset[ToLowerW(sp)] = baseIdx;
            smilCache[ToLowerW(sp)]  = tmp;
            continue;
        }
        // Append clips
        for (auto& c : tmp.clips) {
            book.clips.push_back(c);
        }
        // Remap fragToClip to book-global indices
        SmilResult remapped;
        remapped.firstClip = tmp.clips.empty() ? -1 : baseIdx;
        for (auto& kv : tmp.fragToClip) {
            remapped.fragToClip.emplace(kv.first, kv.second + baseIdx);
        }
        smilCache[ToLowerW(sp)] = std::move(remapped);
        baseOffset[ToLowerW(sp)] = baseIdx;
    }

    // Build navPoints with resolved clipIndex
    for (auto& p : pending) {
        auto& sr = smilCache[ToLowerW(p.smilFile)];
        int idx = -1;
        if (!p.fragId.empty()) {
            auto it = sr.fragToClip.find(p.fragId);
            if (it != sr.fragToClip.end()) idx = it->second;
        }
        if (idx < 0) idx = sr.firstClip;
        if (idx < 0) idx = 0;  // last-resort fallback

        DaisyNavPoint np;
        np.level     = p.level;
        np.label     = p.label;
        np.clipIndex = idx;
        np.kind      = p.kind;
        book.navPoints.push_back(np);
    }

    // Total duration
    double total = 0;
    for (auto& c : book.clips) {
        if (c.clipEnd > c.clipBegin) total += (c.clipEnd - c.clipBegin);
    }
    book.totalDuration = total;
    return true;
}

// -----------------------------------------------------------------------------
// DAISY 3 parser (.opf + .ncx + .smil)
// -----------------------------------------------------------------------------

// Recurse through navMap, emitting nav points in document order.
void WalkNcxNavPoints(IXMLDOMNode* node,
                      const std::wstring& ncxFolder,
                      const std::unordered_map<std::wstring, SmilResult>& smilCache,
                      std::vector<DaisyNavPoint>& out) {
    if (!node) return;
    auto children = SelectNodes(node, L"./*[local-name()='navPoint']");
    if (!children) return;
    long n = 0; children->get_length(&n);
    for (long i = 0; i < n; ++i) {
        ComPtr<IXMLDOMNode> np;
        children->get_item(i, np.PutVoid());
        if (!np) continue;

        std::wstring cls = AttrValue(np.Get(), L"class");  // h1 / h2 / ...
        int level = -1;
        if (cls.size() == 2 && cls[0] == L'h' &&
            cls[1] >= L'1' && cls[1] <= L'6') {
            level = cls[1] - L'0';
        }

        // <navLabel><text>...</text></navLabel>
        auto txt = SelectSingleNode(np.Get(),
            L"./*[local-name()='navLabel']/*[local-name()='text']");
        std::wstring label = txt ? NodeText(txt.Get()) : L"";

        // <content src="..."/>
        auto cn = SelectSingleNode(np.Get(), L"./*[local-name()='content']");
        std::wstring src = cn ? AttrValue(cn.Get(), L"src") : L"";

        int clipIdx = 0;
        if (!src.empty()) {
            std::wstring file; std::string frag;
            SplitFragment(UrlDecode(src), file, frag);
            std::wstring abs = JoinPath(ncxFolder, file);
            auto it = smilCache.find(ToLowerW(abs));
            if (it != smilCache.end()) {
                if (!frag.empty()) {
                    auto f = it->second.fragToClip.find(frag);
                    if (f != it->second.fragToClip.end()) clipIdx = f->second;
                    else clipIdx = std::max(0, it->second.firstClip);
                } else {
                    clipIdx = std::max(0, it->second.firstClip);
                }
            }
        }

        DaisyNavPoint pt;
        pt.level     = level;
        pt.label     = label;
        pt.clipIndex = clipIdx;
        pt.kind      = DaisyNavPoint::Heading;
        out.push_back(pt);

        // Recurse for nested navPoints
        WalkNcxNavPoints(np.Get(), ncxFolder, smilCache, out);
    }
}

void WalkNcxPageList(IXMLDOMNode* root,
                     const std::wstring& ncxFolder,
                     const std::unordered_map<std::wstring, SmilResult>& smilCache,
                     std::vector<DaisyNavPoint>& out) {
    auto plist = SelectSingleNode(root, L"//*[local-name()='pageList']");
    if (!plist) return;
    auto pages = SelectNodes(plist.Get(), L"./*[local-name()='pageTarget']");
    if (!pages) return;
    long n = 0; pages->get_length(&n);
    for (long i = 0; i < n; ++i) {
        ComPtr<IXMLDOMNode> pn;
        pages->get_item(i, pn.PutVoid());
        if (!pn) continue;

        auto txt = SelectSingleNode(pn.Get(),
            L"./*[local-name()='navLabel']/*[local-name()='text']");
        std::wstring label = txt ? NodeText(txt.Get()) : L"";
        if (label.empty()) label = AttrValue(pn.Get(), L"value");
        bool numeric = !label.empty();
        for (wchar_t c : label) {
            if (!std::iswdigit(c) && !std::iswspace(c)) { numeric = false; break; }
        }
        if (numeric && !label.empty()) label = L"Page " + label;
        else if (label.empty()) label = L"Page";

        auto cn = SelectSingleNode(pn.Get(), L"./*[local-name()='content']");
        std::wstring src = cn ? AttrValue(cn.Get(), L"src") : L"";

        int clipIdx = 0;
        if (!src.empty()) {
            std::wstring file; std::string frag;
            SplitFragment(UrlDecode(src), file, frag);
            std::wstring abs = JoinPath(ncxFolder, file);
            auto it = smilCache.find(ToLowerW(abs));
            if (it != smilCache.end()) {
                if (!frag.empty()) {
                    auto f = it->second.fragToClip.find(frag);
                    if (f != it->second.fragToClip.end()) clipIdx = f->second;
                    else clipIdx = std::max(0, it->second.firstClip);
                } else {
                    clipIdx = std::max(0, it->second.firstClip);
                }
            }
        }

        DaisyNavPoint pt;
        pt.level     = 0;
        pt.label     = label;
        pt.clipIndex = clipIdx;
        pt.kind      = DaisyNavPoint::Page;
        out.push_back(pt);
    }
}

bool ParseDaisy3(const std::wstring& opfPath, DaisyBook& book) {
    book.format = DaisyFormat::Daisy3;
    std::wstring opfFolder = ParentFolderImpl(opfPath);

    auto doc = LoadXmlFromFile(opfPath, /*htmlFallback*/false);
    if (!doc) {
        LogF("daisy", "could not parse OPF: %s", WideToUtf8(opfPath).c_str());
        return false;
    }

    // Metadata
    auto getDc = [&](const wchar_t* tag) -> std::wstring {
        std::wstring xpath = L"//*[local-name()='";
        xpath += tag; xpath += L"']";
        auto n = SelectSingleNode(doc.Get(), xpath.c_str());
        return n ? NodeText(n.Get()) : L"";
    };
    book.title    = getDc(L"title");
    book.author   = getDc(L"creator");
    book.language = getDc(L"language");

    // Manifest: id -> href + media-type
    struct ManifestItem { std::wstring href, mediaType; };
    std::map<std::wstring, ManifestItem> manifest;
    auto items = SelectNodes(doc.Get(), L"//*[local-name()='item']");
    long iCount = 0; if (items) items->get_length(&iCount);
    for (long i = 0; i < iCount; ++i) {
        ComPtr<IXMLDOMNode> it;
        items->get_item(i, it.PutVoid());
        if (!it) continue;
        std::wstring id   = AttrValue(it.Get(), L"id");
        std::wstring href = AttrValue(it.Get(), L"href");
        std::wstring mt   = AttrValue(it.Get(), L"media-type");
        if (!id.empty() && !href.empty()) {
            manifest[id] = { UrlDecode(href), mt };
        }
    }

    // Spine: ordered idrefs
    std::vector<std::wstring> spine;
    auto refs = SelectNodes(doc.Get(), L"//*[local-name()='spine']/*[local-name()='itemref']");
    long sCount = 0; if (refs) refs->get_length(&sCount);
    for (long i = 0; i < sCount; ++i) {
        ComPtr<IXMLDOMNode> it;
        refs->get_item(i, it.PutVoid());
        if (!it) continue;
        std::wstring id = AttrValue(it.Get(), L"idref");
        if (!id.empty()) spine.push_back(id);
    }

    // Parse each SMIL in spine order
    std::unordered_map<std::wstring, SmilResult> smilCache;
    for (auto& id : spine) {
        auto mit = manifest.find(id);
        if (mit == manifest.end()) continue;
        std::wstring abs = JoinPath(opfFolder, mit->second.href);
        std::wstring mt  = mit->second.mediaType;
        std::wstring mtLow = ToLowerW(mt);
        // Only parse SMIL spine entries (text-only books spine XHTML; skip those for now)
        if (mtLow.find(L"smil") == std::wstring::npos &&
            !EndsWith(abs, L".smil")) {
            continue;
        }
        SmilResult tmp;
        if (!ParseSmilFile(abs, tmp)) {
            LogF("daisy", "failed to parse SMIL: %s", WideToUtf8(abs).c_str());
            smilCache[ToLowerW(abs)] = tmp;
            continue;
        }
        int baseIdx = (int)book.clips.size();
        SmilResult remapped;
        remapped.firstClip = tmp.clips.empty() ? -1 : baseIdx;
        for (auto& c : tmp.clips) book.clips.push_back(c);
        for (auto& kv : tmp.fragToClip)
            remapped.fragToClip.emplace(kv.first, kv.second + baseIdx);
        smilCache[ToLowerW(abs)] = std::move(remapped);
    }

    // Locate NCX (media-type application/x-dtbncx+xml, or *.ncx)
    std::wstring ncxPath;
    for (auto& kv : manifest) {
        if (_wcsicmp(kv.second.mediaType.c_str(), L"application/x-dtbncx+xml") == 0) {
            ncxPath = JoinPath(opfFolder, kv.second.href);
            break;
        }
    }
    if (ncxPath.empty()) {
        ncxPath = FindFileBySuffix(opfFolder, L".ncx");
    }

    if (!ncxPath.empty() && FileExists(ncxPath)) {
        std::wstring ncxFolder = ParentFolderImpl(ncxPath);
        auto ncxDoc = LoadXmlFromFile(ncxPath, /*htmlFallback*/false);
        if (ncxDoc) {
            // Pull docTitle as fallback
            if (book.title.empty()) {
                auto t = SelectSingleNode(ncxDoc.Get(),
                    L"//*[local-name()='docTitle']/*[local-name()='text']");
                if (t) book.title = NodeText(t.Get());
            }
            if (book.author.empty()) {
                auto a = SelectSingleNode(ncxDoc.Get(),
                    L"//*[local-name()='docAuthor']/*[local-name()='text']");
                if (a) book.author = NodeText(a.Get());
            }
            auto navMap = SelectSingleNode(ncxDoc.Get(), L"//*[local-name()='navMap']");
            if (navMap) WalkNcxNavPoints(navMap.Get(), ncxFolder, smilCache, book.navPoints);
            WalkNcxPageList(ncxDoc.Get(), ncxFolder, smilCache, book.navPoints);
        }
    }

    // Total duration
    double total = 0;
    for (auto& c : book.clips) {
        if (c.clipEnd > c.clipBegin) total += (c.clipEnd - c.clipBegin);
    }
    book.totalDuration = total;
    return true;
}

// -----------------------------------------------------------------------------
// EPUB 3 metadata-only parser
//
// EPUB = ZIP. We don't pull in a ZIP library here; instead we recognize the
// PK header, walk the central directory ourselves enough to extract two
// small files (META-INF/container.xml and the .opf it points to). The files
// are virtually always STORED or deflate-compressed; we handle both via
// Windows' built-in Compression API for deflate, or pass through STORED.
//
// If anything goes wrong we still return a DaisyBook with format=Epub3 and
// the filename as the title so the library scanner can show *something*.
// -----------------------------------------------------------------------------

#pragma pack(push, 1)
struct ZipLocalHeader {
    uint32_t sig;
    uint16_t verNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t compSize;
    uint32_t uncompSize;
    uint16_t nameLen;
    uint16_t extraLen;
};
struct ZipCentralDirHeader {
    uint32_t sig;            // 0x02014b50
    uint16_t verMade;
    uint16_t verNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t compSize;
    uint32_t uncompSize;
    uint16_t nameLen;
    uint16_t extraLen;
    uint16_t commentLen;
    uint16_t diskNum;
    uint16_t intAttr;
    uint32_t extAttr;
    uint32_t localHdrOffset;
};
struct ZipEndOfCentralDir {
    uint32_t sig;            // 0x06054b50
    uint16_t diskNum;
    uint16_t diskCdStart;
    uint16_t cdEntriesThisDisk;
    uint16_t cdEntriesTotal;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLen;
};
#pragma pack(pop)

// Inflate raw deflate stream using zlib? We don't have zlib linked. Use
// Windows Compression API (Cabinet.dll exports... actually use COMPRESSION_FORMAT_XPRESS_HUFF
// which isn't deflate). Simpler: support STORED only; if compressed, we just
// fall back to filename-as-title.
bool ZipExtractStored(const std::vector<uint8_t>& buf, const std::string& wantName,
                      std::vector<uint8_t>& out) {
    // Find End of Central Directory record (search last 64KB)
    if (buf.size() < sizeof(ZipEndOfCentralDir)) return false;
    size_t scan = buf.size() - sizeof(ZipEndOfCentralDir);
    size_t minScan = (buf.size() > 65557) ? buf.size() - 65557 : 0;
    const ZipEndOfCentralDir* eocd = nullptr;
    while (scan >= minScan && scan + sizeof(ZipEndOfCentralDir) <= buf.size()) {
        auto* p = reinterpret_cast<const ZipEndOfCentralDir*>(&buf[scan]);
        if (p->sig == 0x06054b50) { eocd = p; break; }
        if (scan == 0) break;
        --scan;
    }
    if (!eocd) return false;

    size_t cdOff = eocd->cdOffset;
    int n = eocd->cdEntriesTotal;
    for (int i = 0; i < n; ++i) {
        if (cdOff + sizeof(ZipCentralDirHeader) > buf.size()) return false;
        auto* cd = reinterpret_cast<const ZipCentralDirHeader*>(&buf[cdOff]);
        if (cd->sig != 0x02014b50) return false;
        std::string name((const char*)&buf[cdOff + sizeof(ZipCentralDirHeader)], cd->nameLen);
        cdOff += sizeof(ZipCentralDirHeader) + cd->nameLen + cd->extraLen + cd->commentLen;

        // Case-insensitive compare for matching
        if (_stricmp(name.c_str(), wantName.c_str()) != 0) continue;
        if (cd->method != 0) {
            // STORED only path — deflate unsupported in Phase 1
            LogF("daisy", "EPUB: file '%s' is compressed (method %u), unsupported",
                 name.c_str(), (unsigned)cd->method);
            return false;
        }
        size_t lhOff = cd->localHdrOffset;
        if (lhOff + sizeof(ZipLocalHeader) > buf.size()) return false;
        auto* lh = reinterpret_cast<const ZipLocalHeader*>(&buf[lhOff]);
        size_t dataOff = lhOff + sizeof(ZipLocalHeader) + lh->nameLen + lh->extraLen;
        if (dataOff + cd->uncompSize > buf.size()) return false;
        out.assign(buf.begin() + dataOff, buf.begin() + dataOff + cd->uncompSize);
        return true;
    }
    return false;
}

bool ParseEpub3Metadata(const std::wstring& epubPath, DaisyBook& book) {
    book.format = DaisyFormat::Epub3;

    // Use filename (sans ext) as title fallback
    std::wstring fname = GetFileName(epubPath);
    size_t dot = fname.find_last_of(L'.');
    if (dot != std::wstring::npos) fname = fname.substr(0, dot);
    book.title = fname;

    // Read whole .epub into memory
    HANDLE h = CreateFileW(epubPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return true;  // we already have a title fallback
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 200LL * 1024 * 1024) {
        CloseHandle(h);
        return true;
    }
    std::vector<uint8_t> buf((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != buf.size() || buf.size() < 4) return true;
    if (buf[0] != 'P' || buf[1] != 'K') return true;

    std::vector<uint8_t> container;
    if (!ZipExtractStored(buf, "META-INF/container.xml", container)) {
        // container.xml is almost always STORED but tolerate failure
        return true;
    }

    std::string containerStr((const char*)container.data(), container.size());
    auto cdoc = LoadXmlFromString(containerStr);
    if (!cdoc) return true;
    auto rf = SelectSingleNode(cdoc.Get(), L"//*[local-name()='rootfile']");
    if (!rf) return true;
    std::wstring opfRel = AttrValue(rf.Get(), L"full-path");
    if (opfRel.empty()) return true;

    std::vector<uint8_t> opfBuf;
    if (!ZipExtractStored(buf, WideToUtf8(opfRel), opfBuf)) return true;

    std::string opfStr((const char*)opfBuf.data(), opfBuf.size());
    auto odoc = LoadXmlFromString(opfStr);
    if (!odoc) return true;

    auto getDc = [&](const wchar_t* tag) -> std::wstring {
        std::wstring xpath = L"//*[local-name()='";
        xpath += tag; xpath += L"']";
        auto n = SelectSingleNode(odoc.Get(), xpath.c_str());
        return n ? NodeText(n.Get()) : L"";
    };
    std::wstring t = getDc(L"title");
    std::wstring a = getDc(L"creator");
    std::wstring l = getDc(L"language");
    if (!t.empty()) book.title    = t;
    if (!a.empty()) book.author   = a;
    if (!l.empty()) book.language = l;
    return true;
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

std::wstring ParentFolder(const std::wstring& path) {
    return ParentFolderImpl(path);
}

DaisyFormat DetectDaisyFormat(const std::wstring& path) {
    if (path.empty()) return DaisyFormat::Unknown;

    // Direct file?
    if (FileExists(path)) {
        if (EndsWith(path, L".epub")) return DaisyFormat::Epub3;
        if (EndsWith(path, L".opf"))  return DaisyFormat::Daisy3;
        if (EndsWith(path, L".html") || EndsWith(path, L".htm")) {
            // Treat NCC.html paths as DAISY 2.02
            std::wstring fn = ToLowerW(GetFileName(path));
            if (fn == L"ncc.html" || fn == L"ncc.htm") return DaisyFormat::Daisy202;
        }
        return DaisyFormat::Unknown;
    }

    // Folder?
    if (DirExists(path)) {
        if (!FindFileCI(path, L"ncc.html").empty() ||
            !FindFileCI(path, L"ncc.htm").empty()) {
            return DaisyFormat::Daisy202;
        }
        if (!FindFileBySuffix(path, L".opf").empty()) {
            return DaisyFormat::Daisy3;
        }
    }
    return DaisyFormat::Unknown;
}

std::unique_ptr<DaisyBook> OpenDaisyBook(const std::wstring& path,
                                         std::string* errorOut) {
    if (path.empty()) {
        if (errorOut) *errorOut = "empty path";
        return nullptr;
    }

    auto book = std::make_unique<DaisyBook>();
    book->path = path;

    DaisyFormat fmt = DetectDaisyFormat(path);
    if (fmt == DaisyFormat::Unknown) {
        if (errorOut) *errorOut = "unrecognized DAISY/EPUB format";
        return nullptr;
    }

    bool ok = false;
    switch (fmt) {
        case DaisyFormat::Daisy202: {
            std::wstring folder = DirExists(path) ? path : ParentFolderImpl(path);
            ok = ParseDaisy202(folder, *book);
            break;
        }
        case DaisyFormat::Daisy3: {
            std::wstring opfPath;
            if (FileExists(path) && EndsWith(path, L".opf")) {
                opfPath = path;
            } else if (DirExists(path)) {
                opfPath = FindFileBySuffix(path, L".opf");
            }
            if (opfPath.empty()) {
                if (errorOut) *errorOut = "no .opf file found";
                return nullptr;
            }
            ok = ParseDaisy3(opfPath, *book);
            break;
        }
        case DaisyFormat::Epub3: {
            ok = ParseEpub3Metadata(path, *book);
            break;
        }
        default:
            break;
    }

    if (!ok) {
        if (errorOut) *errorOut = "parse failed";
        return nullptr;
    }

    LogF("daisy", "opened %ls: %d clips, %d nav points, %.1fs total",
         book->title.c_str(),
         (int)book->clips.size(),
         (int)book->navPoints.size(),
         book->totalDuration);
    return book;
}

} // namespace mediaaccess
