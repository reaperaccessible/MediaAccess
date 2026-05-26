#include "ui_internal.h"

// ========== Podcast Dialog ==========

// Podcast search result (from iTunes API)
struct PodcastSearchResult {
    std::wstring name;
    std::wstring feedUrl;
    std::wstring imageUrl;
    std::wstring artistName;
};

// Podcast dialog state
static std::vector<PodcastSubscription> g_podcastSubs;
static std::vector<PodcastEpisode> g_podcastEpisodes;
static std::vector<PodcastSearchResult> g_podcastSearchResults;
static int g_currentPodcastId = -1;

// Diagnostic info captured during feed fetch/parse, shown to the user on failure
struct PodcastFetchDiag {
    DWORD statusCode = 0;        // HTTP status (0 if request never completed)
    DWORD lastError = 0;         // Win32 error code if the request failed
    std::wstring errorText;      // Human-readable network error
    size_t bytesReceived = 0;
    int itemTagsFound = 0;       // Raw <item tags in the XML
    int episodesExtracted = 0;   // Items with both audioUrl and title
    std::wstring bodyPreview;    // First ~400 chars of the response
};

// Format a WinInet / Win32 error code into a human-readable message
static std::wstring FormatWinInetError(DWORD code) {
    if (code == 0) return L"";
    wchar_t* buf = nullptr;
    HMODULE hWinInet = GetModuleHandleW(L"wininet.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                  FORMAT_MESSAGE_FROM_SYSTEM;
    if (hWinInet) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    FormatMessageW(flags, hWinInet, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring result = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                               result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

// HTTP GET for podcast operations
static std::wstring PodcastHttpGet(const std::wstring& url, PodcastFetchDiag* diag = nullptr) {
    std::wstring result;

    // Parse URL to get host and path
    std::wstring host, path;
    bool secure = false;

    if (url.find(L"https://") == 0) {
        secure = true;
        size_t hostStart = 8;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else if (url.find(L"http://") == 0) {
        size_t hostStart = 7;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else {
        if (diag) {
            diag->errorText = L"Unsupported URL scheme (expected http:// or https://)";
        }
        return result;
    }

    HINTERNET hInternet = InternetOpenW(L"MediaAccess/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) {
        if (diag) {
            diag->lastError = GetLastError();
            diag->errorText = FormatWinInetError(diag->lastError);
        }
        return result;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (secure) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hConnect = InternetConnectW(hInternet, host.c_str(),
                                          secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
                                          nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (hConnect) {
        HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (hRequest) {
            if (HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0)) {
                if (diag) {
                    DWORD status = 0, sz = sizeof(status);
                    HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                                   &status, &sz, nullptr);
                    diag->statusCode = status;
                }
                char buffer[4096];
                DWORD bytesRead;
                std::string response;
                while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    response.append(buffer, bytesRead);
                }
                if (diag) {
                    diag->bytesReceived = response.size();
                    diag->bodyPreview = Utf8ToWide(response.substr(0, 400));
                }
                result = Utf8ToWide(response);
            } else if (diag) {
                diag->lastError = GetLastError();
                diag->errorText = FormatWinInetError(diag->lastError);
            }
            InternetCloseHandle(hRequest);
        } else if (diag) {
            diag->lastError = GetLastError();
            diag->errorText = FormatWinInetError(diag->lastError);
        }
        InternetCloseHandle(hConnect);
    } else if (diag) {
        diag->lastError = GetLastError();
        diag->errorText = FormatWinInetError(diag->lastError);
    }
    InternetCloseHandle(hInternet);

    return result;
}

// HTTP GET with Basic Authentication support
static std::wstring PodcastHttpGetAuth(const std::wstring& url, const std::wstring& username, const std::wstring& password, PodcastFetchDiag* diag = nullptr) {
    // If no credentials, use regular function
    if (username.empty() && password.empty()) {
        return PodcastHttpGet(url, diag);
    }

    std::wstring result;

    // Parse URL to get host and path
    std::wstring host, path;
    bool secure = false;

    if (url.find(L"https://") == 0) {
        secure = true;
        size_t hostStart = 8;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else if (url.find(L"http://") == 0) {
        size_t hostStart = 7;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) {
            host = url.substr(hostStart);
            path = L"/";
        } else {
            host = url.substr(hostStart, pathStart - hostStart);
            path = url.substr(pathStart);
        }
    } else {
        if (diag) {
            diag->errorText = L"Unsupported URL scheme (expected http:// or https://)";
        }
        return result;
    }

    HINTERNET hInternet = InternetOpenW(L"MediaAccess/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) {
        if (diag) {
            diag->lastError = GetLastError();
            diag->errorText = FormatWinInetError(diag->lastError);
        }
        return result;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (secure) flags |= INTERNET_FLAG_SECURE;

    HINTERNET hConnect = InternetConnectW(hInternet, host.c_str(),
                                          secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
                                          username.c_str(), password.c_str(), INTERNET_SERVICE_HTTP, 0, 0);
    if (hConnect) {
        HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (hRequest) {
            if (HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0)) {
                if (diag) {
                    DWORD status = 0, sz = sizeof(status);
                    HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                                   &status, &sz, nullptr);
                    diag->statusCode = status;
                }
                char buffer[4096];
                DWORD bytesRead;
                std::string response;
                while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    response.append(buffer, bytesRead);
                }
                if (diag) {
                    diag->bytesReceived = response.size();
                    diag->bodyPreview = Utf8ToWide(response.substr(0, 400));
                }
                result = Utf8ToWide(response);
            } else if (diag) {
                diag->lastError = GetLastError();
                diag->errorText = FormatWinInetError(diag->lastError);
            }
            InternetCloseHandle(hRequest);
        } else if (diag) {
            diag->lastError = GetLastError();
            diag->errorText = FormatWinInetError(diag->lastError);
        }
        InternetCloseHandle(hConnect);
    } else if (diag) {
        diag->lastError = GetLastError();
        diag->errorText = FormatWinInetError(diag->lastError);
    }
    InternetCloseHandle(hInternet);

    return result;
}

// URL encode for podcast searches
static std::wstring PodcastUrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else if (c == ' ') {
            encoded << L'+';
        } else {
            encoded << L'%' << std::hex << std::uppercase << std::setw(2) << std::setfill(L'0') << static_cast<int>(c);
        }
    }
    return encoded.str();
}

// Sanitize filename for saving
static std::wstring SanitizeFilename(const std::wstring& name) {
    std::wstring result;
    for (wchar_t c : name) {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            result += L'_';
        } else {
            result += c;
        }
    }
    // Trim trailing spaces and dots
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

// Get file extension from URL
static std::wstring GetUrlExtension(const std::wstring& url) {
    size_t queryPos = url.find(L'?');
    std::wstring path = (queryPos != std::wstring::npos) ? url.substr(0, queryPos) : url;
    size_t dotPos = path.rfind(L'.');
    if (dotPos != std::wstring::npos && dotPos > path.rfind(L'/')) {
        return path.substr(dotPos);
    }
    return L".mp3";  // Default to mp3
}

// Extract text content from an XML element
static std::wstring ExtractXmlContent(const std::wstring& xml, const std::wstring& tagName) {
    std::wstring startTag = L"<" + tagName;
    std::wstring endTag = L"</" + tagName + L">";

    size_t start = xml.find(startTag);
    if (start == std::wstring::npos) return L"";

    // Find the end of the opening tag
    size_t tagEnd = xml.find(L'>', start);
    if (tagEnd == std::wstring::npos) return L"";

    // Check for CDATA
    size_t contentStart = tagEnd + 1;
    size_t end = xml.find(endTag, contentStart);
    if (end == std::wstring::npos) return L"";

    std::wstring content = xml.substr(contentStart, end - contentStart);

    // Strip CDATA wrapper if present (may have leading whitespace)
    size_t cdataStart = content.find(L"<![CDATA[");
    if (cdataStart != std::wstring::npos) {
        size_t cdataEnd = content.rfind(L"]]>");
        if (cdataEnd != std::wstring::npos && cdataEnd > cdataStart) {
            content = content.substr(cdataStart + 9, cdataEnd - cdataStart - 9);
        }
    }

    // Basic HTML entity decode - named entities
    size_t pos = 0;
    while ((pos = content.find(L"&amp;", pos)) != std::wstring::npos) {
        content.replace(pos, 5, L"&");
    }
    pos = 0;
    while ((pos = content.find(L"&lt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L"<");
    }
    pos = 0;
    while ((pos = content.find(L"&gt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L">");
    }
    pos = 0;
    while ((pos = content.find(L"&quot;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"\"");
    }
    pos = 0;
    while ((pos = content.find(L"&apos;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"'");
    }
    pos = 0;
    while ((pos = content.find(L"&nbsp;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L" ");
    }

    // Numeric HTML entities (&#039; &#39; &#34; etc.)
    pos = 0;
    while ((pos = content.find(L"&#", pos)) != std::wstring::npos) {
        size_t semicolon = content.find(L';', pos);
        if (semicolon != std::wstring::npos && semicolon - pos < 8) {
            std::wstring numStr = content.substr(pos + 2, semicolon - pos - 2);
            int codePoint = 0;
            if (!numStr.empty() && (numStr[0] == L'x' || numStr[0] == L'X')) {
                // Hex: &#x27;
                codePoint = wcstol(numStr.c_str() + 1, nullptr, 16);
            } else {
                // Decimal: &#39;
                codePoint = wcstol(numStr.c_str(), nullptr, 10);
            }
            if (codePoint > 0 && codePoint < 0x10000) {
                wchar_t ch = static_cast<wchar_t>(codePoint);
                content.replace(pos, semicolon - pos + 1, 1, ch);
            } else {
                pos++;  // Skip invalid entity
            }
        } else {
            pos++;  // Skip malformed entity
        }
    }

    return content;
}

// Extract enclosure URL from an item
static std::wstring ExtractEnclosureUrl(const std::wstring& item) {
    size_t encPos = item.find(L"<enclosure");
    if (encPos == std::wstring::npos) return L"";

    size_t urlStart = item.find(L"url=\"", encPos);
    if (urlStart == std::wstring::npos) {
        urlStart = item.find(L"url='", encPos);
        if (urlStart == std::wstring::npos) return L"";
        urlStart += 5;
        size_t urlEnd = item.find(L"'", urlStart);
        if (urlEnd == std::wstring::npos) return L"";
        return item.substr(urlStart, urlEnd - urlStart);
    }
    urlStart += 5;
    size_t urlEnd = item.find(L"\"", urlStart);
    if (urlEnd == std::wstring::npos) return L"";
    return item.substr(urlStart, urlEnd - urlStart);
}

// Parse iTunes duration (HH:MM:SS or MM:SS or seconds)
static int ParseDuration(const std::wstring& duration) {
    if (duration.empty()) return 0;

    // Check if it's just seconds
    bool hasColon = duration.find(L':') != std::wstring::npos;
    if (!hasColon) {
        return _wtoi(duration.c_str());
    }

    // Parse HH:MM:SS or MM:SS
    int h = 0, m = 0, s = 0;
    if (swscanf(duration.c_str(), L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600 + m * 60 + s;
    } else if (swscanf(duration.c_str(), L"%d:%d", &m, &s) == 2) {
        return m * 60 + s;
    }
    return 0;
}

// Extract attribute value from an XML element string
static std::wstring ExtractXmlAttribute(const std::wstring& element, const std::wstring& attrName) {
    std::wstring search = attrName + L"=\"";
    size_t start = element.find(search);
    if (start == std::wstring::npos) {
        // Try single quotes
        search = attrName + L"='";
        start = element.find(search);
        if (start == std::wstring::npos) return L"";
    }
    start += search.length();
    char quoteChar = (search.back() == L'"') ? L'"' : L'\'';
    size_t end = element.find(quoteChar, start);
    if (end == std::wstring::npos) return L"";
    return element.substr(start, end - start);
}

// Structure for OPML feed entry
struct OpmlFeed {
    std::wstring title;
    std::wstring feedUrl;
};

// Parse OPML file and extract feed URLs
static std::vector<OpmlFeed> ParseOpmlFile(const std::wstring& filePath) {
    std::vector<OpmlFeed> feeds;

    // Read file content
    FILE* f = _wfopen(filePath.c_str(), L"rb");
    if (!f) return feeds;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 10 * 1024 * 1024) {  // Max 10MB
        fclose(f);
        return feeds;
    }

    std::vector<char> buffer(fileSize + 1);
    size_t bytesRead = fread(buffer.data(), 1, fileSize, f);
    fclose(f);
    buffer[bytesRead] = '\0';

    // Convert to wide string (assuming UTF-8)
    std::wstring xml = Utf8ToWide(std::string(buffer.data(), bytesRead));

    // Find all <outline elements with xmlUrl attribute
    size_t pos = 0;
    while ((pos = xml.find(L"<outline", pos)) != std::wstring::npos) {
        size_t elementEnd = xml.find(L'>', pos);
        if (elementEnd == std::wstring::npos) break;

        std::wstring element = xml.substr(pos, elementEnd - pos + 1);

        // Extract xmlUrl attribute (this is the feed URL)
        std::wstring feedUrl = ExtractXmlAttribute(element, L"xmlUrl");
        if (feedUrl.empty()) {
            // Try alternate attribute names
            feedUrl = ExtractXmlAttribute(element, L"xmlurl");
        }

        if (!feedUrl.empty()) {
            OpmlFeed feed;
            feed.feedUrl = feedUrl;
            // Try to get title from text attribute
            feed.title = ExtractXmlAttribute(element, L"text");
            if (feed.title.empty()) {
                feed.title = ExtractXmlAttribute(element, L"title");
            }
            feeds.push_back(feed);
        }

        pos = elementEnd;
    }

    return feeds;
}

// Parse RSS feed and extract episodes (with optional authentication)
static bool ParsePodcastFeed(const std::wstring& feedUrl, std::wstring& outTitle,
                             std::vector<PodcastEpisode>& episodes,
                             const std::wstring& username = L"", const std::wstring& password = L"",
                             PodcastFetchDiag* diag = nullptr) {
    episodes.clear();

    std::wstring xml = PodcastHttpGetAuth(feedUrl, username, password, diag);
    if (xml.empty()) return false;

    // Extract channel title
    size_t channelStart = xml.find(L"<channel");
    if (channelStart != std::wstring::npos) {
        size_t channelEnd = xml.find(L"</channel>", channelStart);
        if (channelEnd != std::wstring::npos) {
            std::wstring channel = xml.substr(channelStart, channelEnd - channelStart);
            // Get title before first <item>
            size_t firstItem = channel.find(L"<item");
            if (firstItem != std::wstring::npos) {
                std::wstring header = channel.substr(0, firstItem);
                outTitle = ExtractXmlContent(header, L"title");
            }
        }
    }

    // Find all <item> elements
    int itemCount = 0;
    size_t pos = 0;
    while ((pos = xml.find(L"<item", pos)) != std::wstring::npos) {
        itemCount++;
        size_t itemEnd = xml.find(L"</item>", pos);
        if (itemEnd == std::wstring::npos) break;

        std::wstring item = xml.substr(pos, itemEnd - pos + 7);
        PodcastEpisode ep;

        ep.title = ExtractXmlContent(item, L"title");
        ep.description = ExtractXmlContent(item, L"description");
        ep.pubDate = ExtractXmlContent(item, L"pubDate");
        ep.guid = ExtractXmlContent(item, L"guid");
        ep.audioUrl = ExtractEnclosureUrl(item);

        // Try to get duration from itunes:duration
        std::wstring durationStr = ExtractXmlContent(item, L"itunes:duration");
        ep.durationSeconds = ParseDuration(durationStr);

        if (!ep.audioUrl.empty() && !ep.title.empty()) {
            episodes.push_back(ep);
        }

        pos = itemEnd;
    }

    if (diag) {
        diag->itemTagsFound = itemCount;
        diag->episodesExtracted = static_cast<int>(episodes.size());
    }

    return !episodes.empty();
}

// Search iTunes podcast directory
static bool SearchItunesPodcasts(const std::wstring& query, std::vector<PodcastSearchResult>& results) {
    results.clear();

    std::wstring url = L"https://itunes.apple.com/search?term=" +
                       PodcastUrlEncode(query) + L"&media=podcast&limit=25";

    std::wstring json = PodcastHttpGet(url);
    if (json.empty()) return false;

    // Parse JSON results - look for each result object
    size_t pos = 0;
    while ((pos = json.find(L"\"collectionName\"", pos)) != std::wstring::npos) {
        PodcastSearchResult r;

        // Extract collectionName
        size_t valueStart = json.find(L":", pos);
        if (valueStart != std::wstring::npos) {
            size_t strStart = json.find(L"\"", valueStart + 1);
            if (strStart != std::wstring::npos) {
                strStart++;
                size_t strEnd = json.find(L"\"", strStart);
                if (strEnd != std::wstring::npos) {
                    r.name = json.substr(strStart, strEnd - strStart);
                }
            }
        }

        // Find feedUrl in the same object (search backwards and forwards)
        size_t searchStart = (pos > 500) ? pos - 500 : 0;
        size_t searchEnd = pos + 1000;
        if (searchEnd > json.length()) searchEnd = json.length();
        std::wstring context = json.substr(searchStart, searchEnd - searchStart);

        size_t feedPos = context.find(L"\"feedUrl\"");
        if (feedPos != std::wstring::npos) {
            size_t fvalueStart = context.find(L":", feedPos);
            if (fvalueStart != std::wstring::npos) {
                size_t fstrStart = context.find(L"\"", fvalueStart + 1);
                if (fstrStart != std::wstring::npos) {
                    fstrStart++;
                    size_t fstrEnd = context.find(L"\"", fstrStart);
                    if (fstrEnd != std::wstring::npos) {
                        r.feedUrl = context.substr(fstrStart, fstrEnd - fstrStart);
                    }
                }
            }
        }

        // Extract artistName
        size_t artistPos = context.find(L"\"artistName\"");
        if (artistPos != std::wstring::npos) {
            size_t avalueStart = context.find(L":", artistPos);
            if (avalueStart != std::wstring::npos) {
                size_t astrStart = context.find(L"\"", avalueStart + 1);
                if (astrStart != std::wstring::npos) {
                    astrStart++;
                    size_t astrEnd = context.find(L"\"", astrStart);
                    if (astrEnd != std::wstring::npos) {
                        r.artistName = context.substr(astrStart, astrEnd - astrStart);
                    }
                }
            }
        }

        if (!r.name.empty() && !r.feedUrl.empty()) {
            results.push_back(r);
        }

        pos++;
    }

    return !results.empty();
}

// Refresh podcast subscriptions list
static void RefreshPodcastSubsList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_podcastSubs = GetPodcastSubscriptions();
    for (const auto& sub : g_podcastSubs) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(sub.name.c_str()));
    }
}

// Subclass proc for podcast description edit - prevents text selection
static WNDPROC g_origDescProc = nullptr;
static LRESULT CALLBACK PodcastDescSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LRESULT result = CallWindowProcW(g_origDescProc, hwnd, msg, wParam, lParam);
    if (msg == WM_SETFOCUS || msg == WM_SETTEXT) {
        // Deselect any text after focus or text change
        SendMessageW(hwnd, EM_SETSEL, 0, 0);
    }
    return result;
}

// Build a human-readable diagnostic report from a failed feed fetch/parse
static std::wstring BuildPodcastDiagMessage(const std::wstring& feedUrl, const PodcastFetchDiag& diag) {
    std::wstring msg;
    msg += L"Feed URL:\r\n";
    msg += feedUrl;
    msg += L"\r\n\r\n";

    wchar_t num[32];
    msg += L"HTTP status: ";
    if (diag.statusCode > 0) {
        swprintf(num, 32, L"%lu", diag.statusCode);
        msg += num;
    } else {
        msg += L"(not reached)";
    }
    msg += L"\r\n";

    if (diag.lastError != 0) {
        swprintf(num, 32, L"%lu", diag.lastError);
        msg += L"Network error: ";
        msg += num;
        if (!diag.errorText.empty()) {
            msg += L" - " + diag.errorText;
        }
        msg += L"\r\n";
    } else if (!diag.errorText.empty()) {
        msg += L"Error: " + diag.errorText + L"\r\n";
    }

    swprintf(num, 32, L"%zu", diag.bytesReceived);
    msg += L"Bytes received: ";
    msg += num;
    msg += L"\r\n";

    swprintf(num, 32, L"%d", diag.itemTagsFound);
    msg += L"<item> tags in XML: ";
    msg += num;
    msg += L"\r\n";

    swprintf(num, 32, L"%d", diag.episodesExtracted);
    msg += L"Episodes extracted (had audio URL + title): ";
    msg += num;
    msg += L"\r\n";

    if (!diag.bodyPreview.empty()) {
        msg += L"\r\nResponse preview (first 400 chars):\r\n";
        msg += diag.bodyPreview;
    }

    return msg;
}

// Deferred description-update message — posted from the listbox subclass
// after the default proc handles a key/click, so the listbox finishes its
// own selection bookkeeping and accessibility events BEFORE we rewrite the
// description edit.
static const UINT WM_PODCAST_UPDATE_DESC = WM_APP + 12;
// Track the last caret index we updated the description for so we don't
// rewrite the description on Ctrl+Space toggles or other events that don't
// move the caret.
static int g_lastDescCaret = -1;
static WNDPROC g_origPodcastEpsProc = nullptr;

// Helper: post a deferred description update if the caret moved.
static void NotifyEpsCaretMoved(HWND hwnd) {
    int caret = static_cast<int>(SendMessageW(hwnd, LB_GETCARETINDEX, 0, 0));
    if (caret != g_lastDescCaret) {
        g_lastDescCaret = caret;
        PostMessageW(GetParent(hwnd), WM_PODCAST_UPDATE_DESC, static_cast<WPARAM>(caret), 0);
    }
}

// Subclass for the episodes listbox. Implements Ctrl+Up / Ctrl+Down (move the
// caret without changing selection) and Ctrl+Space (toggle selection of the
// focused item) explicitly, since not all environments honor the listbox's
// built-in extended-selection keyboard interface for caret-only moves. All
// other keys/clicks fall through to the default proc unchanged so plain and
// Shift+arrow extension keep their stock behavior.
static LRESULT CALLBACK PodcastEpsListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
        if (ctrl && !shift && (wParam == VK_UP || wParam == VK_DOWN) && count > 0) {
            int caret = static_cast<int>(SendMessageW(hwnd, LB_GETCARETINDEX, 0, 0));
            int newCaret = caret + (wParam == VK_DOWN ? 1 : -1);
            if (newCaret < 0) newCaret = 0;
            if (newCaret >= count) newCaret = count - 1;
            if (newCaret != caret) {
                // Move just the focus rectangle; preserve selection state of all items.
                SendMessageW(hwnd, LB_SETCARETINDEX, newCaret, FALSE);
                // Fire MSAA focus event so screen readers announce the new item.
                NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, newCaret + 1);
                NotifyEpsCaretMoved(hwnd);
            }
            return 0;
        }
        if (ctrl && !shift && wParam == VK_SPACE && count > 0) {
            int caret = static_cast<int>(SendMessageW(hwnd, LB_GETCARETINDEX, 0, 0));
            if (caret >= 0 && caret < count) {
                int sel = static_cast<int>(SendMessageW(hwnd, LB_GETSEL, caret, 0));
                SendMessageW(hwnd, LB_SETSEL, sel ? FALSE : TRUE, caret);
                // Fire MSAA selection state change so the screen reader announces it.
                NotifyWinEvent(EVENT_OBJECT_SELECTIONADD, hwnd, OBJID_CLIENT, caret + 1);
                NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, caret + 1);
            }
            return 0;
        }
    }
    LRESULT result = CallWindowProcW(g_origPodcastEpsProc, hwnd, msg, wParam, lParam);
    if (msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) {
        NotifyEpsCaretMoved(hwnd);
    }
    return result;
}

// Compute the cleaned plain-text description for an episode, ready for the
// IDC_PODCAST_EP_DESC edit control.
static std::wstring CleanPodcastDescription(const std::wstring& raw) {
    std::wstring desc = raw;
    size_t pos;
    while ((pos = desc.find(L"<br")) != std::wstring::npos) {
        size_t endPos = desc.find(L'>', pos);
        if (endPos != std::wstring::npos) {
            desc.replace(pos, endPos - pos + 1, L"\n");
        } else {
            break;
        }
    }
    while ((pos = desc.find(L"</p>")) != std::wstring::npos) desc.replace(pos, 4, L"\n\n");
    while ((pos = desc.find(L"</div>")) != std::wstring::npos) desc.replace(pos, 6, L"\n");
    while ((pos = desc.find(L'<')) != std::wstring::npos) {
        size_t endPos = desc.find(L'>', pos);
        if (endPos != std::wstring::npos) {
            desc.erase(pos, endPos - pos + 1);
        } else {
            break;
        }
    }
    while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) desc.replace(pos, 6, L" ");
    while ((pos = desc.find(L"&amp;")) != std::wstring::npos) desc.replace(pos, 5, L"&");
    while ((pos = desc.find(L"&quot;")) != std::wstring::npos) desc.replace(pos, 6, L"\"");
    while ((pos = desc.find(L"&apos;")) != std::wstring::npos) desc.replace(pos, 6, L"'");
    while ((pos = desc.find(L"&lt;")) != std::wstring::npos) desc.replace(pos, 4, L"<");
    while ((pos = desc.find(L"&gt;")) != std::wstring::npos) desc.replace(pos, 4, L">");
    while ((pos = desc.find(L"&#39;")) != std::wstring::npos) desc.replace(pos, 5, L"'");
    while ((pos = desc.find(L"\n\n\n")) != std::wstring::npos) desc.erase(pos, 1);
    pos = 0;
    while ((pos = desc.find(L'\n', pos)) != std::wstring::npos) {
        if (pos == 0 || desc[pos - 1] != L'\r') {
            desc.insert(pos, 1, L'\r');
            pos += 2;
        } else {
            pos++;
        }
    }
    return desc;
}

// Load episodes for a subscription
static void LoadPodcastEpisodes(HWND hwnd, const std::wstring& feedUrl) {
    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_EPISODES);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_podcastEpisodes.clear();
    SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, L"");

    Speak(Ts("Loading episodes"));

    std::wstring title;
    PodcastFetchDiag diag;
    if (ParsePodcastFeed(feedUrl, title, g_podcastEpisodes, L"", L"", &diag)) {
        for (const auto& ep : g_podcastEpisodes) {
            std::wstring display = ep.title;
            if (!ep.pubDate.empty()) {
                // Truncate pub date to just the date part
                size_t commaPos = ep.pubDate.find(L',');
                if (commaPos != std::wstring::npos && commaPos + 12 < ep.pubDate.length()) {
                    display += L" (" + ep.pubDate.substr(commaPos + 2, 11) + L")";
                }
            }
            if (!ep.description.empty()) {
                // Clean up description - remove HTML tags and limit length
                std::wstring desc = ep.description;
                // Remove HTML tags
                size_t pos;
                while ((pos = desc.find(L'<')) != std::wstring::npos) {
                    size_t endPos = desc.find(L'>', pos);
                    if (endPos != std::wstring::npos) {
                        desc.erase(pos, endPos - pos + 1);
                    } else {
                        break;
                    }
                }
                // Replace &nbsp; and other entities
                while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L" ");
                }
                while ((pos = desc.find(L"&amp;")) != std::wstring::npos) {
                    desc.replace(pos, 5, L"&");
                }
                while ((pos = desc.find(L"&quot;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L"\"");
                }
                while ((pos = desc.find(L"&apos;")) != std::wstring::npos) {
                    desc.replace(pos, 6, L"'");
                }
                while ((pos = desc.find(L"&lt;")) != std::wstring::npos) {
                    desc.replace(pos, 4, L"<");
                }
                while ((pos = desc.find(L"&gt;")) != std::wstring::npos) {
                    desc.replace(pos, 4, L">");
                }
                // Trim whitespace and collapse multiple spaces
                while (!desc.empty() && (desc[0] == L' ' || desc[0] == L'\n' || desc[0] == L'\r' || desc[0] == L'\t')) {
                    desc.erase(0, 1);
                }
                // Truncate to reasonable length
                if (desc.length() > 150) {
                    desc = desc.substr(0, 147) + L"...";
                }
                if (!desc.empty()) {
                    display += L" - " + desc;
                }
            }
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        }

        // Select first episode (multi-select listbox needs LB_SETSEL + LB_SETCARETINDEX)
        if (!g_podcastEpisodes.empty()) {
            SendMessageW(hList, LB_SETSEL, FALSE, -1);
            SendMessageW(hList, LB_SETSEL, TRUE, 0);
            SendMessageW(hList, LB_SETCARETINDEX, 0, FALSE);
            // Force a description update for the newly selected first episode.
            g_lastDescCaret = -1;
            PostMessageW(hwnd, WM_PODCAST_UPDATE_DESC, 0, 0);
        }

        char buf[64];
        snprintf(buf, sizeof(buf), Ts("%d episodes").c_str(), static_cast<int>(g_podcastEpisodes.size()));
        Speak(buf);
    } else {
        Speak(Ts("Failed to load episodes"));
        ShowTagDialog(T("Podcast Load Failed"), BuildPodcastDiagMessage(feedUrl, diag));
    }
}

// Update visibility of podcast tab controls
static void UpdatePodcastTabVisibility(HWND hwnd, int tab) {
    // Subscriptions tab controls (tab 0)
    int subsCtrls[] = {IDC_PODCAST_SUBS_LIST, IDC_PODCAST_EPISODES, IDC_PODCAST_EP_DESC,
                       IDC_PODCAST_DOWNLOAD, IDC_PODCAST_DOWNLOAD_ALL, IDC_PODCAST_EXPORT_OPML,
                       IDC_PODCAST_REFRESH, IDC_PODCAST_SUBS_LABEL, IDC_PODCAST_EP_LABEL,
                       IDC_PODCAST_SUBS_HELP};
    // Search tab controls (tab 1)
    int searchCtrls[] = {IDC_PODCAST_SEARCH_EDIT, IDC_PODCAST_SEARCH_BTN,
                         IDC_PODCAST_SEARCH_LIST, IDC_PODCAST_IMPORT_OPML, IDC_PODCAST_SUBSCRIBE,
                         IDC_PODCAST_ADD_URL, IDC_PODCAST_SEARCH_LABEL, IDC_PODCAST_SEARCH_HELP};

    for (int id : subsCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int id : searchCtrls) {
        ShowWindow(GetDlgItem(hwnd, id), tab == 1 ? SW_SHOW : SW_HIDE);
    }
}

// Podcast add dialog data
struct PodcastAddData {
    std::wstring url;
    std::wstring username;
    std::wstring password;
};

// Add podcast dialog procedure
static INT_PTR CALLBACK PodcastAddDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static PodcastAddData* pData = nullptr;

    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            pData = reinterpret_cast<PodcastAddData*>(lParam);
            SetDlgItemTextW(hwnd, IDC_PODCAST_FEED_URL, pData->url.c_str());
            SetDlgItemTextW(hwnd, IDC_PODCAST_USERNAME, pData->username.c_str());
            SetDlgItemTextW(hwnd, IDC_PODCAST_PASSWORD, pData->password.c_str());
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t url[512], user[128], pass[128];
                    GetDlgItemTextW(hwnd, IDC_PODCAST_FEED_URL, url, 512);
                    GetDlgItemTextW(hwnd, IDC_PODCAST_USERNAME, user, 128);
                    GetDlgItemTextW(hwnd, IDC_PODCAST_PASSWORD, pass, 128);

                    // Trim whitespace from url
                    std::wstring u = url;
                    while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(0, 1);
                    while (!u.empty() && (u.back() == L' ' || u.back() == L'\t')) u.pop_back();

                    if (u.empty()) {
                        MessageBoxW(hwnd, T("Please enter a feed URL."), T("Add Podcast"), MB_ICONWARNING);
                        return TRUE;
                    }

                    pData->url = u;
                    pData->username = user;
                    pData->password = pass;
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Subclass proc for podcast subscriptions list - handle Delete key
static WNDPROC g_origPodcastSubsListProc = nullptr;

static LRESULT CALLBACK PodcastSubsListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_DELETE) {
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                // Confirm deletion
                std::wstring msg = std::wstring(T("Unsubscribe from \"")) + g_podcastSubs[sel].name + L"\"?";
                if (MessageBoxW(GetParent(hwnd), msg.c_str(), T("Unsubscribe"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    RemovePodcastSubscription(g_podcastSubs[sel].id);
                    RefreshPodcastSubsList(GetParent(hwnd));
                    Speak(Ts("Unsubscribed"));
                }
            }
            return 0;
        } else if ((wParam == VK_UP || wParam == VK_DOWN) && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Reorder podcast with Ctrl+Up/Down
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            int count = static_cast<int>(g_podcastSubs.size());
            int newSel = (wParam == VK_UP) ? sel - 1 : sel + 1;
            if (sel >= 0 && sel < count && newSel >= 0 && newSel < count) {
                std::swap(g_podcastSubs[sel], g_podcastSubs[newSel]);
                UpdatePodcastSortOrders(g_podcastSubs);
                RefreshPodcastSubsList(GetParent(hwnd));
                SendMessageW(hwnd, LB_SETCURSEL, newSel, 0);
                // Speak position feedback
                const std::wstring& name = g_podcastSubs[newSel].name;
                if (newSel == 0)
                    SpeakW(name + T(" moved to top"));
                else
                    SpeakW(name + T(" moved below ") + g_podcastSubs[newSel - 1].name);
            }
            return 0;
        }
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && pmsg->wParam == VK_DELETE) {
            return DLGC_WANTMESSAGE;
        }
        if (pmsg && (pmsg->wParam == VK_UP || pmsg->wParam == VK_DOWN) &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origPodcastSubsListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origPodcastSubsListProc, hwnd, msg, wParam, lParam);
}

// Podcast dialog procedure
static INT_PTR CALLBACK PodcastDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            // Initialize tab control
            HWND hTab = GetDlgItem(hwnd, IDC_PODCAST_TAB);
            TCITEMW tie = {0};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<LPWSTR>(T("Subscriptions"));
            SendMessageW(hTab, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&tie));
            tie.pszText = const_cast<LPWSTR>(T("Search"));
            SendMessageW(hTab, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&tie));

            // Subclass description edit to prevent text selection
            HWND hDesc = GetDlgItem(hwnd, IDC_PODCAST_EP_DESC);
            g_origDescProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hDesc, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PodcastDescSubclassProc)));

            // Subclass subscriptions list to handle Delete key
            HWND hSubsList = GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST);
            g_origPodcastSubsListProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hSubsList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PodcastSubsListSubclassProc)));

            // Subclass episodes list to deferred-update the description on caret moves
            HWND hEpsList = GetDlgItem(hwnd, IDC_PODCAST_EPISODES);
            g_origPodcastEpsProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hEpsList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PodcastEpsListSubclassProc)));

            // Load subscriptions
            RefreshPodcastSubsList(hwnd);
            g_podcastEpisodes.clear();
            g_podcastSearchResults.clear();
            g_currentPodcastId = -1;

            UpdatePodcastTabVisibility(hwnd, 0);
            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST));
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    // Handle Enter key based on focus
                    HWND hFocus = GetFocus();
                    if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST)) {
                        // Load episodes for selected subscription
                        int sel = static_cast<int>(SendMessageW(hFocus, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                            g_currentPodcastId = g_podcastSubs[sel].id;
                            LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_EPISODES));
                        }
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_EPISODES)) {
                        // Play selected episode(s) — multiselect adds all to the playlist
                        int selCount = static_cast<int>(SendMessageW(hFocus, LB_GETSELCOUNT, 0, 0));
                        if (selCount > 0) {
                            std::vector<int> selItems(selCount);
                            SendMessageW(hFocus, LB_GETSELITEMS, selCount, reinterpret_cast<LPARAM>(selItems.data()));
                            g_playlist.clear();
                            for (int idx : selItems) {
                                if (idx >= 0 && idx < static_cast<int>(g_podcastEpisodes.size())) {
                                    g_playlist.push_back(g_podcastEpisodes[idx].audioUrl);
                                }
                            }
                            if (!g_playlist.empty()) {
                                PlayTrack(0, true);
                                if (g_playlist.size() == 1) {
                                    Speak(Ts("Playing"));
                                } else {
                                    char msg[128];
                                    snprintf(msg, sizeof(msg), Ts("Playing %d episodes").c_str(), static_cast<int>(g_playlist.size()));
                                    Speak(msg);
                                }
                            }
                        }
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SEARCH_EDIT)) {
                        // Trigger search
                        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PODCAST_SEARCH_BTN, BN_CLICKED), 0);
                    } else if (hFocus == GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST)) {
                        // Preview/play first episode of selected podcast
                        int sel = static_cast<int>(SendMessageW(hFocus, LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSearchResults.size())) {
                            std::vector<PodcastEpisode> eps;
                            std::wstring title;
                            Speak(Ts("Loading preview"));
                            if (ParsePodcastFeed(g_podcastSearchResults[sel].feedUrl, title, eps) && !eps.empty()) {
                                g_playlist.clear();
                                g_playlist.push_back(eps[0].audioUrl);
                                PlayTrack(0, true);
                                Speak(Ts("Playing"));
                            } else {
                                Speak(Ts("No episodes found"));
                            }
                        }
                    }
                    return TRUE;
                }

                case IDC_PODCAST_REFRESH: {
                    int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                        LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                        UpdatePodcastLastUpdated(g_podcastSubs[sel].id);
                    }
                    return TRUE;
                }

                case IDC_PODCAST_DOWNLOAD: {
                    // Download all currently selected episodes
                    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_EPISODES);
                    int selCount = static_cast<int>(SendMessageW(hList, LB_GETSELCOUNT, 0, 0));
                    if (selCount <= 0) {
                        Speak(Ts("No episode selected"));
                        return TRUE;
                    }

                    if (g_downloadPath.empty()) {
                        Speak(Ts("Please set a downloads folder in Options"));
                        return TRUE;
                    }

                    std::vector<int> selItems(selCount);
                    SendMessageW(hList, LB_GETSELITEMS, selCount, reinterpret_cast<LPARAM>(selItems.data()));

                    // Build download folder path
                    std::wstring downloadFolder = g_downloadPath;
                    if (!downloadFolder.empty() && downloadFolder.back() != L'\\') downloadFolder += L'\\';

                    // If organize by feed is enabled, create subfolder
                    if (g_downloadOrganizeByFeed && g_currentPodcastId >= 0) {
                        for (const auto& sub : g_podcastSubs) {
                            if (sub.id == g_currentPodcastId) {
                                std::wstring feedFolder = SanitizeFilename(sub.name);
                                if (!feedFolder.empty()) {
                                    downloadFolder += feedFolder + L'\\';
                                    CreateDirectoryW(downloadFolder.c_str(), nullptr);
                                }
                                break;
                            }
                        }
                    }

                    std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> downloads;
                    std::set<std::wstring> usedFilenames;
                    int skipped = 0;

                    for (int idx : selItems) {
                        if (idx < 0 || idx >= static_cast<int>(g_podcastEpisodes.size())) continue;
                        const auto& ep = g_podcastEpisodes[idx];
                        if (ep.audioUrl.empty()) {
                            skipped++;
                            continue;
                        }

                        std::wstring baseName = SanitizeFilename(ep.title);
                        if (baseName.empty()) baseName = L"episode";
                        std::wstring ext = GetUrlExtension(ep.audioUrl);
                        std::wstring filename = baseName + ext;
                        std::wstring filepath = downloadFolder + filename;

                        // Skip if file already exists on disk
                        if (GetFileAttributesW(filepath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            skipped++;
                            continue;
                        }

                        // Handle duplicate titles within this batch by adding number suffix
                        int dupCount = 1;
                        while (usedFilenames.count(filename) > 0) {
                            dupCount++;
                            wchar_t suffix[16];
                            swprintf(suffix, 16, L" (%d)", dupCount);
                            filename = baseName + suffix + ext;
                            filepath = downloadFolder + filename;
                        }
                        usedFilenames.insert(filename);

                        downloads.push_back(std::make_tuple(ep.audioUrl, filepath, ep.title));
                    }

                    if (downloads.empty()) {
                        if (skipped > 0) Speak(Ts("All selected episodes already downloaded"));
                        else Speak(Ts("No episodes to download"));
                        return TRUE;
                    }

                    if (downloads.size() == 1) {
                        const auto& d = downloads[0];
                        DownloadManager::Instance().Enqueue(std::get<0>(d), std::get<1>(d), std::get<2>(d));
                        Speak(Ts("Downloading"));
                    } else {
                        DownloadManager::Instance().EnqueueMultiple(downloads);
                        char msg[256];
                        if (skipped > 0) {
                            snprintf(msg, sizeof(msg), Ts("Downloading %d episodes, %d skipped").c_str(), static_cast<int>(downloads.size()), skipped);
                        } else {
                            snprintf(msg, sizeof(msg), Ts("Downloading %d episodes").c_str(), static_cast<int>(downloads.size()));
                        }
                        Speak(msg);
                    }
                    return TRUE;
                }

                case IDC_PODCAST_DOWNLOAD_ALL: {
                    // Download all episodes in the current feed using download queue
                    if (g_podcastEpisodes.empty()) {
                        Speak(Ts("No episodes loaded"));
                        return TRUE;
                    }

                    if (g_downloadPath.empty()) {
                        Speak(Ts("Please set a downloads folder in Options"));
                        return TRUE;
                    }

                    // Build download folder path
                    std::wstring downloadFolder = g_downloadPath;
                    if (!downloadFolder.empty() && downloadFolder.back() != L'\\') downloadFolder += L'\\';

                    // If organize by feed is enabled, create subfolder
                    if (g_downloadOrganizeByFeed && g_currentPodcastId >= 0) {
                        for (const auto& sub : g_podcastSubs) {
                            if (sub.id == g_currentPodcastId) {
                                std::wstring feedFolder = SanitizeFilename(sub.name);
                                if (!feedFolder.empty()) {
                                    downloadFolder += feedFolder + L'\\';
                                    CreateDirectoryW(downloadFolder.c_str(), nullptr);
                                }
                                break;
                            }
                        }
                    }

                    // Build list of downloads for the queue
                    std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> downloads;
                    std::set<std::wstring> usedFilenames;
                    int skipped = 0;

                    for (const auto& ep : g_podcastEpisodes) {
                        if (ep.audioUrl.empty()) {
                            skipped++;
                            continue;
                        }

                        std::wstring baseName = SanitizeFilename(ep.title);
                        if (baseName.empty()) baseName = L"episode";
                        std::wstring ext = GetUrlExtension(ep.audioUrl);
                        std::wstring filename = baseName + ext;
                        std::wstring filepath = downloadFolder + filename;

                        // Skip if file already exists on disk
                        if (GetFileAttributesW(filepath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            skipped++;
                            continue;
                        }

                        // Handle duplicate titles within this batch by adding number suffix
                        int dupCount = 1;
                        while (usedFilenames.count(filename) > 0) {
                            dupCount++;
                            wchar_t suffix[16];
                            swprintf(suffix, 16, L" (%d)", dupCount);
                            filename = baseName + suffix + ext;
                            filepath = downloadFolder + filename;
                        }
                        usedFilenames.insert(filename);

                        downloads.push_back(std::make_tuple(ep.audioUrl, filepath, ep.title));
                    }

                    // Enqueue all downloads (manager handles concurrency limit and speech)
                    if (!downloads.empty()) {
                        DownloadManager::Instance().EnqueueMultiple(downloads);
                    }

                    char msg[256];
                    int downloadCount = static_cast<int>(downloads.size());
                    if (downloadCount == 0) {
                        Speak(Ts("No episodes to download"));
                    } else if (skipped > 0) {
                        snprintf(msg, sizeof(msg), Ts("Downloading %d episodes, %d skipped").c_str(), downloadCount, skipped);
                        Speak(msg);
                    } else {
                        snprintf(msg, sizeof(msg), Ts("Downloading %d episodes").c_str(), downloadCount);
                        Speak(msg);
                    }
                    return TRUE;
                }

                case IDC_PODCAST_EXPORT_OPML: {
                    // Export subscriptions to OPML file
                    if (g_podcastSubs.empty()) {
                        Speak(Ts("No subscriptions to export"));
                        return TRUE;
                    }

                    wchar_t filePath[MAX_PATH] = L"podcasts.opml";
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"OPML Files (*.opml)\0*.opml\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = T("Export OPML");
                    ofn.lpstrDefExt = L"opml";
                    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

                    if (GetSaveFileNameW(&ofn)) {
                        FILE* f = _wfopen(filePath, L"wb");
                        if (f) {
                            // Write UTF-8 BOM
                            fwrite("\xEF\xBB\xBF", 1, 3, f);

                            // Write OPML header
                            fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
                            fprintf(f, "<opml version=\"2.0\">\n");
                            fprintf(f, "  <head>\n");
                            fprintf(f, "    <title>MediaAccess Podcast Subscriptions</title>\n");
                            fprintf(f, "  </head>\n");
                            fprintf(f, "  <body>\n");

                            // Write each subscription
                            for (const auto& sub : g_podcastSubs) {
                                std::string title = WideToUtf8(sub.name);
                                std::string feedUrl = WideToUtf8(sub.feedUrl);

                                // Escape XML special characters
                                std::string escapedTitle, escapedUrl;
                                for (char c : title) {
                                    if (c == '&') escapedTitle += "&amp;";
                                    else if (c == '<') escapedTitle += "&lt;";
                                    else if (c == '>') escapedTitle += "&gt;";
                                    else if (c == '"') escapedTitle += "&quot;";
                                    else if (c == '\'') escapedTitle += "&apos;";
                                    else escapedTitle += c;
                                }
                                for (char c : feedUrl) {
                                    if (c == '&') escapedUrl += "&amp;";
                                    else if (c == '<') escapedUrl += "&lt;";
                                    else if (c == '>') escapedUrl += "&gt;";
                                    else if (c == '"') escapedUrl += "&quot;";
                                    else escapedUrl += c;
                                }

                                fprintf(f, "    <outline type=\"rss\" text=\"%s\" xmlUrl=\"%s\"/>\n",
                                        escapedTitle.c_str(), escapedUrl.c_str());
                            }

                            fprintf(f, "  </body>\n");
                            fprintf(f, "</opml>\n");
                            fclose(f);

                            char msg[128];
                            snprintf(msg, sizeof(msg), Ts("Exported %d subscriptions").c_str(), static_cast<int>(g_podcastSubs.size()));
                            Speak(msg);
                        } else {
                            Speak(Ts("Failed to create file"));
                        }
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SEARCH_BTN: {
                    wchar_t query[256];
                    GetDlgItemTextW(hwnd, IDC_PODCAST_SEARCH_EDIT, query, 256);
                    if (wcslen(query) == 0) return TRUE;

                    Speak(Ts("Searching"));
                    HWND hList = GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST);
                    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

                    if (SearchItunesPodcasts(query, g_podcastSearchResults)) {
                        for (const auto& r : g_podcastSearchResults) {
                            std::wstring display = r.name;
                            if (!r.artistName.empty()) {
                                display += L" - " + r.artistName;
                            }
                            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
                        }
                        SendMessageW(hList, LB_SETCURSEL, 0, 0);
                        SetFocus(hList);
                        char buf[64];
                        snprintf(buf, sizeof(buf), Ts("%d results").c_str(), static_cast<int>(g_podcastSearchResults.size()));
                        Speak(buf);
                    } else {
                        Speak(Ts("No results"));
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SUBSCRIBE: {
                    int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST), LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_podcastSearchResults.size())) {
                        const auto& r = g_podcastSearchResults[sel];
                        if (AddPodcastSubscription(r.name, r.feedUrl, r.imageUrl) > 0) {
                            RefreshPodcastSubsList(hwnd);
                            Speak(Ts("Subscribed"));
                        } else {
                            Speak(Ts("Already subscribed or failed"));
                        }
                    } else {
                        Speak(Ts("Select a podcast first"));
                    }
                    return TRUE;
                }

                case IDC_PODCAST_ADD_URL: {
                    // Use Add Podcast dialog with authentication support
                    PodcastAddData addData;
                    if (DialogBoxParamW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PODCAST_ADD),
                                        hwnd, PodcastAddDlgProc,
                                        reinterpret_cast<LPARAM>(&addData)) == IDOK && !addData.url.empty()) {
                        Speak(Ts("Fetching feed"));
                        std::wstring title;
                        std::vector<PodcastEpisode> eps;
                        PodcastFetchDiag addDiag;
                        if (ParsePodcastFeed(addData.url, title, eps, addData.username, addData.password, &addDiag)) {
                            if (title.empty()) title = T("Unknown Podcast");
                            if (AddPodcastSubscription(title, addData.url) > 0) {
                                RefreshPodcastSubsList(hwnd);
                                Speak(Ts("Podcast added"));
                            } else {
                                Speak(Ts("Already subscribed or failed"));
                            }
                        } else {
                            Speak(Ts("Failed to fetch feed"));
                            ShowTagDialog(T("Podcast Fetch Failed"), BuildPodcastDiagMessage(addData.url, addDiag));
                        }
                    }
                    return TRUE;
                }

                case IDC_PODCAST_IMPORT_OPML: {
                    // Open file dialog to select OPML file
                    wchar_t filePath[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"OPML Files (*.opml;*.xml)\0*.opml;*.xml\0All Files (*.*)\0*.*\0";
                    ofn.lpstrFile = filePath;
                    ofn.nMaxFile = MAX_PATH;
                    ofn.lpstrTitle = T("Import OPML");
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

                    if (GetOpenFileNameW(&ofn)) {
                        std::vector<OpmlFeed> feeds = ParseOpmlFile(filePath);
                        if (feeds.empty()) {
                            Speak(Ts("No feeds found in file"));
                            return TRUE;
                        }

                        int added = 0;
                        int skipped = 0;
                        Speak(Ts("Importing feeds"));

                        for (const auto& feed : feeds) {
                            // Try to fetch the feed to verify it works and get the title
                            std::wstring title = feed.title;
                            std::vector<PodcastEpisode> eps;

                            // If we don't have a title, try to get it from the feed
                            if (title.empty()) {
                                ParsePodcastFeed(feed.feedUrl, title, eps);
                            }

                            if (title.empty()) {
                                title = T("Unknown Podcast");
                            }

                            if (AddPodcastSubscription(title, feed.feedUrl) > 0) {
                                added++;
                            } else {
                                skipped++;
                            }
                        }

                        RefreshPodcastSubsList(hwnd);

                        // Report results
                        char msg[256];
                        if (skipped > 0) {
                            snprintf(msg, sizeof(msg), Ts("Imported %d feeds, %d skipped").c_str(), added, skipped);
                        } else {
                            snprintf(msg, sizeof(msg), Ts("Imported %d feeds").c_str(), added);
                        }
                        Speak(msg);
                    }
                    return TRUE;
                }

                case IDC_PODCAST_SUBS_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Load episodes on double-click
                        int sel = static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), LB_GETCURSEL, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastSubs.size())) {
                            g_currentPodcastId = g_podcastSubs[sel].id;
                            LoadPodcastEpisodes(hwnd, g_podcastSubs[sel].feedUrl);
                            SetFocus(GetDlgItem(hwnd, IDC_PODCAST_EPISODES));
                        }
                    }
                    break;

                case IDC_PODCAST_EPISODES:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Double-click plays only the item that was clicked (the focused one)
                        HWND hList = GetDlgItem(hwnd, IDC_PODCAST_EPISODES);
                        int sel = static_cast<int>(SendMessageW(hList, LB_GETCARETINDEX, 0, 0));
                        if (sel >= 0 && sel < static_cast<int>(g_podcastEpisodes.size())) {
                            g_playlist.clear();
                            g_playlist.push_back(g_podcastEpisodes[sel].audioUrl);
                            PlayTrack(0, true);
                            Speak(Ts("Playing"));
                        }
                    }
                    // (No LBN_SELCHANGE handler — extended-selection listboxes need
                    // arrow/click events to flow through to the default proc without
                    // interference. Description updates happen via the listbox subclass.)
                    break;

                case IDC_PODCAST_SEARCH_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK) {
                        // Subscribe on double-click
                        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_PODCAST_SUBSCRIBE, BN_CLICKED), 0);
                    }
                    break;

                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_NOTIFY: {
            NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (pnmh->idFrom == IDC_PODCAST_TAB && pnmh->code == TCN_SELCHANGE) {
                int tab = static_cast<int>(SendMessageW(pnmh->hwndFrom, TCM_GETCURSEL, 0, 0));
                UpdatePodcastTabVisibility(hwnd, tab);
            }
            break;
        }

        default:
            if (msg == WM_PODCAST_UPDATE_DESC) {
                int sel = static_cast<int>(wParam);
                if (sel >= 0 && sel < static_cast<int>(g_podcastEpisodes.size())) {
                    std::wstring desc = CleanPodcastDescription(g_podcastEpisodes[sel].description);
                    SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, desc.c_str());
                    SendDlgItemMessageW(hwnd, IDC_PODCAST_EP_DESC, WM_VSCROLL, SB_TOP, 0);
                } else {
                    SetDlgItemTextW(hwnd, IDC_PODCAST_EP_DESC, L"");
                }
                return TRUE;
            }
            break;

        case WM_SIZE: {
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);

            // Resize tab control
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_TAB), nullptr, 7, 7, width - 14, height - 42, SWP_NOZORDER);

            // Resize subscriptions list (left side)
            int subsWidth = (width - 28) / 3;
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SUBS_LIST), nullptr, 14, 40, subsWidth, height - 90, SWP_NOZORDER);

            // Resize episodes list (right side, top portion)
            int epsX = 14 + subsWidth + 8;
            int epsWidth = width - epsX - 14;
            int epsHeight = (height - 90) * 55 / 100;  // 55% for episodes list
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_EPISODES), nullptr, epsX, 40, epsWidth, epsHeight, SWP_NOZORDER);

            // Description field below episodes list
            int descY = 40 + epsHeight + 4;
            int descHeight = height - 90 - epsHeight - 4;
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_EP_DESC), nullptr, epsX, descY, epsWidth, descHeight, SWP_NOZORDER);

            // Reposition buttons
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_ADD_FEED), nullptr, width - 248, height - 46, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_DOWNLOAD), nullptr, width - 184, height - 46, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_DOWNLOAD_ALL), nullptr, width - 130, height - 46, 58, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_REFRESH), nullptr, width - 64, height - 46, 50, 14, SWP_NOZORDER);

            // Search tab controls
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_EDIT), nullptr, 72, 28, width - 140, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_BTN), nullptr, width - 64, 27, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SEARCH_LIST), nullptr, 14, 48, width - 28, height - 120, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_SUBSCRIBE), nullptr, width - 190, height - 66, 55, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_IMPORT_OPML), nullptr, width - 130, height - 66, 60, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_PODCAST_ADD_URL), nullptr, width - 65, height - 66, 55, 14, SWP_NOZORDER);

            // Close button
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr, width - 64, height - 28, 50, 14, SWP_NOZORDER);

            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 400;
            mmi->ptMinTrackSize.y = 250;
            return 0;
        }
    }
    return FALSE;
}

// Show podcast dialog
void ShowPodcastDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PODCAST), g_hwnd, PodcastDlgProc);
}
