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
#include <unordered_set>
#include <vector>

// miniz: DEFLATE / ZIP support for EPUB (Phase 2). Single-source amalgamation
// dropped in deps/miniz/. We only need the inflate side, but the amalgamation
// is small and well-tested so we include it whole.
#include "miniz.h"

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

// Lowercase + strip "ns:" prefix from a DOM node name. Used for tag dispatch
// when walking XHTML in a namespace-agnostic way.
std::wstring LocalTagLower(IXMLDOMNode* node) {
    if (!node) return L"";
    BSTR b = nullptr;
    if (FAILED(node->get_nodeName(&b)) || !b) return L"";
    std::wstring s = b;
    SysFreeString(b);
    s = ToLowerW(s);
    size_t c = s.find(L':');
    if (c != std::wstring::npos) s = s.substr(c + 1);
    return s;
}

// Collapse runs of whitespace (space/tab/CR/LF/NBSP) to a single space and trim.
std::wstring NormalizeWhitespace(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    bool prevSpace = true;  // leading whitespace gets eaten
    for (wchar_t c : in) {
        bool isSpace = (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
                        c == 0x00A0 /* NBSP */);
        if (isSpace) {
            if (!prevSpace) out.push_back(L' ');
            prevSpace = true;
        } else {
            out.push_back(c);
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// Get the (whitespace-normalized) plain text content of a DOM element. MSXML's
// get_text() concatenates descendant text nodes for us; we just normalize.
std::wstring ElementText(IXMLDOMNode* node) {
    if (!node) return L"";
    BSTR b = nullptr;
    if (FAILED(node->get_text(&b)) || !b) return L"";
    std::wstring s = b;
    SysFreeString(b);
    return NormalizeWhitespace(s);
}

// Cache parsed XHTML DOMs keyed by lowercased absolute path. Each XHTML file
// gets parsed exactly once across all the clips that reference it.
struct XhtmlCache {
    std::unordered_map<std::wstring, ComPtr<IXMLDOMDocument2>> docs;

    IXMLDOMDocument2* Get(const std::wstring& absPath) {
        std::wstring key = ToLowerW(absPath);
        auto it = docs.find(key);
        if (it != docs.end()) return it->second.Get();
        auto doc = LoadXmlFromFile(absPath, /*htmlFallback*/true);
        IXMLDOMDocument2* raw = doc.Get();
        docs.emplace(std::move(key), std::move(doc));
        return raw;
    }
};

// Look up an element by id inside a parsed XHTML doc. We try several common
// XPath shapes because XHTML produced by various DAISY tools uses different
// attribute styles (plain id="...", xml:id="...", and sometimes name="..." on
// <a> shims).
ComPtr<IXMLDOMNode> FindElementById(IXMLDOMDocument2* doc, const std::string& fragId) {
    if (!doc || fragId.empty()) return {};
    std::wstring wid = Utf8ToWide(fragId);

    // Escape any quotes that would break the XPath literal — extremely rare
    // but cheap insurance.
    std::wstring esc;
    esc.reserve(wid.size());
    for (wchar_t c : wid) {
        if (c == L'\'' || c == L'"') continue;  // skip rather than build concat()
        esc.push_back(c);
    }

    std::wstring xp = L"//*[@id='" + esc + L"']";
    auto n = SelectSingleNode(doc, xp.c_str());
    if (n) return n;

    xp = L"//*[@xml:id='" + esc + L"']";
    n = SelectSingleNode(doc, xp.c_str());
    if (n) return n;

    // Some tools wrap a <a name="..."> next to the target paragraph; walk to
    // the parent block as a best-effort.
    xp = L"//*[local-name()='a'][@name='" + esc + L"']/..";
    n = SelectSingleNode(doc, xp.c_str());
    return n;
}

// Populate clip.textContent for every clip that has a textFile/textFragId.
// XHTML files are parsed lazily through `cache`, so each file hits disk once.
void PopulateClipTextContent(std::vector<DaisyClip>& clips, XhtmlCache& cache) {
    for (auto& clip : clips) {
        if (clip.textFile.empty() || clip.textFragId.empty()) continue;
        if (!FileExists(clip.textFile)) continue;
        IXMLDOMDocument2* doc = cache.Get(clip.textFile);
        if (!doc) continue;
        auto node = FindElementById(doc, clip.textFragId);
        if (!node) continue;
        clip.textContent = ElementText(node.Get());
    }
}

// Block-level XHTML tags we emit as a DaisyTextSegment when walking text-only
// content. Order matches typical reading flow; we also stop descending into a
// matched element (a <li> inside a <ul> is its own segment, not part of the
// parent).
bool IsBlockTextTag(const std::wstring& tag) {
    return tag == L"p" || tag == L"blockquote" || tag == L"li" ||
           tag == L"dt" || tag == L"dd" || tag == L"pre" ||
           tag == L"caption" || tag == L"figcaption" ||
           tag == L"div";  // div is a catch-all if no inner block matches
}

int HeadingLevel(const std::wstring& tag) {
    if (tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6')
        return tag[1] - L'0';
    return -1;
}

// Phase 4 — SMIL <par> skippability. Walks the par's ancestor chain
// looking for a <seq class="..."> or <par class="..."> with a known
// skippable category. EPUB MO uses epub:type instead of class.
//
// Manages raw pointer AddRef/Release manually for the ancestor walk —
// our ComPtr wrapper doesn't support adoption of a borrowed raw pointer.
SkipKind ClassifySmilParSkipKind(IXMLDOMNode* par) {
    if (!par) return SkipKind::None;
    SkipKind result = SkipKind::None;
    IXMLDOMNode* cur = par;
    cur->AddRef();
    for (int depth = 0; depth < 6 && cur; ++depth) {
        std::wstring cls  = ToLowerW(AttrValue(cur, L"class"));
        std::wstring etyp = ToLowerW(AttrAny(cur, {L"epub:type", L"type"}));
        std::wstring combo = cls + L" " + etyp;
        if (combo.size() > 1) {
            if      (combo.find(L"pagebreak")         != std::wstring::npos) { result = SkipKind::Page;      break; }
            else if (combo.find(L"page-")             != std::wstring::npos) { result = SkipKind::Page;      break; }
            else if (combo.find(L"optional-prodnote") != std::wstring::npos) { result = SkipKind::Prodnote;  break; }
            else if (combo.find(L"prodnote")          != std::wstring::npos) { result = SkipKind::Prodnote;  break; }
            else if (combo.find(L"optional-sidebar")  != std::wstring::npos) { result = SkipKind::Sidebar;   break; }
            else if (combo.find(L"sidebar")           != std::wstring::npos) { result = SkipKind::Sidebar;   break; }
            else if (combo.find(L"footnote")          != std::wstring::npos) { result = SkipKind::Footnote;  break; }
            else if (combo.find(L"endnote")           != std::wstring::npos) { result = SkipKind::Footnote;  break; }
            else if (combo.find(L"noteref")           != std::wstring::npos) { result = SkipKind::Reference; break; }
            else if (combo.find(L"annotation")        != std::wstring::npos) { result = SkipKind::Note;      break; }
            else if (combo.find(L"note")              != std::wstring::npos) { result = SkipKind::Note;      break; }
        }
        IXMLDOMNode* parent = nullptr;
        cur->get_parentNode(&parent);
        cur->Release();
        cur = parent;
    }
    if (cur) cur->Release();
    return result;
}

// Phase 4 — classify an element's epub:type + class attributes into a
// SkipKind so the reader can optionally bypass it during playback.
// Returns SkipKind::None when no marker is present.
SkipKind ClassifySkipKind(IXMLDOMNode* el, int navLevel) {
    if (!el) return SkipKind::None;
    // navLevel == 0 means page-number span (already detected in caller).
    if (navLevel == 0) return SkipKind::Page;

    std::wstring epubType = ToLowerW(AttrAny(el, {L"epub:type", L"type"}));
    if (!epubType.empty()) {
        // epub:type can be a space-separated list; look for known tokens.
        if (epubType.find(L"pagebreak")  != std::wstring::npos) return SkipKind::Page;
        if (epubType.find(L"footnote")   != std::wstring::npos) return SkipKind::Footnote;
        if (epubType.find(L"endnote")    != std::wstring::npos) return SkipKind::Footnote;
        if (epubType.find(L"noteref")    != std::wstring::npos) return SkipKind::Reference;
        if (epubType.find(L"sidebar")    != std::wstring::npos) return SkipKind::Sidebar;
        if (epubType.find(L"annotation") != std::wstring::npos) return SkipKind::Note;
        if (epubType.find(L"note")       != std::wstring::npos) return SkipKind::Note;
    }
    std::wstring cls = ToLowerW(AttrValue(el, L"class"));
    if (!cls.empty()) {
        if (cls.find(L"optional-prodnote") != std::wstring::npos) return SkipKind::Prodnote;
        if (cls.find(L"prodnote")          != std::wstring::npos) return SkipKind::Prodnote;
        if (cls.find(L"optional-sidebar")  != std::wstring::npos) return SkipKind::Sidebar;
        if (cls.find(L"sidebar")           != std::wstring::npos) return SkipKind::Sidebar;
        if (cls.find(L"footnote")          != std::wstring::npos) return SkipKind::Footnote;
        if (cls.find(L"noteref")           != std::wstring::npos) return SkipKind::Reference;
        if (cls.find(L"note")              != std::wstring::npos) return SkipKind::Note;
        if (cls.find(L"page-")             != std::wstring::npos) return SkipKind::Page;
    }
    return SkipKind::None;
}

// Recursively walk a DOM subtree, emitting one DaisyTextSegment per
// "interesting" block element (headings, paragraphs, list items, page
// numbers). To avoid double-counting nested blocks we skip descent once we
// emit for an element — but we DO descend into containers like <div>, <body>,
// <section>, <article>, <nav>, <main>, <aside>, <header>, <footer>.
void WalkXhtmlForSegments(IXMLDOMNode* node,
                          const std::wstring& xhtmlFile,
                          std::vector<DaisyTextSegment>& out,
                          std::unordered_set<std::wstring>& seenSegments) {
    if (!node) return;

    ComPtr<IXMLDOMNodeList> children;
    node->get_childNodes(children.PutVoid());
    if (!children) return;
    long n = 0; children->get_length(&n);

    for (long i = 0; i < n; ++i) {
        ComPtr<IXMLDOMNode> ch;
        children->get_item(i, ch.PutVoid());
        if (!ch) continue;
        DOMNodeType nt = NODE_INVALID;
        ch->get_nodeType(&nt);
        if (nt != NODE_ELEMENT) continue;

        std::wstring tag = LocalTagLower(ch.Get());
        if (tag.empty()) continue;

        // Containers: just recurse, don't emit.
        if (tag == L"html" || tag == L"body" || tag == L"head" ||
            tag == L"section" || tag == L"article" || tag == L"nav" ||
            tag == L"main" || tag == L"aside" || tag == L"header" ||
            tag == L"footer" || tag == L"ul" || tag == L"ol" || tag == L"dl" ||
            tag == L"figure") {
            WalkXhtmlForSegments(ch.Get(), xhtmlFile, out, seenSegments);
            continue;
        }

        // Skip non-content tags entirely.
        if (tag == L"script" || tag == L"style" || tag == L"meta" ||
            tag == L"link" || tag == L"title") {
            continue;
        }

        int level = -1;
        bool emit = false;
        int hl = HeadingLevel(tag);
        if (hl >= 1) { level = hl; emit = true; }
        else if (IsBlockTextTag(tag)) {
            // For <div>, only emit if it doesn't contain a block child we'd
            // otherwise descend into. (Walk one level to check.)
            if (tag == L"div") {
                bool hasBlockChild = false;
                ComPtr<IXMLDOMNodeList> gc;
                ch->get_childNodes(gc.PutVoid());
                long gn = 0; if (gc) gc->get_length(&gn);
                for (long j = 0; j < gn; ++j) {
                    ComPtr<IXMLDOMNode> g;
                    gc->get_item(j, g.PutVoid());
                    if (!g) continue;
                    DOMNodeType gnt = NODE_INVALID;
                    g->get_nodeType(&gnt);
                    if (gnt != NODE_ELEMENT) continue;
                    std::wstring gt = LocalTagLower(g.Get());
                    if (HeadingLevel(gt) > 0 || IsBlockTextTag(gt) ||
                        gt == L"section" || gt == L"article" || gt == L"ul" ||
                        gt == L"ol" || gt == L"dl") {
                        hasBlockChild = true;
                        break;
                    }
                }
                if (hasBlockChild) {
                    WalkXhtmlForSegments(ch.Get(), xhtmlFile, out, seenSegments);
                    continue;
                }
            }
            level = -1;
            emit = true;
        } else if (tag == L"span") {
            // Page-number spans: class contains "page-" (DAISY 2.02 convention)
            std::wstring cls = ToLowerW(AttrValue(ch.Get(), L"class"));
            if (cls.find(L"page") != std::wstring::npos) {
                level = 0;
                emit = true;
            } else {
                continue;  // inline span, ignore
            }
        } else {
            // Some other inline-ish element: skip.
            continue;
        }

        if (emit) {
            std::wstring text = ElementText(ch.Get());
            if (text.empty()) continue;
            std::string id = WideToUtf8(AttrValue(ch.Get(), L"id"));

            // Dedup key — guards against re-walking shared anchors across spine entries
            std::wstring key = xhtmlFile + L"#" + Utf8ToWide(id) + L"\x1f" + text;
            if (!seenSegments.insert(key).second) continue;

            DaisyTextSegment seg;
            seg.text       = std::move(text);
            seg.textFile   = xhtmlFile;
            seg.textFragId = std::move(id);
            seg.navLevel   = level;
            seg.skipKind   = ClassifySkipKind(ch.Get(), level);
            out.push_back(std::move(seg));
        }
    }
}

// Convenience: walk a whole XHTML file (top-level) and append segments.
void AppendSegmentsFromFile(const std::wstring& xhtmlFile,
                            XhtmlCache& cache,
                            std::vector<DaisyTextSegment>& out) {
    if (xhtmlFile.empty() || !FileExists(xhtmlFile)) return;
    IXMLDOMDocument2* doc = cache.Get(xhtmlFile);
    if (!doc) return;
    auto body = SelectSingleNode(doc, L"//*[local-name()='body']");
    IXMLDOMNode* root = body.Get();
    ComPtr<IXMLDOMNode> docRoot;
    if (!root) {
        doc->QueryInterface(IID_PPV_ARGS(docRoot.PutVoid()));
        root = docRoot.Get();
    }
    std::unordered_set<std::wstring> seen;
    WalkXhtmlForSegments(root, xhtmlFile, out, seen);
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
            clip.skipKind   = ClassifySmilParSkipKind(par.Get());  // Phase 4

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

    // Phase 2: populate textContent for every clip whose <text> pointed into a
    // resolvable XHTML fragment. Each unique XHTML file is parsed once.
    {
        XhtmlCache xcache;
        PopulateClipTextContent(book.clips, xcache);

        // Text-only DAISY 2.02 (multimediaType=textNcc or no audio at all):
        // walk the NCC body (and any HTML referenced from it) as the text source.
        if (book.clips.empty()) {
            AppendSegmentsFromFile(nccPath, xcache, book.textSegments);
            // Some textNcc books split content across additional .html files
            // referenced by <a href> in the NCC. Walk those too in encounter order.
            std::unordered_set<std::wstring> seenFile;
            seenFile.insert(ToLowerW(nccPath));
            for (auto& p : pending) {
                // pending stores smilFile but for textNcc the href points at .html
                // — repurpose: any unique referenced file.
                std::wstring lk = ToLowerW(p.smilFile);
                if (seenFile.insert(lk).second &&
                    (EndsWith(p.smilFile, L".html") || EndsWith(p.smilFile, L".htm") ||
                     EndsWith(p.smilFile, L".xhtml"))) {
                    AppendSegmentsFromFile(p.smilFile, xcache, book.textSegments);
                }
            }
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

    // Parse each SMIL in spine order; remember XHTML spine entries for the
    // text-only fallback below.
    std::unordered_map<std::wstring, SmilResult> smilCache;
    std::vector<std::wstring> spineXhtml;  // absolute paths, spine order
    for (auto& id : spine) {
        auto mit = manifest.find(id);
        if (mit == manifest.end()) continue;
        std::wstring abs = JoinPath(opfFolder, mit->second.href);
        std::wstring mt  = mit->second.mediaType;
        std::wstring mtLow = ToLowerW(mt);
        bool isSmil = (mtLow.find(L"smil") != std::wstring::npos) ||
                      EndsWith(abs, L".smil");
        bool isXhtml = (mtLow.find(L"html") != std::wstring::npos) ||
                       EndsWith(abs, L".xhtml") || EndsWith(abs, L".html") ||
                       EndsWith(abs, L".htm");
        if (!isSmil) {
            if (isXhtml) spineXhtml.push_back(abs);
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

    // Phase 2: text content + text-only fallback.
    {
        XhtmlCache xcache;
        PopulateClipTextContent(book.clips, xcache);

        if (book.clips.empty()) {
            // Text-only DAISY 3 (or EPUB-like spine of XHTML). Walk spine
            // XHTML entries in order.
            for (auto& f : spineXhtml) {
                AppendSegmentsFromFile(f, xcache, book.textSegments);
            }
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
// EPUB 3 parser (miniz-backed ZIP reader)
//
// EPUB is a ZIP. miniz gives us read access to STORED and DEFLATE entries, so
// we can pull container.xml, the OPF, and (in Phase 2) every XHTML spine file
// for TTS playback when no Media Overlays are present.
// -----------------------------------------------------------------------------

// Open the .epub as a miniz archive on disk. miniz handles the file I/O so we
// don't have to buffer the whole archive in memory for large books.
struct EpubZip {
    mz_zip_archive zip;
    bool           open = false;

    EpubZip() { memset(&zip, 0, sizeof(zip)); }
    ~EpubZip() { if (open) mz_zip_reader_end(&zip); }

    bool Open(const std::wstring& path) {
        // miniz_reader_init_file takes UTF-8; convert from wide.
        std::string utf8 = WideToUtf8(path);
        if (!mz_zip_reader_init_file(&zip, utf8.c_str(), 0)) return false;
        open = true;
        return true;
    }

    // Extract a single named entry. Looks up case-insensitively because some
    // EPUBs (notably Sigil output) use mixed casing for META-INF.
    bool Extract(const std::string& name, std::vector<uint8_t>& out) {
        if (!open) return false;
        int idx = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
        if (idx < 0) {
            // Case-insensitive scan
            mz_uint n = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < n; ++i) {
                char buf[512];
                mz_uint len = mz_zip_reader_get_filename(&zip, i, buf, sizeof(buf));
                if (len == 0) continue;
                if (_stricmp(buf, name.c_str()) == 0) { idx = (int)i; break; }
            }
            if (idx < 0) return false;
        }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, idx, &st)) return false;
        if (st.m_uncomp_size > 64ULL * 1024 * 1024) return false;  // sanity cap
        out.resize((size_t)st.m_uncomp_size);
        if (st.m_uncomp_size == 0) return true;
        return mz_zip_reader_extract_to_mem(&zip, idx, out.data(),
                                            (size_t)st.m_uncomp_size, 0) != 0;
    }
};

// Strip BOM from a buffer in-place (front trim only).
void StripBomVec(std::vector<uint8_t>& buf) {
    if (buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        buf.erase(buf.begin(), buf.begin() + 3);
    }
}

// Load XML from a zip-extracted buffer.
ComPtr<IXMLDOMDocument2> LoadXmlFromZipEntry(std::vector<uint8_t>& buf) {
    StripBomVec(buf);
    if (buf.empty()) return {};
    std::string s((const char*)buf.data(), buf.size());

    // Same DOCTYPE strip as LoadXmlFromFile.
    size_t p = s.find("<!DOCTYPE");
    if (p == std::string::npos) p = s.find("<!doctype");
    if (p != std::string::npos) {
        size_t q = s.find('>', p);
        if (q != std::string::npos) s.erase(p, q - p + 1);
    }
    return LoadXmlFromString(s);
}

// Parse one XHTML entry from inside the EPUB and walk it for segments. We
// stage the bytes into a temp file so the existing XhtmlCache (which keys off
// file paths) doesn't have to grow a parallel in-memory variant. The temp file
// is deleted on scope exit.
struct TempXhtmlFile {
    std::wstring path;
    ~TempXhtmlFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};

bool WriteTempXhtml(const std::vector<uint8_t>& buf, std::wstring& outPath) {
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir);
    wchar_t tmp[MAX_PATH]; if (!GetTempFileNameW(dir, L"mae", 0, tmp)) return false;
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = buf.empty() ? TRUE
                          : WriteFile(h, buf.data(), (DWORD)buf.size(), &wrote, nullptr);
    CloseHandle(h);
    if (!ok || (!buf.empty() && wrote != buf.size())) return false;
    outPath = tmp;
    return true;
}

// =========================================================================
// Phase 3 — EPUB Media Overlays helpers
//
// Media Overlays are SMIL files that pair XHTML fragments with audio clip
// ranges. The audio (typically MP3) lives inside the .epub ZIP, so to feed
// it to BASS we extract each unique audio entry to a per-book temp folder
// the first time we encounter it. The temp folder is cleaned up by
// DaisyClose() (player) when the book is unloaded.
// =========================================================================

// Resolve a zip-relative href like "audio/track1.mp3" against a base entry
// path like "OEBPS/smil/page1.smil" — produces "OEBPS/audio/track1.mp3".
// Handles "./" and "../" segments. All paths use forward slashes.
std::string ResolveZipPath(const std::string& baseEntry, const std::string& relHref) {
    if (relHref.empty()) return baseEntry;
    // If relHref is absolute (rare in EPUB), use as-is sans leading slash.
    if (relHref[0] == '/') return relHref.substr(1);

    // Split baseEntry into segments (drop the filename component).
    std::vector<std::string> segs;
    {
        std::string b = baseEntry;
        size_t slash = b.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "" : b.substr(0, slash);
        size_t start = 0;
        for (size_t i = 0; i <= dir.size(); ++i) {
            if (i == dir.size() || dir[i] == '/') {
                if (i > start) segs.push_back(dir.substr(start, i - start));
                start = i + 1;
            }
        }
    }
    // Walk relHref segments, applying "./" and "../" semantics.
    size_t start = 0;
    for (size_t i = 0; i <= relHref.size(); ++i) {
        if (i == relHref.size() || relHref[i] == '/') {
            if (i > start) {
                std::string seg = relHref.substr(start, i - start);
                if (seg == ".") {
                    // skip
                } else if (seg == "..") {
                    if (!segs.empty()) segs.pop_back();
                } else {
                    segs.push_back(seg);
                }
            }
            start = i + 1;
        }
    }
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += '/';
        out += segs[i];
    }
    return out;
}

// Create a unique per-book temp folder under %TEMP%. Returns the path on
// success or empty on failure.
std::wstring CreateBookTempDir() {
    wchar_t tempRoot[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempRoot)) return L"";
    SYSTEMTIME st; GetSystemTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"MediaAccess_EpubMO_%lu_%lu_%lu",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount(),
             (unsigned long)st.wMilliseconds);
    std::wstring full = std::wstring(tempRoot) + name;
    if (!CreateDirectoryW(full.c_str(), nullptr)) {
        // If it already exists (collision is essentially impossible but be safe),
        // append another disambiguator.
        full += L"_x";
        if (!CreateDirectoryW(full.c_str(), nullptr)) return L"";
    }
    return full;
}

// Extract one zip entry to disk at tempDir/<baseName>. baseName must be a
// safe filename (no path separators). Returns the full disk path on success.
std::wstring ExtractZipEntryToDir(EpubZip& zip, const std::string& entryName,
                                  const std::wstring& tempDir,
                                  const std::wstring& baseName) {
    std::vector<uint8_t> data;
    if (!zip.Extract(entryName, data)) return L"";
    std::wstring out = tempDir + L"\\" + baseName;
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD wrote = 0;
    BOOL ok = data.empty() ? TRUE
                           : WriteFile(h, data.data(), (DWORD)data.size(),
                                       &wrote, nullptr);
    CloseHandle(h);
    if (!ok || (!data.empty() && wrote != data.size())) {
        DeleteFileW(out.c_str());
        return L"";
    }
    return out;
}

// Mint a unique on-disk filename for a zip entry. Strips path components,
// appends a numeric disambiguator if the basename has already been used.
std::wstring UniqueBaseNameFor(const std::string& entryName,
                               std::map<std::string, std::wstring>& assigned) {
    auto it = assigned.find(entryName);
    if (it != assigned.end()) return it->second;
    size_t slash = entryName.find_last_of('/');
    std::string base = (slash == std::string::npos)
                       ? entryName : entryName.substr(slash + 1);
    if (base.empty()) base = "audio.bin";
    // Strip any odd characters that Windows objects to.
    for (auto& c : base) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' ||
            c == '?' || c == '*' || c == '\\') c = '_';
    }
    std::wstring w = Utf8ToWide(base);
    // Ensure uniqueness across different zip entries with the same basename.
    int suffix = 0;
    std::wstring candidate = w;
    while (true) {
        bool used = false;
        for (auto& kv : assigned) if (kv.second == candidate) { used = true; break; }
        if (!used) break;
        wchar_t buf[16]; swprintf(buf, 16, L"_%d", ++suffix);
        // Insert before extension if any.
        size_t dot = w.find_last_of(L'.');
        candidate = (dot == std::wstring::npos)
                    ? (w + buf)
                    : (w.substr(0, dot) + buf + w.substr(dot));
    }
    assigned[entryName] = candidate;
    return candidate;
}

// Parse one in-zip SMIL file and append its clips to the book. Extracts
// referenced audio entries to tempDir on first use.
//
// smilEntry — the zip entry path of the SMIL file (used to resolve relative
//             audio src refs)
// epubPath  — original .epub disk path (used for the XHTML origin label)
// audioMap  — cache: zip-entry → on-disk extracted filename (per book)
bool ParseEpubSmilFromZip(EpubZip& zip, const std::string& smilEntry,
                          const std::wstring& tempDir,
                          const std::wstring& epubPath,
                          std::map<std::string, std::wstring>& audioMap,
                          std::vector<DaisyClip>& outClips) {
    std::vector<uint8_t> smilBuf;
    if (!zip.Extract(smilEntry, smilBuf)) return false;
    auto doc = LoadXmlFromZipEntry(smilBuf);
    if (!doc) return false;

    auto pars = SelectNodes(doc.Get(), L"//*[local-name()='par']");
    long count = 0; if (pars) pars->get_length(&count);

    for (long i = 0; i < count; ++i) {
        ComPtr<IXMLDOMNode> par;
        pars->get_item(i, par.PutVoid());
        if (!par) continue;

        // Text reference
        std::wstring textFile;
        std::string  textFrag;
        auto textList = SelectNodes(par.Get(), L".//*[local-name()='text']");
        long tCount = 0; if (textList) textList->get_length(&tCount);
        if (tCount > 0) {
            ComPtr<IXMLDOMNode> tn;
            textList->get_item(0, tn.PutVoid());
            if (tn) {
                std::wstring src = AttrValue(tn.Get(), L"src");
                if (!src.empty()) {
                    std::wstring file;
                    SplitFragment(UrlDecode(src), file, textFrag);
                    if (!file.empty()) {
                        std::string resolved = ResolveZipPath(
                            smilEntry, WideToUtf8(file));
                        textFile = epubPath + L"!/" + Utf8ToWide(resolved);
                    }
                }
            }
        }

        // Audio references
        auto audioList = SelectNodes(par.Get(), L".//*[local-name()='audio']");
        long aCount = 0; if (audioList) audioList->get_length(&aCount);
        for (long a = 0; a < aCount; ++a) {
            ComPtr<IXMLDOMNode> an;
            audioList->get_item(a, an.PutVoid());
            if (!an) continue;
            std::wstring src = AttrValue(an.Get(), L"src");
            if (src.empty()) continue;

            std::string audioEntry = ResolveZipPath(smilEntry,
                                                    WideToUtf8(UrlDecode(src)));

            // Extract on first sight, cache disk path.
            std::wstring diskPath;
            auto it = audioMap.find(audioEntry);
            if (it != audioMap.end()) {
                diskPath = tempDir + L"\\" + it->second;
            } else {
                std::wstring base = UniqueBaseNameFor(audioEntry, audioMap);
                diskPath = ExtractZipEntryToDir(zip, audioEntry, tempDir, base);
                if (diskPath.empty()) {
                    LogF("daisy",
                         "EPUB MO: could not extract audio %s",
                         audioEntry.c_str());
                    continue;
                }
            }

            std::wstring beginStr = AttrAny(an.Get(),
                {L"clipBegin", L"clip-begin", L"clipbegin"});
            std::wstring endStr   = AttrAny(an.Get(),
                {L"clipEnd",   L"clip-end",   L"clipend"});

            DaisyClip clip;
            clip.audioFile  = diskPath;
            clip.clipBegin  = beginStr.empty() ? 0.0 : ParseSmilTime(beginStr);
            clip.clipEnd    = endStr.empty()   ? 0.0 : ParseSmilTime(endStr);
            if (clip.clipBegin < 0) clip.clipBegin = 0;
            if (clip.clipEnd   < 0) clip.clipEnd   = 0;
            clip.textFile   = textFile;
            clip.textFragId = textFrag;
            clip.skipKind   = ClassifySmilParSkipKind(par.Get());  // Phase 4
            outClips.push_back(clip);
        }
    }
    return !outClips.empty();
}

// =========================================================================
// Phase 4 — EPUB Navigation Document / NCX parsing
//
// EPUB navigation lives in one of two places (in order of preference):
//   1. A Navigation Document — an XHTML file with the OPF manifest
//      `properties="nav"` attribute, containing <nav epub:type="toc">...</nav>
//      built from nested <ol>/<li>/<a> elements (EPUB 3 standard).
//   2. An NCX file (EPUB 2 legacy compatibility) referenced via the spine
//      `toc` attribute or as a manifest item with media-type
//      application/x-dtbncx+xml.
//
// For each TOC entry we resolve "chap1.xhtml#frag1" against either
// textSegments (text-only EPUB, segmentIndex stored in clipIndex) or
// clips (Media Overlays mode, true clipIndex). If neither source is
// available we fall back to synthesising nav points from the h1-h6
// headings already detected during the textSegments walk.
// =========================================================================

// Match originLabel format used by Phase 2 / Phase 3 segment+MO clip walks.
inline std::wstring EpubOriginLabel(const std::wstring& epubPath,
                                    const std::string& entryName) {
    return epubPath + L"!/" + Utf8ToWide(entryName);
}

// Find the first segment whose textFile == originLabel AND (if fragId
// non-empty) textFragId == fragId. Falls back to first-segment-of-file
// match when the fragment id isn't found. Returns -1 if no match at all.
int ResolveEpubSegment(const DaisyBook& book,
                       const std::wstring& originLabel,
                       const std::string& fragId) {
    int firstInFile = -1;
    for (size_t i = 0; i < book.textSegments.size(); ++i) {
        const auto& s = book.textSegments[i];
        if (s.textFile != originLabel) continue;
        if (firstInFile < 0) firstInFile = (int)i;
        if (fragId.empty()) return (int)i;
        if (s.textFragId == fragId) return (int)i;
    }
    return firstInFile;
}

// Same but for MO clips.
int ResolveEpubClip(const DaisyBook& book,
                    const std::wstring& originLabel,
                    const std::string& fragId) {
    int firstInFile = -1;
    for (size_t i = 0; i < book.clips.size(); ++i) {
        const auto& c = book.clips[i];
        if (c.textFile != originLabel) continue;
        if (firstInFile < 0) firstInFile = (int)i;
        if (fragId.empty()) return (int)i;
        if (c.textFragId == fragId) return (int)i;
    }
    return firstInFile;
}

// Walk an <ol> recursively. Each direct <li> contributes one nav point;
// each nested <ol> increments the heading depth. Cap depth at 6.
void WalkEpubNavOl(IXMLDOMNode* olNode, int depth,
                   const std::string& navEntry,
                   const std::wstring& epubPath,
                   const DaisyBook& book,
                   bool useClips,
                   std::vector<DaisyNavPoint>& out) {
    if (!olNode) return;
    int capDepth = (depth < 1) ? 1 : ((depth > 6) ? 6 : depth);

    auto lis = SelectNodes(olNode, L"./*[local-name()='li']");
    long n = 0; if (lis) lis->get_length(&n);
    for (long i = 0; i < n; ++i) {
        ComPtr<IXMLDOMNode> li;
        lis->get_item(i, li.PutVoid());
        if (!li) continue;

        // Direct <a>: target link. May be missing for group-only headings —
        // in which case we still recurse for the inner <ol>.
        auto a = SelectSingleNode(li.Get(), L"./*[local-name()='a']");
        if (a) {
            std::wstring label = NodeText(a.Get());
            std::wstring href  = AttrValue(a.Get(), L"href");
            if (!label.empty() && !href.empty()) {
                std::wstring file; std::string frag;
                SplitFragment(UrlDecode(href), file, frag);
                std::string resolved = ResolveZipPath(navEntry,
                                                      WideToUtf8(file));
                std::wstring originLabel = EpubOriginLabel(epubPath, resolved);
                int idx = useClips
                          ? ResolveEpubClip(book, originLabel, frag)
                          : ResolveEpubSegment(book, originLabel, frag);
                if (idx >= 0) {
                    DaisyNavPoint pt;
                    pt.level     = capDepth;
                    pt.label     = label;
                    pt.clipIndex = idx;
                    pt.kind      = DaisyNavPoint::Heading;
                    out.push_back(pt);
                }
            }
        }

        // Recurse: any nested <ol> inside this <li> goes one level deeper.
        auto nestedOls = SelectNodes(li.Get(), L"./*[local-name()='ol']");
        long m = 0; if (nestedOls) nestedOls->get_length(&m);
        for (long j = 0; j < m; ++j) {
            ComPtr<IXMLDOMNode> nol;
            nestedOls->get_item(j, nol.PutVoid());
            WalkEpubNavOl(nol.Get(), depth + 1, navEntry, epubPath,
                          book, useClips, out);
        }
    }
}

// Parse the EPUB 3 Navigation Document. Returns number of nav points added.
int ParseEpubNavDoc(EpubZip& zip,
                    const std::string& navEntry,
                    const std::wstring& epubPath,
                    DaisyBook& book,
                    bool useClips) {
    std::vector<uint8_t> buf;
    if (!zip.Extract(navEntry, buf)) return 0;
    auto doc = LoadXmlFromZipEntry(buf);
    if (!doc) return 0;

    size_t before = book.navPoints.size();

    // <nav epub:type="toc">. EPUB uses the epub: prefix but our XPath uses
    // local-name() — match both qualified and unqualified type attributes.
    auto navs = SelectNodes(doc.Get(), L"//*[local-name()='nav']");
    long nn = 0; if (navs) navs->get_length(&nn);

    auto walkNav = [&](IXMLDOMNode* navNode, bool isPageList) {
        auto ol = SelectSingleNode(navNode, L"./*[local-name()='ol']");
        if (!ol) return;
        size_t startSize = book.navPoints.size();
        WalkEpubNavOl(ol.Get(), 1, navEntry, epubPath, book, useClips,
                      book.navPoints);
        if (isPageList) {
            // Rewrite the just-emitted entries as Page kind, level 0.
            for (size_t k = startSize; k < book.navPoints.size(); ++k) {
                book.navPoints[k].kind  = DaisyNavPoint::Page;
                book.navPoints[k].level = 0;
            }
        }
    };

    for (long i = 0; i < nn; ++i) {
        ComPtr<IXMLDOMNode> nav;
        navs->get_item(i, nav.PutVoid());
        if (!nav) continue;
        std::wstring tp = AttrAny(nav.Get(), {L"epub:type", L"type"});
        std::wstring tpLower = ToLowerW(tp);
        // We handle 'toc' (regular nav) and 'page-list' (page numbers).
        // Skip 'landmarks' and any other type.
        if (tpLower == L"toc" || tpLower.empty()) {
            walkNav(nav.Get(), /*isPageList*/false);
        } else if (tpLower == L"page-list") {
            walkNav(nav.Get(), /*isPageList*/true);
        }
    }

    return (int)(book.navPoints.size() - before);
}

// Fallback: synthesise nav points from already-walked textSegments. h1-h6
// headings become DaisyNavPoint::Heading; navLevel==0 spans become Page.
int BuildEpubHeadingNavPoints(DaisyBook& book) {
    size_t before = book.navPoints.size();
    for (size_t i = 0; i < book.textSegments.size(); ++i) {
        const auto& seg = book.textSegments[i];
        if (seg.navLevel == 0) {
            DaisyNavPoint pt;
            pt.level     = 0;
            pt.label     = seg.text;
            pt.clipIndex = (int)i;
            pt.kind      = DaisyNavPoint::Page;
            book.navPoints.push_back(pt);
        } else if (seg.navLevel >= 1 && seg.navLevel <= 6) {
            DaisyNavPoint pt;
            pt.level     = seg.navLevel;
            pt.label     = seg.text;
            pt.clipIndex = (int)i;
            pt.kind      = DaisyNavPoint::Heading;
            book.navPoints.push_back(pt);
        }
    }
    return (int)(book.navPoints.size() - before);
}

bool ParseEpub3Metadata(const std::wstring& epubPath, DaisyBook& book) {
    book.format = DaisyFormat::Epub3;

    // Use filename (sans ext) as title fallback
    std::wstring fname = GetFileName(epubPath);
    size_t dot = fname.find_last_of(L'.');
    if (dot != std::wstring::npos) fname = fname.substr(0, dot);
    book.title = fname;

    EpubZip zip;
    if (!zip.Open(epubPath)) {
        LogF("daisy", "EPUB: could not open zip %s", WideToUtf8(epubPath).c_str());
        return true;
    }

    std::vector<uint8_t> container;
    if (!zip.Extract("META-INF/container.xml", container)) return true;
    auto cdoc = LoadXmlFromZipEntry(container);
    if (!cdoc) return true;

    auto rf = SelectSingleNode(cdoc.Get(), L"//*[local-name()='rootfile']");
    if (!rf) return true;
    std::wstring opfRel = AttrValue(rf.Get(), L"full-path");
    if (opfRel.empty()) return true;

    std::vector<uint8_t> opfBuf;
    if (!zip.Extract(WideToUtf8(opfRel), opfBuf)) return true;

    auto odoc = LoadXmlFromZipEntry(opfBuf);
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

    // Phase 2: read the spine and build textSegments from every XHTML entry.
    // The OPF base directory inside the zip is the parent of the .opf path.
    std::string opfBaseUtf8;
    {
        std::string r = WideToUtf8(opfRel);
        size_t slash = r.find_last_of('/');
        opfBaseUtf8 = (slash == std::string::npos) ? "" : r.substr(0, slash + 1);
    }

    // Manifest: id -> (href, mediaType, mediaOverlay, properties).
    // mediaOverlay is the id of another manifest item (a SMIL file) — present
    // on EPUBs with audio sync (Phase 3 Media Overlays support).
    // properties carries the "nav" token when the item is the EPUB 3
    // Navigation Document (Phase 4 chapter navigation).
    struct ManifestItem {
        std::string href, mediaType, mediaOverlay, properties;
    };
    std::map<std::string, ManifestItem> manifest;
    auto items = SelectNodes(odoc.Get(), L"//*[local-name()='item']");
    long iCount = 0; if (items) items->get_length(&iCount);
    for (long i = 0; i < iCount; ++i) {
        ComPtr<IXMLDOMNode> it;
        items->get_item(i, it.PutVoid());
        if (!it) continue;
        std::string id    = WideToUtf8(AttrValue(it.Get(), L"id"));
        std::string href  = WideToUtf8(UrlDecode(AttrValue(it.Get(), L"href")));
        std::string mt    = WideToUtf8(AttrValue(it.Get(), L"media-type"));
        std::string mo    = WideToUtf8(AttrValue(it.Get(), L"media-overlay"));
        std::string props = WideToUtf8(AttrValue(it.Get(), L"properties"));
        if (!id.empty() && !href.empty())
            manifest[id] = { href, mt, mo, props };
    }

    // Spine
    std::vector<std::string> spineIds;
    auto refs = SelectNodes(odoc.Get(),
        L"//*[local-name()='spine']/*[local-name()='itemref']");
    long sCount = 0; if (refs) refs->get_length(&sCount);
    for (long i = 0; i < sCount; ++i) {
        ComPtr<IXMLDOMNode> it;
        refs->get_item(i, it.PutVoid());
        if (!it) continue;
        std::string id = WideToUtf8(AttrValue(it.Get(), L"idref"));
        if (!id.empty()) spineIds.push_back(id);
    }

    // Walk each XHTML spine entry, extract its text into book.textSegments.
    XhtmlCache xcache;
    std::vector<TempXhtmlFile> tmpFiles;  // keep alive until function returns
    for (auto& id : spineIds) {
        auto mit = manifest.find(id);
        if (mit == manifest.end()) continue;
        std::string mtLow = ToLowerA(mit->second.mediaType);
        bool isXhtml = mtLow.find("html") != std::string::npos;
        if (!isXhtml) {
            // Fall back on extension
            std::string href = ToLowerA(mit->second.href);
            if (href.size() < 4) continue;
            if (href.rfind(".xhtml") == std::string::npos &&
                href.rfind(".html")  == std::string::npos &&
                href.rfind(".htm")   == std::string::npos) continue;
        }
        std::string entryName = opfBaseUtf8 + mit->second.href;
        std::vector<uint8_t> body;
        if (!zip.Extract(entryName, body)) continue;

        TempXhtmlFile tmp;
        if (!WriteTempXhtml(body, tmp.path)) continue;

        // Remember where segments came from in terms users / search will
        // recognise: original epub path + entry. Rewrite textFile after the walk.
        size_t before = book.textSegments.size();
        AppendSegmentsFromFile(tmp.path, xcache, book.textSegments);
        std::wstring originLabel = epubPath + L"!/" + Utf8ToWide(entryName);
        for (size_t k = before; k < book.textSegments.size(); ++k) {
            book.textSegments[k].textFile = originLabel;
        }
        tmpFiles.push_back(std::move(tmp));
    }
    // tmpFiles destructor cleans up the on-disk temp files now that the XHTML
    // DOMs are parsed and the text content has been copied into segments.

    // ----------------------------------------------------------------------
    // Phase 3 — Media Overlays. If any spine entry has a media-overlay attr,
    // build clips from the referenced SMIL files in spine order. When this
    // produces at least one clip we drop the text-only TTS fallback (clips
    // take priority in DaisyLoadAndPlay).
    // ----------------------------------------------------------------------
    bool anyOverlay = false;
    for (auto& id : spineIds) {
        auto mit = manifest.find(id);
        if (mit == manifest.end()) continue;
        if (!mit->second.mediaOverlay.empty()) { anyOverlay = true; break; }
    }

    if (anyOverlay) {
        std::wstring tempDir = CreateBookTempDir();
        if (tempDir.empty()) {
            LogF("daisy", "EPUB MO: could not create temp dir, skipping overlays");
        } else {
            std::map<std::string, std::wstring> audioMap;
            std::vector<DaisyClip>              moClips;
            for (auto& id : spineIds) {
                auto mit = manifest.find(id);
                if (mit == manifest.end()) continue;
                if (mit->second.mediaOverlay.empty()) continue;
                auto smilManifest = manifest.find(mit->second.mediaOverlay);
                if (smilManifest == manifest.end()) continue;
                std::string smilEntry = opfBaseUtf8 + smilManifest->second.href;
                ParseEpubSmilFromZip(zip, smilEntry, tempDir, epubPath,
                                     audioMap, moClips);
            }

            if (!moClips.empty()) {
                book.tempAudioDir = tempDir;
                book.clips        = std::move(moClips);
                // Recompute totalDuration from clip ranges.
                double total = 0.0;
                for (const auto& c : book.clips) {
                    if (c.clipEnd > c.clipBegin) total += c.clipEnd - c.clipBegin;
                }
                if (total > 0) book.totalDuration = total;
                LogF("daisy", "EPUB MO: built %zu clips into %s",
                     book.clips.size(), WideToUtf8(tempDir).c_str());
            } else {
                // No clips produced — clean up the empty temp dir.
                RemoveDirectoryW(tempDir.c_str());
            }
        }
    }

    // ----------------------------------------------------------------------
    // Phase 4 — Chapter navigation. Build navPoints from:
    //   1. EPUB 3 Navigation Document if present (manifest item with
    //      properties="nav") — preferred.
    //   2. Otherwise synthesize from h1-h6 / page spans detected in the
    //      textSegments walk (the navLevel field is already populated).
    // Both modes (text-only TTS and Media Overlays) use the same navPoints
    // vector; clipIndex stores either a segment index (text-only) or a
    // clip index (MO) — see DaisyNavPoint comment in daisy_book.h.
    // ----------------------------------------------------------------------
    bool useClips = !book.clips.empty();
    int  navAdded = 0;

    // Look for the EPUB 3 Navigation Document.
    for (const auto& kv : manifest) {
        if (kv.second.properties.find("nav") == std::string::npos) continue;
        std::string navEntry = opfBaseUtf8 + kv.second.href;
        navAdded = ParseEpubNavDoc(zip, navEntry, epubPath, book, useClips);
        break;
    }

    // Fallback: synthesise from h1-h6 + page spans already captured during
    // the textSegments walk. Only useful in text-only mode (MO mode resolves
    // by clips, which the segment walker doesn't index).
    if (navAdded == 0 && !useClips) {
        navAdded = BuildEpubHeadingNavPoints(book);
    }

    if (navAdded > 0) {
        // Existing navPoint scans in daisy_player.cpp assume monotonic
        // clipIndex ordering. Sort to guarantee that.
        std::sort(book.navPoints.begin(), book.navPoints.end(),
                  [](const DaisyNavPoint& a, const DaisyNavPoint& b) {
                      return a.clipIndex < b.clipIndex;
                  });
        LogF("daisy", "EPUB nav: built %d nav points (%s)",
             navAdded, useClips ? "MO clips" : "text segments");
    }

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

    LogF("daisy", "opened %ls: %d clips, %d nav points, %d segments, %.1fs total",
         book->title.c_str(),
         (int)book->clips.size(),
         (int)book->navPoints.size(),
         (int)book->textSegments.size(),
         book->totalDuration);
    return book;
}

// -----------------------------------------------------------------------------
// DaisySearchBook — case-insensitive substring search over all text content.
//
// Uses CompareStringW with NORM_IGNORECASE so accented characters fold
// correctly (é matches É, ñ matches Ñ). For each hit we extract a short
// single-line snippet centred on the match so the UI list can preview it.
// -----------------------------------------------------------------------------

namespace {

// Case-insensitive substring search using CompareStringW. Returns the start
// index of the first match, or std::wstring::npos.
size_t FindCI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty() || hay.size() < needle.size()) return std::wstring::npos;
    LCID lc = LOCALE_USER_DEFAULT;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        int r = CompareStringW(lc, NORM_IGNORECASE,
                               hay.c_str() + i, (int)needle.size(),
                               needle.c_str(), (int)needle.size());
        if (r == CSTR_EQUAL) return i;
    }
    return std::wstring::npos;
}

std::wstring MakeSnippet(const std::wstring& src, size_t matchPos, size_t matchLen) {
    constexpr size_t kPad = 30;
    size_t start = (matchPos > kPad) ? matchPos - kPad : 0;
    size_t end   = matchPos + matchLen + kPad;
    if (end > src.size()) end = src.size();
    std::wstring s = src.substr(start, end - start);
    // Collapse newlines/tabs to spaces for one-line display.
    for (auto& c : s) {
        if (c == L'\n' || c == L'\r' || c == L'\t') c = L' ';
    }
    if (start > 0)        s = L"..." + s;
    if (end   < src.size()) s += L"...";
    return s;
}

} // namespace

std::vector<DaisySearchHit> DaisySearchBook(const DaisyBook& book,
                                            const std::wstring& needle) {
    std::vector<DaisySearchHit> out;
    if (needle.empty()) return out;

    for (int i = 0; i < (int)book.clips.size(); ++i) {
        const auto& c = book.clips[i];
        if (c.textContent.empty()) continue;
        size_t pos = FindCI(c.textContent, needle);
        if (pos == std::wstring::npos) continue;
        DaisySearchHit h;
        h.clipIndex    = i;
        h.segmentIndex = -1;
        h.snippet      = MakeSnippet(c.textContent, pos, needle.size());
        out.push_back(std::move(h));
    }

    for (int i = 0; i < (int)book.textSegments.size(); ++i) {
        const auto& s = book.textSegments[i];
        if (s.text.empty()) continue;
        size_t pos = FindCI(s.text, needle);
        if (pos == std::wstring::npos) continue;
        DaisySearchHit h;
        h.clipIndex    = -1;
        h.segmentIndex = i;
        h.snippet      = MakeSnippet(s.text, pos, needle.size());
        out.push_back(std::move(h));
    }

    return out;
}

} // namespace mediaaccess
