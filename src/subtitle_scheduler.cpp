// =============================================================================
// subtitle_scheduler.cpp — subtitle parsing (and, later, prefetch scheduling).
// See subtitle_scheduler.h for the design.
// =============================================================================

#include "mediaaccess/subtitle_scheduler.h"
#include "mediaaccess/utils.h"   // Utf8ToWide

#include <algorithm>
#include <cctype>

namespace mediaaccess {

namespace {

// Split into lines on '\n', dropping a trailing '\r' (CRLF) and any UTF-8 BOM.
std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    // Skip UTF-8 BOM if present.
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF &&
        (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        start = 3;
    std::string cur;
    for (size_t i = start; i < s.size(); i++) {
        char c = s[i];
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += c;
    }
    out.push_back(cur);
    return out;
}

bool IsBlank(const std::string& s) {
    for (char c : s) if (!isspace((unsigned char)c)) return false;
    return true;
}

// Parse "HH:MM:SS,mmm" / "MM:SS.mmm" (hours optional, ',' or '.' for ms) to sec.
// Returns -1 on failure.
double ParseTimecode(const std::string& tc) {
    int parts[3] = {0, 0, 0};   // we fill from the right: sec, min, hour
    int ms = 0;
    std::string t;
    for (char c : tc) if (!isspace((unsigned char)c)) t += c;  // trim spaces
    if (t.empty()) return -1;

    // Split off milliseconds at ',' or '.'.
    size_t dot = t.find_first_of(",.");
    std::string hms = (dot == std::string::npos) ? t : t.substr(0, dot);
    if (dot != std::string::npos) {
        std::string msStr = t.substr(dot + 1);
        if (msStr.empty()) return -1;
        // Pad/truncate to 3 digits.
        msStr = (msStr + "000").substr(0, 3);
        for (char c : msStr) { if (!isdigit((unsigned char)c)) return -1; ms = ms * 10 + (c - '0'); }
    }

    // Split hms on ':' from the right into up to 3 fields.
    int nfields = 0;
    size_t end = hms.size();
    while (end != std::string::npos && nfields < 3) {
        size_t colon = hms.rfind(':', end == 0 ? std::string::npos : end - 1);
        std::string field = (colon == std::string::npos) ? hms.substr(0, end)
                                                          : hms.substr(colon + 1, end - colon - 1);
        if (field.empty()) return -1;
        int val = 0;
        for (char c : field) { if (!isdigit((unsigned char)c)) return -1; val = val * 10 + (c - '0'); }
        parts[nfields++] = val;
        if (colon == std::string::npos) break;
        end = colon;
    }
    // parts[0]=sec, [1]=min, [2]=hour
    return parts[2] * 3600.0 + parts[1] * 60.0 + parts[0] + ms / 1000.0;
}

// Remove markup: <...> (HTML/VTT tags) and {...} (SSA/ASS overrides).
std::string StripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int depthAngle = 0, depthBrace = 0;
    for (char c : s) {
        if (c == '<') { depthAngle++; continue; }
        if (c == '>') { if (depthAngle > 0) depthAngle--; continue; }
        if (c == '{') { depthBrace++; continue; }
        if (c == '}') { if (depthBrace > 0) depthBrace--; continue; }
        if (depthAngle == 0 && depthBrace == 0) out += c;
    }
    return out;
}

} // namespace

std::vector<SubCue> ParseSubtitles(const std::string& utf8Data) {
    std::vector<SubCue> cues;
    std::vector<std::string> lines = SplitLines(utf8Data);

    for (size_t i = 0; i < lines.size(); i++) {
        size_t arrow = lines[i].find("-->");
        if (arrow == std::string::npos) continue;

        double start = ParseTimecode(lines[i].substr(0, arrow));
        // End time is the token right after "-->"; ignore any VTT cue settings.
        std::string rest = lines[i].substr(arrow + 3);
        size_t e = 0; while (e < rest.size() && isspace((unsigned char)rest[e])) e++;
        size_t e2 = e; while (e2 < rest.size() && !isspace((unsigned char)rest[e2])) e2++;
        double end = ParseTimecode(rest.substr(e, e2 - e));
        if (start < 0 || end < 0) continue;

        // Gather text lines until a blank line (cue separator).
        std::wstring text;
        size_t j = i + 1;
        for (; j < lines.size() && !IsBlank(lines[j]); j++) {
            std::string clean = StripTags(lines[j]);
            if (clean.empty()) continue;
            if (!text.empty()) text += L' ';
            text += Utf8ToWide(clean);
        }
        i = j;  // continue scanning after this cue's text block

        if (end < start) std::swap(start, end);
        if (!text.empty()) cues.push_back({start, end, std::move(text)});
    }

    std::sort(cues.begin(), cues.end(),
              [](const SubCue& a, const SubCue& b) { return a.startSec < b.startSec; });
    return cues;
}

} // namespace mediaaccess
