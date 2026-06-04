#pragma once
#ifndef MEDIAACCESS_DATABASE_H
#define MEDIAACCESS_DATABASE_H

#include <string>
#include <vector>

// Bookmark structure
struct Bookmark {
    int id;
    std::wstring filePath;
    double position;
    std::wstring displayName;  // filename + position for display
    int64_t timestamp;
};

// Radio station structure
struct RadioStation {
    int id;
    std::wstring name;
    std::wstring url;
    int64_t timestamp;
    int sortOrder;
};

// Podcast subscription structure
struct PodcastSubscription {
    int id;
    std::wstring name;
    std::wstring feedUrl;
    std::wstring imageUrl;
    int64_t lastUpdated;
    int sortOrder;
};

// Song history entry (a recently played item)
struct SongHistoryEntry {
    int id;
    std::wstring title;
    std::wstring source;    // v2.11 — playable target: file path / URL / YouTube
                            // videoId. Empty = legacy row, not replayable.
    int sourceType = 0;     // v2.11 — mirrors SourceType (ui.h); 0 = None/legacy.
                            // Stored as int to keep this header free of ui.h.
    int64_t timestamp;      // Unix timestamp when captured
};

// Podcast episode structure (not stored in DB - fetched from RSS)
struct PodcastEpisode {
    std::wstring title;
    std::wstring description;
    std::wstring audioUrl;
    std::wstring pubDate;
    int durationSeconds;
    std::wstring guid;
    std::wstring chaptersUrl;   // Podcast 2.0 <podcast:chapters url="..."/> if present
};

// Schedule action type
enum class ScheduleAction {
    Playback = 0,
    Recording = 1,
    Both = 2
};

// Schedule source type
enum class ScheduleSource {
    File = 0,
    Radio = 1
};

// Schedule repeat type
enum class ScheduleRepeat {
    None = 0,
    Daily = 1,
    Weekly = 2,
    Weekdays = 3,
    Weekends = 4,
    Monthly = 5
};

// What to stop when duration expires (for "Both" action)
enum class ScheduleStopAction {
    StopBoth = 0,
    StopPlayback = 1,
    StopRecording = 2
};

// Scheduled event structure
struct ScheduledEvent {
    int id;
    std::wstring name;
    ScheduleAction action;
    ScheduleSource sourceType;
    std::wstring sourcePath;    // File path or radio station URL
    int radioStationId;         // Radio station ID (0 if file)
    int64_t scheduledTime;      // Unix timestamp of next scheduled run
    ScheduleRepeat repeat;
    bool enabled;
    int64_t lastRun;            // Unix timestamp of last run
    int duration;               // Duration in minutes (0 = no limit)
    ScheduleStopAction stopAction;  // What to stop when duration expires
    std::wstring displayName;   // For display in list
};

// Initialize database (call once at startup)
bool InitDatabase();

// Close database (call at shutdown)
void CloseDatabase();

// File position operations
void SaveFilePositionDB(const std::wstring& filePath, double position);
double LoadFilePositionDB(const std::wstring& filePath);

// Bookmark operations
int AddBookmark(const std::wstring& filePath, double position);
bool RemoveBookmark(int id);
std::vector<Bookmark> GetAllBookmarks();

// Radio station operations
int AddRadioStation(const std::wstring& name, const std::wstring& url);
bool RemoveRadioStation(int id);
bool RenameRadioStation(int id, const std::wstring& newName);
bool UpdateRadioStation(int id, const std::wstring& newName, const std::wstring& newUrl);
std::vector<RadioStation> GetRadioFavorites();
bool UpdateRadioSortOrders(const std::vector<RadioStation>& stations);
bool ResetRadioSortOrder();

// Podcast subscription operations
int AddPodcastSubscription(const std::wstring& name, const std::wstring& feedUrl,
                           const std::wstring& imageUrl = L"");
bool RemovePodcastSubscription(int id);
bool UpdatePodcastSubscription(int id, const std::wstring& name, const std::wstring& feedUrl);
bool UpdatePodcastLastUpdated(int id);
std::vector<PodcastSubscription> GetPodcastSubscriptions();
bool UpdatePodcastSortOrders(const std::vector<PodcastSubscription>& subs);
bool ResetPodcastSortOrder();

// Schedule operations
int AddScheduledEvent(const std::wstring& name, ScheduleAction action,
                      ScheduleSource sourceType, const std::wstring& sourcePath,
                      int radioStationId, int64_t scheduledTime,
                      ScheduleRepeat repeat, bool enabled,
                      int duration, ScheduleStopAction stopAction);
bool RemoveScheduledEvent(int id);
bool UpdateScheduledEventEnabled(int id, bool enabled);
bool UpdateScheduledEventLastRun(int id, int64_t lastRun);
bool UpdateScheduledEventTime(int id, int64_t scheduledTime);
bool UpdateScheduledEvent(int id, const std::wstring& name, ScheduleAction action,
                          ScheduleSource sourceType, const std::wstring& sourcePath,
                          int radioStationId, int64_t scheduledTime,
                          ScheduleRepeat repeat, bool enabled,
                          int duration, ScheduleStopAction stopAction);
std::vector<ScheduledEvent> GetAllScheduledEvents();
std::vector<ScheduledEvent> GetPendingScheduledEvents();

// Song history operations (a list of recently played items).
// v2.11 — `source` is the playable target (file path / URL / YouTube videoId)
// and `sourceType` mirrors SourceType so the history window can replay the entry.
// Both default to empty/0 for callers that only have a title (legacy/non-replayable).
void AddSongHistoryEntry(const std::wstring& title,
                         const std::wstring& source = L"",
                         int sourceType = 0);
std::vector<SongHistoryEntry> GetSongHistory();
void ClearSongHistory();
// v2.11 — prune to the configured g_historyLimit (FIFO, drops oldest). Called at
// startup and when the option changes so an over-cap DB shrinks immediately.
void PruneSongHistoryToLimit();

// =============================================================================
// Book library (DAISY / EPUB reader — Phase 1)
// =============================================================================

struct BookEntry {
    int          id = 0;             // 0 means "not found"
    std::wstring path;
    std::wstring title;
    std::wstring author;
    std::wstring format;             // "daisy202" / "daisy3" / "epub3"
    double       totalDuration = 0;  // seconds
    int64_t      lastOpened = 0;     // unix timestamp
    int          positionClip = 0;
    double       positionOffset = 0;
};

struct BookBookmark {
    int          id = 0;
    int          bookId = 0;
    int          clipIndex = 0;
    double       offsetSeconds = 0;
    std::wstring note;
    int64_t      created = 0;
};

// Insert a book if its path is new, or update title/author/format/totalDuration
// if it already exists. Position fields are NOT touched (so re-scanning the
// library doesn't lose the user's last-read position). Returns the book id.
int UpsertBook(const std::wstring& path, const std::wstring& title,
               const std::wstring& author, const std::wstring& format,
               double totalDuration);

bool UpdateBookPosition(int bookId, int clipIndex, double offsetSeconds);
bool MarkBookOpened(int bookId);          // Touch last_opened to now
bool RemoveBook(int bookId);              // Also removes its bookmarks

BookEntry GetBookByPath(const std::wstring& path);  // id=0 if not found
BookEntry GetBookById(int bookId);                  // id=0 if not found
std::vector<BookEntry> GetAllBooks();               // Sorted by last_opened DESC

int  AddBookBookmark(int bookId, int clipIndex, double offsetSeconds,
                     const std::wstring& note);
bool RemoveBookBookmark(int bookmarkId);
bool UpdateBookBookmarkNote(int bookmarkId, const std::wstring& note);
std::vector<BookBookmark> GetBookBookmarks(int bookId);

bool AddBookLibraryFolder(const std::wstring& path);
bool RemoveBookLibraryFolder(const std::wstring& path);
std::vector<std::wstring> GetBookLibraryFolders();

#endif // MEDIAACCESS_DATABASE_H
