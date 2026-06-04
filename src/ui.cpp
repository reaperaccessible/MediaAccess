#include "ui_internal.h"
#include "video_engine.h"
#include "mediaaccess/translations.h"
#include "mediaaccess/wasapi_loopback.h"  // v1.94 — system-capture state for status bar
#include <shlwapi.h>
#include <vector>

// Constants
const wchar_t* const APP_NAME_INTERNAL = L"MediaAccess";

// Pull the localized source label. For SourceType::Local with no explicit
// source provided, resolve "(Local)" at display time via T() so a language
// change is reflected immediately on the next refresh.
static std::wstring NowPlayingSourceLabel() {
    if (g_nowPlayingType == SourceType::Local && g_nowPlayingSource.empty()) {
        return T("(Local)");
    }
    return g_nowPlayingSource;
}

// Update window title — composes "MediaAccess - <source> - <item>" using
// the now-playing globals. Falls back to GetTagTitle() / MPV title /
// filename when the explicit item is empty (e.g. between Play and the
// first ICY metadata event for net streams).
void UpdateWindowTitle() {
    std::wstring title = APP_NAME_INTERNAL;

    if (g_showTitleInWindow && g_nowPlayingType != SourceType::None) {
        std::wstring src = NowPlayingSourceLabel();
        std::wstring itm = g_nowPlayingItem;
        if (itm.empty()) {
            if (g_activeEngine == PlaybackEngine::MPV) {
                itm = MPVGetMediaTitle();
            } else {
                std::wstring t = GetTagTitle();
                // v1.85 — also compare against the localized placeholders so
                // the French build does not display "Aucun titre" / "Aucune
                // lecture en cours" as the now-playing item after a paste or
                // Ctrl+O (companion fix to the v1.78 localized-fallback patch
                // in PlayTrack — that one missed UpdateWindowTitle). Sèb.
                if (!t.empty() &&
                    t != L"No title" && t != L"Nothing playing" &&
                    t != T("No title") && t != T("Nothing playing")) {
                    itm = t;
                }
            }
        }
        std::wstring composed;
        if (src.empty() && itm.empty()) {
            // Last-resort fallback — show the filename of the active playlist
            // entry. Mainly kicks in for local files with no tags before
            // SetNowPlaying has settled.
            if (g_currentTrack >= 0 &&
                g_currentTrack < static_cast<int>(g_playlist.size())) {
                composed = GetFileName(g_playlist[g_currentTrack]);
            }
        } else if (src.empty()) {
            composed = itm;
        } else if (itm.empty()) {
            composed = src;
        } else {
            composed = src + L" - " + itm;
        }
        if (!composed.empty()) {
            title += L" - ";
            title += composed;
        }
    }

    SetWindowTextW(g_hwnd, title.c_str());
}

// Now-playing helpers. Each one updates the relevant globals and then
// refreshes the window title in one go so callers can't forget to mirror
// the change in the title bar.
void SetNowPlaying(SourceType type,
                   const std::wstring& source,
                   const std::wstring& item) {
    g_nowPlayingType   = type;
    g_nowPlayingSource = source;
    g_nowPlayingItem   = item;
    UpdateWindowTitle();

    // v2.11 (issue #3) — record every started item into the play history so it
    // covers ALL sources (local files, YouTube, podcasts, local video…), not
    // only radio ICY metadata. EXCLUDES books: DAISY/EPUB now-playing is a
    // navigation label that changes on every move and would flood the history
    // (books are kept via their own library, not the play history). Radio song
    // titles arrive via SetNowPlayingItem (ICY) and are recorded on that path.
    // AddSongHistoryEntry already drops empty titles and consecutive duplicates,
    // so a title refresh with the same item won't pile up.
    if (type != SourceType::None && type != SourceType::Book) {
        if (!item.empty()) {
            AddSongHistoryEntry(item);
        } else if (type == SourceType::RadioFavorite ||
                   type == SourceType::RadioUrl) {
            // Radio passes the station name in `source` with an empty item, so
            // record the station so a station that sends no ICY song metadata
            // still shows up in the history. An ad-hoc RadioUrl before its
            // icy-name arrives has an empty source — AddSongHistoryEntry drops
            // empty titles. NOT applied to YouTube/Podcast, to avoid recording
            // placeholder calls like SetNowPlaying(YouTube, "YouTube", "").
            AddSongHistoryEntry(source);
        }
    }
}

void SetNowPlayingItem(const std::wstring& item) {
    g_nowPlayingItem = item;
    UpdateWindowTitle();
}

void ClearNowPlaying() {
    g_nowPlayingType   = SourceType::None;
    g_nowPlayingSource.clear();
    g_nowPlayingItem.clear();
    UpdateWindowTitle();
}

// Compose the spoken "<source> - <item>" string used by SpeakTagTitle and
// by the WM_ACTIVATEAPP auto-announce. Mirrors UpdateWindowTitle's
// composition exactly so what's spoken matches what's displayed.
std::wstring BuildNowPlayingSpeech() {
    if (g_nowPlayingType == SourceType::None) return L"";
    std::wstring src = NowPlayingSourceLabel();
    std::wstring itm = g_nowPlayingItem;
    if (itm.empty()) {
        if (g_activeEngine == PlaybackEngine::MPV) {
            itm = MPVGetMediaTitle();
        } else {
            std::wstring t = GetTagTitle();
            if (!t.empty() && t != L"No title" && t != L"Nothing playing") {
                itm = t;
            }
        }
    }
    if (src.empty() && itm.empty()) {
        if (g_currentTrack >= 0 &&
            g_currentTrack < static_cast<int>(g_playlist.size())) {
            return GetFileName(g_playlist[g_currentTrack]);
        }
        return L"";
    }
    if (src.empty()) return itm;
    if (itm.empty()) return src;
    return src + L" - " + itm;
}

// Update status bar with position, volume, state
void UpdateStatusBar() {
    if (!g_statusBar || g_isLoading || g_isBusy) return;

    // Position part
    std::wstring posText = L"--:-- / --:--";
    std::wstring stateText;

    // MPV active: query video engine for position/state. Times are scaled
    // by the effective playback speed so a 2x-faster video shows half the
    // total / position (matches Arnaud's VoiceDream-style request).
    if (g_activeEngine == PlaybackEngine::MPV) {
        double speed = GetEffectivePlaybackSpeed();
        double pos = MPVGetPosition() / speed;
        double len = MPVGetLength()   / speed;
        if (len > 0) posText = FormatTime(pos) + L" / " + FormatTime(len);
        if (MPVIsPlaying()) stateText = T("Playing");
        else if (MPVIsPaused()) stateText = T("Paused");
        else stateText = T("Stopped");
        if (g_isVideoPlaying) {
            if (!stateText.empty()) stateText += L" | ";
            stateText += T("Video");
        }
        SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_POSITION, reinterpret_cast<LPARAM>(posText.c_str()));
        wchar_t volBuf[32];
        swprintf(volBuf, 32, L"%s: %d%%", T("Vol"), static_cast<int>(g_volume * 100 + 0.5f));
        SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_VOLUME, reinterpret_cast<LPARAM>(volBuf));
        SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_STATE, reinterpret_cast<LPARAM>(stateText.c_str()));
        return;
    }

    if (g_fxStream) {
        // Use tempo processor to get position and length. Both are scaled
        // by the effective tempo×rate so that the status bar reports real
        // wall-clock time. A 33-minute file played at 3x shows 0:00 / 11:00.
        TempoProcessor* processor = GetTempoProcessor();
        if (processor && processor->IsActive()) {
            double speed = GetEffectivePlaybackSpeed();
            double pos = processor->GetPosition() / speed;
            double len = processor->GetLength()  / speed;
            if (len > 0) {
                posText = FormatTime(pos) + L" / " + FormatTime(len);
            }
        }

        DWORD state = BASS_ChannelIsActive(g_fxStream);
        switch (state) {
            case BASS_ACTIVE_PLAYING: stateText = T("Playing"); break;
            case BASS_ACTIVE_PAUSED:  stateText = T("Paused"); break;
            case BASS_ACTIVE_STOPPED: stateText = T("Stopped"); break;
            default: stateText = L""; break;
        }

        // Add bitrate if available
        int bitrate = GetCurrentBitrate();
        if (bitrate > 0) {
            if (!stateText.empty()) stateText += L" | ";
            wchar_t brBuf[64];
            // Check if VBR. Format strings routed through T() so the status
            // bar (read aloud by NVDA) stays in the active UI language.
            // "kbps" and "VBR" are universal abbreviations; only the number
            // format differs.
            float vbr = 0;
            if (g_sourceStream && BASS_ChannelGetAttribute(g_sourceStream, BASS_ATTRIB_VBR, &vbr) && vbr > 0) {
                swprintf(brBuf, 64, T("~%d kbps VBR"), bitrate);
            } else {
                swprintf(brBuf, 64, T("%d kbps"), bitrate);
            }
            stateText += brBuf;
        }

        // Add recording indicator
        if (g_isRecording) {
            if (!stateText.empty()) stateText += L" | ";
            stateText += T("REC");
        }
    }

    // v1.94 — system-audio (WASAPI loopback) capture indicator. This path runs
    // independently of g_fxStream, so the indicator is added here (outside the
    // g_fxStream block) and works even with no MediaAccess stream loaded.
    if (mediaaccess::IsSystemCapturing()) {
        if (!stateText.empty()) stateText += L" | ";
        stateText += T("REC system");
    }

    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_POSITION, reinterpret_cast<LPARAM>(posText.c_str()));

    // Volume part
    wchar_t volBuf[32];
    swprintf(volBuf, 32, L"%s: %d%%", T("Vol"), static_cast<int>(g_volume * 100 + 0.5f));
    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_VOLUME, reinterpret_cast<LPARAM>(volBuf));

    SendMessageW(g_statusBar, SB_SETTEXTW, SB_PART_STATE, reinterpret_cast<LPARAM>(stateText.c_str()));
}

// Create status bar
void CreateStatusBar(HWND hwnd, HINSTANCE hInstance) {
    g_statusBar = CreateWindowExW(
        0,
        STATUSCLASSNAMEW,
        nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0,
        hwnd,
        nullptr,
        hInstance,
        nullptr
    );

    if (g_statusBar) {
        // Set up parts: position (200px), volume (100px), state (rest)
        int parts[SB_PART_COUNT] = {200, 300, -1};
        SendMessageW(g_statusBar, SB_SETPARTS, SB_PART_COUNT, reinterpret_cast<LPARAM>(parts));
    }
}

// Check if a file extension is a supported audio format
bool IsSupportedAudioExt(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp3", L".wav", L".ogg", L".oga", L".flac", L".m4a", L".m4b", L".wma", L".aac",
        L".opus", L".aiff", L".ape", L".wv", L".mid", L".midi", L".dff", L".dsf"
    };
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) c = towlower(c);
    for (const auto& e : exts) {
        if (lowerExt == e) return true;
    }
    return false;
}

// Check if a file extension is any supported media format (audio OR video).
// Used by clipboard paste so users can copy a .mkv from Explorer and paste
// it into MediaAccess.
bool IsSupportedMediaExt(const std::wstring& ext) {
    if (IsSupportedAudioExt(ext)) return true;
    static const wchar_t* videoExts[] = {
        L".mp4", L".mkv", L".avi", L".mov", L".webm", L".wmv", L".flv",
        L".ts", L".m2ts", L".vob", L".ogv", L".3gp", L".mpg", L".mpeg",
        L".m4v", L".divx", L".rmvb"
    };
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) c = towlower(c);
    for (const auto& e : videoExts) {
        if (lowerExt == e) return true;
    }
    return false;
}

// Expand a single file to all audio files in its folder
// Returns the index of the original file in the expanded list
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles) {
    outFiles.clear();

    // Get directory and filename
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::wstring dir = filePath.substr(0, lastSlash + 1);
    std::wstring targetFile = filePath.substr(lastSlash + 1);

    // Find all audio files in the directory
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring name = fd.cFileName;
            size_t dotPos = name.find_last_of(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = name.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(dir + name);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // Find the index of the original file
    int targetIndex = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (_wcsicmp(GetFileName(files[i]).c_str(), targetFile.c_str()) == 0) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    outFiles = std::move(files);
    return targetIndex;
}

// Get executable path
std::wstring GetExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// Check if file extension is associated with MediaAccess
bool IsExtensionAssociated(const wchar_t* ext) {
    wchar_t keyPath[256];
    swprintf(keyPath, 256, L"Software\\Classes\\%s", ext);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[256] = {0};
    DWORD size = sizeof(value);
    DWORD type;
    bool associated = false;

    if (RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS) {
        associated = (wcscmp(value, L"MediaAccess.AudioFile") == 0);
    }
    RegCloseKey(hKey);
    return associated;
}

// Set or remove file association
void SetFileAssociation(const wchar_t* ext, bool associate) {
    wchar_t extKeyPath[256];
    swprintf(extKeyPath, 256, L"Software\\Classes\\%s", ext);

    if (associate) {
        // Create extension key pointing to our ProgId
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, extKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* progId = L"MediaAccess.AudioFile";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(progId), static_cast<DWORD>((wcslen(progId) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // Create ProgId with shell\open\command
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\MediaAccess.AudioFile", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* desc = L"MediaAccess Audio File";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(desc), static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\MediaAccess.AudioFile\\shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + GetExePath() + L"\" \"%1\"";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    } else {
        // Remove association by deleting the extension key
        RegDeleteKeyW(HKEY_CURRENT_USER, extKeyPath);
    }

    // Notify shell of change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Register all supported file types
void RegisterAllFileTypes() {
    for (int i = 0; i < g_fileAssocCount; i++) {
        if (!IsExtensionAssociated(g_fileAssocs[i].ext)) {
            SetFileAssociation(g_fileAssocs[i].ext, true);
        }
    }
}

// Unregister all supported file types
void UnregisterAllFileTypes() {
    for (int i = 0; i < g_fileAssocCount; i++) {
        if (IsExtensionAssociated(g_fileAssocs[i].ext)) {
            SetFileAssociation(g_fileAssocs[i].ext, false);
        }
    }
}

// Show the file-open dialog and play the chosen file(s). Handles three
// selection modes: a single playlist file (parsed), a single audio/video
// file (optionally expanded to its containing folder when g_loadFolder is
// set), or a multi-selection (Explorer-style — first wstring is the
// directory, subsequent strings are filenames).
void ShowOpenDialog() {
    // Buffer for multiple file selection
    wchar_t szFile[32768] = {0};

    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    // Keep these lists in sync with IsVideoExtension() in player.cpp.
    ofn.lpstrFilter = L"All Supported\0*.mp3;*.mp2;*.mp1;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.m4r;*.mp4;*.wma;*.wmv;*.aac;*.opus;*.aiff;*.aif;*.ape;*.wv;*.alac;*.mid;*.midi;*.rmi;*.kar;*.dff;*.dsf;*.cda;*.mod;*.s3m;*.xm;*.it;*.mtm;*.umx;*.mkv;*.avi;*.mov;*.webm;*.flv;*.ts;*.m2ts;*.vob;*.ogv;*.3gp;*.mpg;*.mpeg;*.m4v;*.divx;*.rmvb;*.m3u;*.m3u8;*.pls\0"
                      L"Audio Files\0*.mp3;*.mp2;*.mp1;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.m4r;*.mp4;*.wma;*.aac;*.opus;*.aiff;*.aif;*.ape;*.wv;*.alac;*.mid;*.midi;*.rmi;*.kar;*.dff;*.dsf;*.cda;*.mod;*.s3m;*.xm;*.it;*.mtm;*.umx\0"
                      L"Video Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.ts;*.m2ts;*.vob;*.ogv;*.3gp;*.mpg;*.mpeg;*.m4v;*.divx;*.rmvb\0"
                      L"Playlists\0*.m3u;*.m3u8;*.pls\0"
                      L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (GetOpenFileNameW(&ofn)) {
        g_playlist.clear();
        g_currentTrack = -1;

        // Check if multiple files selected
        wchar_t* p = szFile;
        std::wstring dir = p;
        p += wcslen(p) + 1;

        int startIndex = 0;
        if (*p == 0) {
            // Single file selected
            // Check if it's a playlist file
            if (IsPlaylistFile(dir)) {
                g_playlist = ParsePlaylist(dir);
            } else if (g_loadFolder && IsSupportedAudioExt(
                           dir.substr(dir.find_last_of(L'.')))) {
                // v1.76 — "Load folder" only makes sense for audio
                // (the use case is "play the whole album when I pick a
                // track"). For a video file selected through File > Open,
                // ExpandFileToFolder would filter the video out of the
                // resulting playlist (its scan only keeps audio extensions),
                // leaving an empty playlist and nothing to play. The user
                // could only work around this by pasting the file from
                // Explorer, which bypasses the folder-expansion path.
                // Skip the expansion for non-audio targets and just load
                // the single file.
                startIndex = ExpandFileToFolder(dir, g_playlist);
            } else {
                g_playlist.push_back(dir);
            }
        } else {
            // Multiple files - first string is directory
            while (*p) {
                std::wstring fullPath = dir + L"\\" + p;
                // Check if it's a playlist file
                if (IsPlaylistFile(fullPath)) {
                    auto entries = ParsePlaylist(fullPath);
                    g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
                } else {
                    g_playlist.push_back(fullPath);
                }
                p += wcslen(p) + 1;
            }
        }

        // Play selected file
        if (!g_playlist.empty()) {
            PlayTrack(startIndex);
        }
    }
}

// Recursively add audio files from a folder
static void AddFilesFromFolderRecursive(const std::wstring& folder, std::vector<std::wstring>& files, int depth) {
    // Limit recursion depth to prevent stack overflow
    if (depth > 32) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = folder + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        // Skip reparse points (junctions, symlinks) to avoid infinite loops
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring fullPath = folder + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse into subdirectory
            AddFilesFromFolderRecursive(fullPath, files, depth + 1);
        } else {
            // Check if it's a supported audio file
            size_t dotPos = fullPath.rfind(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = fullPath.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files) {
    AddFilesFromFolderRecursive(folder, files, 0);
}

// Show folder browser dialog and add all audio files
void ShowAddFolderDialog() {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = T("Select folder to add to playlist");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t folderPath[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, folderPath)) {
            // Collect all audio files recursively
            std::vector<std::wstring> newFiles;
            AddFilesFromFolder(folderPath, newFiles);

            // Sort files alphabetically
            std::sort(newFiles.begin(), newFiles.end());

            if (!newFiles.empty()) {
                // Replace playlist with new files
                g_playlist.clear();
                g_currentTrack = -1;
                for (const auto& file : newFiles) {
                    g_playlist.push_back(file);
                }

                // Start playing from the beginning
                PlayTrack(0);

                Speak(std::to_string(newFiles.size()) + " " + Ts("files loaded"));
            } else {
                Speak(Ts("No audio files found"));
            }
        }
        CoTaskMemFree(pidl);
    }
}

// Get files from clipboard (supports files, folders, and text URLs/paths).
// Tries CF_HDROP first (Explorer copy/cut); if nothing usable was found,
// falls back to CF_UNICODETEXT and treats each non-empty line as a URL or
// path. Folders are expanded recursively to their audio files.
std::vector<std::wstring> GetFilesFromClipboard() {
    std::vector<std::wstring> files;

    if (!OpenClipboard(nullptr)) return files;

    // First try file drop format
    HANDLE hData = GetClipboardData(CF_HDROP);
    if (hData) {
        HDROP hDrop = static_cast<HDROP>(hData);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

        for (UINT i = 0; i < count; i++) {
            // Get required buffer size first
            UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
            if (pathLen > 0 && pathLen < 32768) {
                std::wstring path(pathLen + 1, L'\0');
                if (DragQueryFileW(hDrop, i, &path[0], pathLen + 1)) {
                    // Remove null terminator from string
                    path.resize(wcslen(path.c_str()));

                    DWORD attrs = GetFileAttributesW(path.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES) {
                        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                            // Recursively add folder contents
                            AddFilesFromFolder(path, files);
                        } else {
                            // Check if it's a supported audio file
                            size_t dotPos = path.rfind(L'.');
                            if (dotPos != std::wstring::npos) {
                                std::wstring ext = path.substr(dotPos);
                                if (IsSupportedMediaExt(ext)) {
                                    files.push_back(path);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // If no files from drop, try text format (URLs or file paths)
    if (files.empty()) {
        HANDLE hText = GetClipboardData(CF_UNICODETEXT);
        if (hText) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(hText));
            if (text) {
                std::wstring clipText = text;
                GlobalUnlock(hText);

                // Split by newlines and process each line
                size_t start = 0;
                while (start < clipText.length()) {
                    size_t end = clipText.find_first_of(L"\r\n", start);
                    if (end == std::wstring::npos) end = clipText.length();

                    std::wstring line = clipText.substr(start, end - start);
                    // Trim whitespace
                    while (!line.empty() && (line[0] == L' ' || line[0] == L'\t')) line.erase(0, 1);
                    while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();

                    if (!line.empty()) {
                        // Check if it's a URL
                        if (line.find(L"http://") == 0 || line.find(L"https://") == 0 ||
                            line.find(L"mms://") == 0 || line.find(L"rtsp://") == 0) {
                            files.push_back(line);
                        } else {
                            // Check if it's a valid file/folder path
                            DWORD attrs = GetFileAttributesW(line.c_str());
                            if (attrs != INVALID_FILE_ATTRIBUTES) {
                                if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                                    AddFilesFromFolder(line, files);
                                } else {
                                    std::wstring ext = line;
                                    size_t dotPos = ext.rfind(L'.');
                                    if (dotPos != std::wstring::npos) {
                                        ext = ext.substr(dotPos);
                                        if (IsSupportedAudioExt(ext)) {
                                            files.push_back(line);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    start = end + 1;
                    // Skip consecutive newlines
                    while (start < clipText.length() && (clipText[start] == L'\r' || clipText[start] == L'\n')) start++;
                }
            }
        }
    }

    CloseClipboard();
    return files;
}

// URL dialog procedure
static std::wstring g_urlResult;

INT_PTR CALLBACK URLDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            // Check clipboard for URL
            if (OpenClipboard(hwnd)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* clipText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (clipText) {
                        // Check if clipboard contains a URL
                        if (_wcsnicmp(clipText, L"http://", 7) == 0 ||
                            _wcsnicmp(clipText, L"https://", 8) == 0) {
                            SetDlgItemTextW(hwnd, IDC_URL_EDIT, clipText);
                            // Select all text
                            SendDlgItemMessageW(hwnd, IDC_URL_EDIT, EM_SETSEL, 0, -1);
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            SetFocus(GetDlgItem(hwnd, IDC_URL_EDIT));
            return FALSE;  // Don't set default focus

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t url[2048] = {0};
                    GetDlgItemTextW(hwnd, IDC_URL_EDIT, url, 2048);
                    g_urlResult = url;
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    g_urlResult.clear();
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

// Check if file is a playlist
bool IsPlaylistFile(const std::wstring& path) {
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = towlower(c);
    return (ext == L".m3u" || ext == L".m3u8" || ext == L".pls");
}

// Parse M3U / M3U8 playlist file.
// Format: plain-text, one entry per line; lines starting with '#' are
// comments (#EXTM3U header, #EXTINF metadata) and are skipped. Auto-detects
// a UTF-8 BOM and falls back to ACP for legacy M3Us. Relative paths are
// resolved against the playlist's directory. Directory entries are
// recursively expanded to their audio files.
static std::vector<std::wstring> ParseM3U(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read file as binary first to detect BOM
    FILE* f = _wfopen(playlistPath.c_str(), L"rb");
    if (!f) return entries;

    // Check for UTF-8 BOM
    unsigned char bom[3] = {0};
    fread(bom, 1, 3, f);
    bool isUtf8 = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF);
    if (!isUtf8) {
        fseek(f, 0, SEEK_SET);  // No BOM, rewind
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // Trim whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) start++;
        size_t slen = strlen(start);
        if (slen == 0) continue;
        char* end = start + slen - 1;
        while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }

        // Skip empty lines and comments
        if (*start == '\0' || *start == '#') continue;

        // Convert to wide string (try UTF-8 first, then ACP)
        std::wstring entry;
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, nullptr, 0);
        if (len > 0) {
            entry.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, start, -1, &entry[0], len);
        } else {
            len = MultiByteToWideChar(CP_ACP, 0, start, -1, nullptr, 0);
            if (len <= 0) continue;
            entry.resize(len);
            MultiByteToWideChar(CP_ACP, 0, start, -1, &entry[0], len);
        }
        // Remove null terminator from string
        if (!entry.empty() && entry.back() == L'\0') {
            entry.pop_back();
        }

        if (entry.empty()) continue;

        // Build full path
        std::wstring fullPath;
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    fclose(f);
    return entries;
}

// Parse PLS playlist file.
// Format: INI-style with [playlist] section and File1, File2, ... keys.
// We reuse GetPrivateProfileString rather than rolling our own parser so
// quoted/escaped values are handled per the Windows INI rules. Stops at
// the first missing FileN key (capped at 1000 entries as a safety).
static std::vector<std::wstring> ParsePLS(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read entries using GetPrivateProfileString
    for (int i = 1; i <= 1000; i++) {  // Reasonable max
        wchar_t key[32];
        swprintf(key, 32, L"File%d", i);
        wchar_t value[4096] = {0};
        GetPrivateProfileStringW(L"playlist", key, L"", value, 4096, playlistPath.c_str());
        if (value[0] == L'\0') break;

        std::wstring entry = value;
        std::wstring fullPath;
        // Check if it's a URL or absolute path
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    return entries;
}

// Parse playlist file (M3U or PLS)
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath) {
    size_t dotPos = playlistPath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};

    std::wstring ext = playlistPath.substr(dotPos);
    for (auto& c : ext) c = towlower(c);

    if (ext == L".pls") {
        return ParsePLS(playlistPath);
    } else {
        return ParseM3U(playlistPath);
    }
}

// Show open URL dialog
void ShowOpenURLDialog() {
    g_urlResult.clear();
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_URL), g_hwnd, URLDlgProc) == IDOK) {
        if (!g_urlResult.empty()) {
            // Add URL to playlist and play
            g_playlist.clear();
            g_playlist.push_back(g_urlResult);
            g_currentTrack = -1;
            PlayTrack(0);
        }
    }
}

// Diagnostic: verify the YouTube extractor is functional and reports a version.
// Purely informational — invoked from the Help menu (IDM_HELP_TEST_YOUTUBE) so
// the user can confirm the YouTube + video toolchain is healthy before relying
// on it for playback. Internal implementation uses yt-dlp but the user-facing
// messages refer only to "the YouTube extractor".
void ShowTestYouTubePlayback() {
    std::wstring msg;
    if (g_ytdlpPath.empty() || !PathFileExistsW(g_ytdlpPath.c_str())) {
        msg = T("The YouTube extractor was not found. Please reinstall MediaAccess.");
        MessageBoxW(g_hwnd, msg.c_str(), T("Test YouTube playback"), MB_ICONWARNING | MB_OK);
        return;
    }

    // Run the extractor with --version to confirm it works
    std::wstring cmd = L"\"" + g_ytdlpPath + L"\" --version";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);

    std::string output;
    if (ok) {
        char buf[256];
        DWORD readBytes = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
            output.append(buf, readBytes);
        }
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);

    // Trim whitespace from output
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
        output.pop_back();

    std::wstring version;
    if (!output.empty()) {
        int len = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
        if (len > 1) {
            version.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &version[0], len);
        }
    }

    if (ok && !version.empty()) {
        msg = T("The YouTube extractor is working.\n\n");
        msg += T("Version: ") + version + L"\n";
        msg += T("Video engine: ") + std::wstring(IsMPVAvailable() ? T("available") : T("not available")) + L"\n\n";
        msg += T("If a YouTube video still fails, check the log file at:\n");
        wchar_t base[MAX_PATH] = {0};
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, base);
        msg += std::wstring(base) + L"\\MediaAccess\\mediaaccess.log";
        MessageBoxW(g_hwnd, msg.c_str(), T("Test YouTube playback"), MB_ICONINFORMATION | MB_OK);
    } else {
        msg = T("The YouTube extractor was found but failed to run. Please reinstall MediaAccess.");
        MessageBoxW(g_hwnd, msg.c_str(), T("Test YouTube playback"), MB_ICONERROR | MB_OK);
    }
}

// Jump to time dialog

static double g_jumpTimeResult = -1;

// Parse time string (mm:ss or hh:mm:ss) to seconds
static double ParseTimeString(const wchar_t* str) {
    int h = 0, m = 0, s = 0;

    // Try hh:mm:ss format
    if (swscanf(str, L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600.0 + m * 60.0 + s;
    }
    // Try mm:ss format
    if (swscanf(str, L"%d:%d", &m, &s) == 2) {
        return m * 60.0 + s;
    }
    // Try just seconds
    double secs = 0;
    if (swscanf(str, L"%lf", &secs) == 1) {
        return secs;
    }
    return -1;
}

// Format seconds to time string (mm:ss or hh:mm:ss)
static std::wstring FormatTimeForEdit(double seconds) {
    if (seconds < 0) seconds = 0;
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    int s = static_cast<int>(seconds) % 60;

    wchar_t buf[32];
    if (h > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf(buf, 32, L"%d:%02d", m, s);
    }
    return buf;
}

static INT_PTR CALLBACK JumpToTimeDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            // Prefill with current position
            double pos = GetCurrentPosition();
            std::wstring timeStr = FormatTimeForEdit(pos);
            SetDlgItemTextW(hwnd, IDC_JUMPTIME_EDIT, timeStr.c_str());
            // Select all text
            SendDlgItemMessageW(hwnd, IDC_JUMPTIME_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(hwnd, IDC_JUMPTIME_EDIT));
            return FALSE;  // We set focus manually
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t buf[64];
                    GetDlgItemTextW(hwnd, IDC_JUMPTIME_EDIT, buf, 64);
                    g_jumpTimeResult = ParseTimeString(buf);
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

void ShowJumpToTimeDialog() {
    g_jumpTimeResult = -1;
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_JUMPTOTIME), g_hwnd, JumpToTimeDlgProc) == IDOK) {
        if (g_jumpTimeResult >= 0) {
            SeekToPosition(g_jumpTimeResult);
        }
    }
}

// Effect presets dialogs

static std::wstring g_presetNameResult;

static INT_PTR CALLBACK PresetNameDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG:
            LocalizeDialog(hwnd);
            SetFocus(GetDlgItem(hwnd, IDC_PRESET_NAME));
            return FALSE;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDOK: {
                    wchar_t buf[128] = {0};
                    GetDlgItemTextW(hwnd, IDC_PRESET_NAME, buf, 128);
                    // Trim whitespace and reject characters that would break INI section names
                    std::wstring name = buf;
                    while (!name.empty() && (name.front() == L' ' || name.front() == L'\t')) name.erase(name.begin());
                    while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
                    for (auto& c : name) {
                        if (c == L'[' || c == L']' || c == L'=' || c == L'\r' || c == L'\n') c = L'_';
                    }
                    g_presetNameResult = name;
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

void ShowSaveEffectPresetDialog() {
    g_presetNameResult.clear();
    if (DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_PRESET_NAME), g_hwnd, PresetNameDlgProc) == IDOK) {
        if (!g_presetNameResult.empty()) {
            if (SaveEffectPreset(g_presetNameResult)) {
                std::wstring msg = std::wstring(T("Saved preset")) + L" " + g_presetNameResult;
                SpeakW(msg);
            }
        }
    }
}

void ShowEffectPresetsMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    auto names = GetEffectPresetNames();
    if (names.empty()) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, T("(No presets saved)"));
    } else {
        for (size_t i = 0; i < names.size() && i < 100; i++) {
            AppendMenuW(menu, MF_STRING, IDM_PRESET_BASE + i, names[i].c_str());
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        HMENU deleteMenu = CreatePopupMenu();
        for (size_t i = 0; i < names.size() && i < 100; i++) {
            AppendMenuW(deleteMenu, MF_STRING, IDM_PRESET_DELETE_BASE + i, names[i].c_str());
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)deleteMenu, T("&Delete preset"));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_PRESET_SAVE_NEW, T("&Save current as new preset..."));

    POINT pt;
    GetCursorPos(&pt);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    if (pt.x < rc.left || pt.x > rc.right || pt.y < rc.top || pt.y > rc.bottom) {
        pt.x = rc.left + 20;
        pt.y = rc.top + 40;
    }
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ============================================================================
// Song History Dialog
// ============================================================================

static std::vector<SongHistoryEntry> g_dialogSongHistory;
static WNDPROC g_origHistoryListProc = nullptr;

static std::wstring FormatHistoryTimestamp(int64_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm local;
    localtime_s(&local, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

static void CopyHistoryEntryToClipboard(HWND hwnd, const std::wstring& title) {
    if (title.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (title.size() + 1) * sizeof(wchar_t));
    if (hMem) {
        wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
        wcscpy(pMem, title.c_str());
        GlobalUnlock(hMem);
        SetClipboardData(CF_UNICODETEXT, hMem);
        Speak(Ts("Song copied"));
    }
    CloseClipboard();
}

static void RefreshHistoryList(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_HISTORY_LIST);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    g_dialogSongHistory = GetSongHistory();

    for (const auto& entry : g_dialogSongHistory) {
        std::wstring line = entry.title + L"\t" + FormatHistoryTimestamp(entry.timestamp);
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }

    if (!g_dialogSongHistory.empty()) {
        SendMessageW(hList, LB_SETCURSEL, 0, 0);
    }
}

static LRESULT CALLBACK HistoryListSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            EndDialog(GetParent(hwnd), IDCANCEL);
            return 0;
        }
        // Ctrl+C to copy selected entry's title
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            int sel = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(g_dialogSongHistory.size())) {
                CopyHistoryEntryToClipboard(GetParent(hwnd), g_dialogSongHistory[sel].title);
            }
            return 0;
        }
    } else if (msg == WM_CHAR) {
        // Ctrl+C generates WM_CHAR with control character 3 (ETX) via TranslateMessage,
        // which the listbox would otherwise feed into its prefix-search and move the
        // selection. Swallow it so focus stays on the song the user just copied.
        if (wParam == 3) return 0;
    } else if (msg == WM_GETDLGCODE) {
        MSG* pmsg = reinterpret_cast<MSG*>(lParam);
        if (pmsg && pmsg->wParam == VK_ESCAPE) {
            return DLGC_WANTMESSAGE;
        }
        return CallWindowProcW(g_origHistoryListProc, hwnd, msg, wParam, lParam);
    }
    return CallWindowProcW(g_origHistoryListProc, hwnd, msg, wParam, lParam);
}

static INT_PTR CALLBACK SongHistoryDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            LocalizeDialog(hwnd);
            // Set tab stops on the listbox so title and timestamp line up
            HWND hList = GetDlgItem(hwnd, IDC_HISTORY_LIST);
            int tabStops[1] = {260};
            SendMessageW(hList, LB_SETTABSTOPS, 1, reinterpret_cast<LPARAM>(tabStops));

            g_origHistoryListProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hList, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(HistoryListSubclassProc)));

            RefreshHistoryList(hwnd);
            SetFocus(hList);
            return FALSE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_HISTORY_COPY: {
                    HWND hList = GetDlgItem(hwnd, IDC_HISTORY_LIST);
                    int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
                    if (sel >= 0 && sel < static_cast<int>(g_dialogSongHistory.size())) {
                        CopyHistoryEntryToClipboard(hwnd, g_dialogSongHistory[sel].title);
                    }
                    return TRUE;
                }
                case IDC_HISTORY_CLEAR: {
                    if (MessageBoxW(hwnd, T("Clear all song history?"), T("Song History"),
                                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        ClearSongHistory();
                        RefreshHistoryList(hwnd);
                        SetFocus(GetDlgItem(hwnd, IDC_HISTORY_LIST));
                    }
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hwnd, IDCANCEL);
                    return TRUE;
            }
            break;

        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) break;
            RECT rc;
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            // Resize listbox to fill space, leaving room for buttons at bottom
            SetWindowPos(GetDlgItem(hwnd, IDC_HISTORY_LIST), nullptr,
                7, 22, w - 14, h - 50, SWP_NOZORDER);
            // Reposition buttons at bottom
            SetWindowPos(GetDlgItem(hwnd, IDC_HISTORY_COPY), nullptr,
                7, h - 22, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDC_HISTORY_CLEAR), nullptr,
                62, h - 22, 50, 14, SWP_NOZORDER);
            SetWindowPos(GetDlgItem(hwnd, IDCANCEL), nullptr,
                w - 57, h - 22, 50, 14, SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, TRUE);
            return TRUE;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 300;
            mmi->ptMinTrackSize.y = 200;
            return TRUE;
        }

        case WM_DESTROY: {
            HWND hList = GetDlgItem(hwnd, IDC_HISTORY_LIST);
            if (g_origHistoryListProc) {
                SetWindowLongPtrW(hList, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origHistoryListProc));
                g_origHistoryListProc = nullptr;
            }
            break;
        }
    }
    return FALSE;
}

void ShowSongHistoryDialog() {
    DialogBoxW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDD_SONG_HISTORY), g_hwnd, SongHistoryDlgProc);
}
