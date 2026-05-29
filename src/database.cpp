#include "database.h"
#include "sqlite3.h"
#include "utils.h"
#include "updater.h"
#include <windows.h>
#include <shlobj.h>
#include <ctime>
#include <vector>

static sqlite3* g_db = nullptr;
static std::wstring g_dbPath;

// Initialize database
bool InitDatabase() {
    if (g_db) return true;  // Already initialized

    // Get database path
    if (IsInstalledMode()) {
        // Installed mode: use AppData\Roaming\MediaAccess
        wchar_t appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
            g_dbPath = appDataPath;
            g_dbPath += L"\\MediaAccess";
            CreateDirectoryW(g_dbPath.c_str(), NULL);
            g_dbPath += L"\\MediaAccess.db";
        }
    }

    // Portable mode or fallback: use exe directory
    if (g_dbPath.empty()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        g_dbPath = exePath;
        size_t pos = g_dbPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            g_dbPath = g_dbPath.substr(0, pos + 1);
        }
        g_dbPath += L"MediaAccess.db";
    }

    // Open database (UTF-8 path)
    std::string dbPathUtf8 = WideToUtf8(g_dbPath);
    int rc = sqlite3_open(dbPathUtf8.c_str(), &g_db);
    if (rc != SQLITE_OK) {
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    // Set busy timeout to prevent hangs (5 seconds)
    sqlite3_busy_timeout(g_db, 5000);

    // Enable WAL mode for better concurrency
    char* errMsg = nullptr;
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);

    // Create tables if not exists
    const char* sql =
        "CREATE TABLE IF NOT EXISTS file_positions ("
        "  path TEXT PRIMARY KEY,"
        "  position REAL,"
        "  last_updated INTEGER"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    // Create bookmarks table
    const char* bookmarkSql =
        "CREATE TABLE IF NOT EXISTS bookmarks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT NOT NULL,"
        "  position REAL NOT NULL,"
        "  created INTEGER NOT NULL"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, bookmarkSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
        // Don't fail completely, just log
    }

    // Create radio favorites table
    const char* radioSql =
        "CREATE TABLE IF NOT EXISTS radio_favorites ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  url TEXT NOT NULL,"
        "  created INTEGER NOT NULL"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, radioSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
    }

    // Create podcast subscriptions table
    const char* podcastSql =
        "CREATE TABLE IF NOT EXISTS podcast_subscriptions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  feed_url TEXT NOT NULL UNIQUE,"
        "  image_url TEXT,"
        "  last_updated INTEGER NOT NULL"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, podcastSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
    }

    // Create scheduled events table
    const char* scheduleSql =
        "CREATE TABLE IF NOT EXISTS scheduled_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  action INTEGER NOT NULL,"
        "  source_type INTEGER NOT NULL,"
        "  source_path TEXT NOT NULL,"
        "  radio_station_id INTEGER DEFAULT 0,"
        "  scheduled_time INTEGER NOT NULL,"
        "  repeat_type INTEGER DEFAULT 0,"
        "  enabled INTEGER DEFAULT 1,"
        "  last_run INTEGER DEFAULT 0,"
        "  duration INTEGER DEFAULT 0,"
        "  stop_action INTEGER DEFAULT 0"
        ");";

    errMsg = nullptr;
    rc = sqlite3_exec(g_db, scheduleSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
    }

    // Migration: Add duration and stop_action columns if they don't exist
    sqlite3_exec(g_db, "ALTER TABLE scheduled_events ADD COLUMN duration INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE scheduled_events ADD COLUMN stop_action INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    // Migration: Add sort_order column to radio_favorites and podcast_subscriptions
    sqlite3_exec(g_db, "ALTER TABLE radio_favorites ADD COLUMN sort_order INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE podcast_subscriptions ADD COLUMN sort_order INTEGER DEFAULT 0;",
                 nullptr, nullptr, nullptr);

    // Create song_history table (stream metadata capture)
    const char* songHistorySql =
        "CREATE TABLE IF NOT EXISTS song_history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "title TEXT NOT NULL, "
        "timestamp INTEGER NOT NULL"
        ");";
    sqlite3_exec(g_db, songHistorySql, nullptr, nullptr, nullptr);

    // -------------------------------------------------------------------
    // Book library tables (DAISY / EPUB reader — Phase 1)
    // -------------------------------------------------------------------
    const char* booksSql =
        "CREATE TABLE IF NOT EXISTS books ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  path TEXT UNIQUE NOT NULL,"
        "  title TEXT,"
        "  author TEXT,"
        "  format TEXT,"               // 'daisy202' / 'daisy3' / 'epub3'
        "  total_duration REAL DEFAULT 0,"   // seconds
        "  last_opened INTEGER DEFAULT 0,"   // unix timestamp
        "  position_clip INTEGER DEFAULT 0,"
        "  position_offset REAL DEFAULT 0"
        ");";
    sqlite3_exec(g_db, booksSql, nullptr, nullptr, nullptr);

    const char* bookBookmarksSql =
        "CREATE TABLE IF NOT EXISTS book_bookmarks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  book_id INTEGER NOT NULL,"
        "  clip_index INTEGER NOT NULL,"
        "  offset_seconds REAL NOT NULL,"
        "  note TEXT,"
        "  created INTEGER NOT NULL"
        ");";
    sqlite3_exec(g_db, bookBookmarksSql, nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "CREATE INDEX IF NOT EXISTS idx_book_bookmarks_book ON book_bookmarks(book_id);",
                 nullptr, nullptr, nullptr);

    const char* bookFoldersSql =
        "CREATE TABLE IF NOT EXISTS book_library_folders ("
        "  path TEXT PRIMARY KEY"
        ");";
    sqlite3_exec(g_db, bookFoldersSql, nullptr, nullptr, nullptr);

    return true;
}

// Close database
void CloseDatabase() {
    if (g_db) {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
}

// Save file position to database
void SaveFilePositionDB(const std::wstring& filePath, double position) {
    if (!g_db) return;

    std::string pathUtf8 = WideToUtf8(filePath);

    const char* sql =
        "INSERT OR REPLACE INTO file_positions (path, position, last_updated) "
        "VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, position);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Load file position from database
double LoadFilePositionDB(const std::wstring& filePath) {
    if (!g_db) return 0.0;

    std::string pathUtf8 = WideToUtf8(filePath);
    double position = 0.0;

    const char* sql = "SELECT position FROM file_positions WHERE path = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            position = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    return position;
}

// Add a bookmark, returns the bookmark ID or -1 on failure
int AddBookmark(const std::wstring& filePath, double position) {
    if (!g_db) return -1;

    std::string pathUtf8 = WideToUtf8(filePath);

    const char* sql =
        "INSERT INTO bookmarks (path, position, created) VALUES (?, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, position);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a bookmark by ID
bool RemoveBookmark(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM bookmarks WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Helper to extract filename from path
static std::wstring ExtractFilename(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

// Helper to format position as HH:MM:SS
static std::wstring FormatPosition(double seconds) {
    int totalSecs = static_cast<int>(seconds);
    int hours = totalSecs / 3600;
    int mins = (totalSecs % 3600) / 60;
    int secs = totalSecs % 60;

    wchar_t buf[32];
    if (hours > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", hours, mins, secs);
    } else {
        swprintf(buf, 32, L"%d:%02d", mins, secs);
    }
    return buf;
}

// Get all bookmarks
std::vector<Bookmark> GetAllBookmarks() {
    std::vector<Bookmark> bookmarks;
    if (!g_db) return bookmarks;

    const char* sql = "SELECT id, path, position, created FROM bookmarks ORDER BY created DESC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Bookmark bm;
            bm.id = sqlite3_column_int(stmt, 0);

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            bm.filePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            bm.position = sqlite3_column_double(stmt, 2);
            bm.timestamp = sqlite3_column_int64(stmt, 3);

            // Create display name: filename @ position
            bm.displayName = ExtractFilename(bm.filePath) + L" @ " + FormatPosition(bm.position);

            bookmarks.push_back(bm);
        }
        sqlite3_finalize(stmt);
    }

    return bookmarks;
}

// Add a radio station, returns the station ID or -1 on failure
int AddRadioStation(const std::wstring& name, const std::wstring& url) {
    if (!g_db) return -1;

    std::string nameUtf8 = WideToUtf8(name);
    std::string urlUtf8 = WideToUtf8(url);

    const char* sql =
        "INSERT INTO radio_favorites (name, url, created, sort_order) "
        "VALUES (?, ?, ?, (SELECT CASE WHEN MAX(sort_order) > 0 THEN MAX(sort_order) + 1 ELSE 0 END FROM radio_favorites));";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, urlUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(time(nullptr)));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a radio station by ID
bool RemoveRadioStation(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM radio_favorites WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

bool RenameRadioStation(int id, const std::wstring& newName) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(newName);
    const char* sql = "UPDATE radio_favorites SET name = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

bool UpdateRadioStation(int id, const std::wstring& newName, const std::wstring& newUrl) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(newName);
    std::string urlUtf8 = WideToUtf8(newUrl);
    const char* sql = "UPDATE radio_favorites SET name = ?, url = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, urlUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Get all radio favorites
std::vector<RadioStation> GetRadioFavorites() {
    std::vector<RadioStation> stations;
    if (!g_db) return stations;

    const char* sql = "SELECT id, name, url, created, sort_order FROM radio_favorites "
                      "ORDER BY sort_order ASC, name COLLATE NOCASE ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            RadioStation rs;
            rs.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            rs.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            const char* urlUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            rs.url = Utf8ToWide(urlUtf8 ? urlUtf8 : "");

            rs.timestamp = sqlite3_column_int64(stmt, 3);
            rs.sortOrder = sqlite3_column_int(stmt, 4);

            stations.push_back(rs);
        }
        sqlite3_finalize(stmt);
    }

    return stations;
}

// Update sort_order for all radio stations based on vector order
bool UpdateRadioSortOrders(const std::vector<RadioStation>& stations) {
    if (!g_db) return false;

    const char* sql = "UPDATE radio_favorites SET sort_order = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    for (int i = 0; i < static_cast<int>(stations.size()); i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, i + 1);
        sqlite3_bind_int(stmt, 2, stations[i].id);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

// Reset radio sort order to alphabetical
bool ResetRadioSortOrder() {
    if (!g_db) return false;
    return sqlite3_exec(g_db, "UPDATE radio_favorites SET sort_order = 0;",
                        nullptr, nullptr, nullptr) == SQLITE_OK;
}

// Add a podcast subscription, returns the subscription ID or -1 on failure
int AddPodcastSubscription(const std::wstring& name, const std::wstring& feedUrl,
                           const std::wstring& imageUrl) {
    if (!g_db) return -1;

    std::string nameUtf8 = WideToUtf8(name);
    std::string feedUtf8 = WideToUtf8(feedUrl);
    std::string imageUtf8 = WideToUtf8(imageUrl);

    const char* sql =
        "INSERT INTO podcast_subscriptions (name, feed_url, image_url, last_updated, sort_order) "
        "VALUES (?, ?, ?, ?, (SELECT CASE WHEN MAX(sort_order) > 0 THEN MAX(sort_order) + 1 ELSE 0 END FROM podcast_subscriptions));";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, feedUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, imageUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(time(nullptr)));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a podcast subscription by ID
bool RemovePodcastSubscription(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM podcast_subscriptions WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update a podcast subscription
bool UpdatePodcastSubscription(int id, const std::wstring& newName, const std::wstring& newFeedUrl) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(newName);
    std::string feedUtf8 = WideToUtf8(newFeedUrl);
    const char* sql = "UPDATE podcast_subscriptions SET name = ?, feed_url = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, feedUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update podcast last_updated timestamp
bool UpdatePodcastLastUpdated(int id) {
    if (!g_db) return false;

    const char* sql = "UPDATE podcast_subscriptions SET last_updated = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(time(nullptr)));
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Get all podcast subscriptions
std::vector<PodcastSubscription> GetPodcastSubscriptions() {
    std::vector<PodcastSubscription> subscriptions;
    if (!g_db) return subscriptions;

    const char* sql = "SELECT id, name, feed_url, image_url, last_updated, sort_order "
                      "FROM podcast_subscriptions ORDER BY sort_order ASC, name COLLATE NOCASE ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            PodcastSubscription ps;
            ps.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            ps.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            const char* feedUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            ps.feedUrl = Utf8ToWide(feedUtf8 ? feedUtf8 : "");

            const char* imageUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            ps.imageUrl = Utf8ToWide(imageUtf8 ? imageUtf8 : "");

            ps.lastUpdated = sqlite3_column_int64(stmt, 4);
            ps.sortOrder = sqlite3_column_int(stmt, 5);

            subscriptions.push_back(ps);
        }
        sqlite3_finalize(stmt);
    }

    return subscriptions;
}

// Update sort_order for all podcast subscriptions based on vector order
bool UpdatePodcastSortOrders(const std::vector<PodcastSubscription>& subs) {
    if (!g_db) return false;

    const char* sql = "UPDATE podcast_subscriptions SET sort_order = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

    for (int i = 0; i < static_cast<int>(subs.size()); i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, i + 1);
        sqlite3_bind_int(stmt, 2, subs[i].id);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return true;
}

// Reset podcast sort order to alphabetical
bool ResetPodcastSortOrder() {
    if (!g_db) return false;
    return sqlite3_exec(g_db, "UPDATE podcast_subscriptions SET sort_order = 0;",
                        nullptr, nullptr, nullptr) == SQLITE_OK;
}

// Helper to format schedule time
static std::wstring FormatScheduleTime(int64_t timestamp) {
    time_t t = static_cast<time_t>(timestamp);
    struct tm tm;
    localtime_s(&tm, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M", &tm);
    return buf;
}

// Helper to get action name
static std::wstring GetActionName(ScheduleAction action) {
    switch (action) {
        case ScheduleAction::Playback: return L"Play";
        case ScheduleAction::Recording: return L"Record";
        case ScheduleAction::Both: return L"Play+Record";
        default: return L"Unknown";
    }
}

// Helper to get repeat name
static std::wstring GetRepeatName(ScheduleRepeat repeat) {
    switch (repeat) {
        case ScheduleRepeat::None: return L"Once";
        case ScheduleRepeat::Daily: return L"Daily";
        case ScheduleRepeat::Weekly: return L"Weekly";
        case ScheduleRepeat::Weekdays: return L"Weekdays";
        case ScheduleRepeat::Weekends: return L"Weekends";
        case ScheduleRepeat::Monthly: return L"Monthly";
        default: return L"Unknown";
    }
}

// Add a scheduled event
int AddScheduledEvent(const std::wstring& name, ScheduleAction action,
                      ScheduleSource sourceType, const std::wstring& sourcePath,
                      int radioStationId, int64_t scheduledTime,
                      ScheduleRepeat repeat, bool enabled,
                      int duration, ScheduleStopAction stopAction) {
    if (!g_db) return -1;

    std::string nameUtf8 = WideToUtf8(name);
    std::string pathUtf8 = WideToUtf8(sourcePath);

    const char* sql =
        "INSERT INTO scheduled_events (name, action, source_type, source_path, "
        "radio_station_id, scheduled_time, repeat_type, enabled, last_run, duration, stop_action) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, static_cast<int>(action));
        sqlite3_bind_int(stmt, 3, static_cast<int>(sourceType));
        sqlite3_bind_text(stmt, 4, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, radioStationId);
        sqlite3_bind_int64(stmt, 6, scheduledTime);
        sqlite3_bind_int(stmt, 7, static_cast<int>(repeat));
        sqlite3_bind_int(stmt, 8, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 9, duration);
        sqlite3_bind_int(stmt, 10, static_cast<int>(stopAction));

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return static_cast<int>(sqlite3_last_insert_rowid(g_db));
        }
        sqlite3_finalize(stmt);
    }
    return -1;
}

// Remove a scheduled event
bool RemoveScheduledEvent(int id) {
    if (!g_db) return false;

    const char* sql = "DELETE FROM scheduled_events WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update enabled state
bool UpdateScheduledEventEnabled(int id, bool enabled) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET enabled = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update last run time
bool UpdateScheduledEventLastRun(int id, int64_t lastRun) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET last_run = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, lastRun);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update scheduled time (for repeating events)
bool UpdateScheduledEventTime(int id, int64_t scheduledTime) {
    if (!g_db) return false;

    const char* sql = "UPDATE scheduled_events SET scheduled_time = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, scheduledTime);
        sqlite3_bind_int(stmt, 2, id);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Update full scheduled event
bool UpdateScheduledEvent(int id, const std::wstring& name, ScheduleAction action,
                          ScheduleSource sourceType, const std::wstring& sourcePath,
                          int radioStationId, int64_t scheduledTime,
                          ScheduleRepeat repeat, bool enabled,
                          int duration, ScheduleStopAction stopAction) {
    if (!g_db) return false;

    std::string nameUtf8 = WideToUtf8(name);
    std::string pathUtf8 = WideToUtf8(sourcePath);

    const char* sql =
        "UPDATE scheduled_events SET name = ?, action = ?, source_type = ?, source_path = ?, "
        "radio_station_id = ?, scheduled_time = ?, repeat_type = ?, enabled = ?, "
        "duration = ?, stop_action = ? WHERE id = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, nameUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, static_cast<int>(action));
        sqlite3_bind_int(stmt, 3, static_cast<int>(sourceType));
        sqlite3_bind_text(stmt, 4, pathUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, radioStationId);
        sqlite3_bind_int64(stmt, 6, scheduledTime);
        sqlite3_bind_int(stmt, 7, static_cast<int>(repeat));
        sqlite3_bind_int(stmt, 8, enabled ? 1 : 0);
        sqlite3_bind_int(stmt, 9, duration);
        sqlite3_bind_int(stmt, 10, static_cast<int>(stopAction));
        sqlite3_bind_int(stmt, 11, id);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }
    return false;
}

// Get all scheduled events
std::vector<ScheduledEvent> GetAllScheduledEvents() {
    std::vector<ScheduledEvent> events;
    if (!g_db) return events;

    const char* sql = "SELECT id, name, action, source_type, source_path, "
                      "radio_station_id, scheduled_time, repeat_type, enabled, last_run, "
                      "duration, stop_action "
                      "FROM scheduled_events ORDER BY scheduled_time ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ScheduledEvent ev;
            ev.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            ev.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            ev.action = static_cast<ScheduleAction>(sqlite3_column_int(stmt, 2));
            ev.sourceType = static_cast<ScheduleSource>(sqlite3_column_int(stmt, 3));

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            ev.sourcePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            ev.radioStationId = sqlite3_column_int(stmt, 5);
            ev.scheduledTime = sqlite3_column_int64(stmt, 6);
            ev.repeat = static_cast<ScheduleRepeat>(sqlite3_column_int(stmt, 7));
            ev.enabled = sqlite3_column_int(stmt, 8) != 0;
            ev.lastRun = sqlite3_column_int64(stmt, 9);
            ev.duration = sqlite3_column_int(stmt, 10);
            ev.stopAction = static_cast<ScheduleStopAction>(sqlite3_column_int(stmt, 11));

            // Build display name: [Enabled] Name - Action @ Time (Repeat) [Duration]
            std::wstring enabledStr = ev.enabled ? L"[On] " : L"[Off] ";
            ev.displayName = enabledStr + ev.name + L" - " + GetActionName(ev.action) +
                            L" @ " + FormatScheduleTime(ev.scheduledTime) +
                            L" (" + GetRepeatName(ev.repeat) + L")";
            if (ev.duration > 0) {
                wchar_t durBuf[32];
                swprintf(durBuf, 32, L" [%d min]", ev.duration);
                ev.displayName += durBuf;
            }

            events.push_back(ev);
        }
        sqlite3_finalize(stmt);
    }

    return events;
}

// Get pending events that should trigger now
std::vector<ScheduledEvent> GetPendingScheduledEvents() {
    std::vector<ScheduledEvent> events;
    if (!g_db) return events;

    int64_t now = static_cast<int64_t>(time(nullptr));

    // Get events where: enabled=1, scheduled_time <= now, and last_run < scheduled_time
    const char* sql = "SELECT id, name, action, source_type, source_path, "
                      "radio_station_id, scheduled_time, repeat_type, enabled, last_run, "
                      "duration, stop_action "
                      "FROM scheduled_events "
                      "WHERE enabled = 1 AND scheduled_time <= ? AND last_run < scheduled_time "
                      "ORDER BY scheduled_time ASC;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ScheduledEvent ev;
            ev.id = sqlite3_column_int(stmt, 0);

            const char* nameUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            ev.name = Utf8ToWide(nameUtf8 ? nameUtf8 : "");

            ev.action = static_cast<ScheduleAction>(sqlite3_column_int(stmt, 2));
            ev.sourceType = static_cast<ScheduleSource>(sqlite3_column_int(stmt, 3));

            const char* pathUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            ev.sourcePath = Utf8ToWide(pathUtf8 ? pathUtf8 : "");

            ev.radioStationId = sqlite3_column_int(stmt, 5);
            ev.scheduledTime = sqlite3_column_int64(stmt, 6);
            ev.repeat = static_cast<ScheduleRepeat>(sqlite3_column_int(stmt, 7));
            ev.enabled = sqlite3_column_int(stmt, 8) != 0;
            ev.lastRun = sqlite3_column_int64(stmt, 9);
            ev.duration = sqlite3_column_int(stmt, 10);
            ev.stopAction = static_cast<ScheduleStopAction>(sqlite3_column_int(stmt, 11));

            events.push_back(ev);
        }
        sqlite3_finalize(stmt);
    }

    return events;
}

// Song history operations

void AddSongHistoryEntry(const std::wstring& title) {
    if (!g_db || title.empty()) return;

    std::string titleUtf8 = WideToUtf8(title);

    // Avoid consecutive duplicates: if the most recent entry matches this title, skip.
    const char* checkSql = "SELECT title FROM song_history ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt* checkStmt = nullptr;
    if (sqlite3_prepare_v2(g_db, checkSql, -1, &checkStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(checkStmt) == SQLITE_ROW) {
            const char* prev = reinterpret_cast<const char*>(sqlite3_column_text(checkStmt, 0));
            if (prev && titleUtf8 == prev) {
                sqlite3_finalize(checkStmt);
                return;
            }
        }
        sqlite3_finalize(checkStmt);
    }

    const char* insertSql = "INSERT INTO song_history (title, timestamp) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, insertSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, titleUtf8.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(time(nullptr)));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Trim to last 100 entries
    const char* trimSql =
        "DELETE FROM song_history WHERE id NOT IN ("
        "SELECT id FROM song_history ORDER BY id DESC LIMIT 100"
        ");";
    sqlite3_exec(g_db, trimSql, nullptr, nullptr, nullptr);
}

std::vector<SongHistoryEntry> GetSongHistory() {
    std::vector<SongHistoryEntry> history;
    if (!g_db) return history;

    const char* sql = "SELECT id, title, timestamp FROM song_history ORDER BY id DESC LIMIT 100;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SongHistoryEntry entry;
            entry.id = sqlite3_column_int(stmt, 0);
            const char* titleUtf8 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (titleUtf8) entry.title = Utf8ToWide(titleUtf8);
            entry.timestamp = sqlite3_column_int64(stmt, 2);
            history.push_back(entry);
        }
        sqlite3_finalize(stmt);
    }
    return history;
}

void ClearSongHistory() {
    if (!g_db) return;
    sqlite3_exec(g_db, "DELETE FROM song_history;", nullptr, nullptr, nullptr);
}

// =============================================================================
// Book library implementation (DAISY / EPUB reader — Phase 1)
// =============================================================================

static int64_t NowUnix() {
    return (int64_t)time(nullptr);
}

int UpsertBook(const std::wstring& path, const std::wstring& title,
               const std::wstring& author, const std::wstring& format,
               double totalDuration) {
    if (!g_db || path.empty()) return 0;

    std::string pathU8   = WideToUtf8(path);
    std::string titleU8  = WideToUtf8(title);
    std::string authorU8 = WideToUtf8(author);
    std::string fmtU8    = WideToUtf8(format);

    // Try to find existing by path first.
    int existingId = 0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db, "SELECT id FROM books WHERE path=? LIMIT 1;",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, pathU8.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) existingId = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
    }

    if (existingId != 0) {
        // Update metadata; leave position fields alone.
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(g_db,
            "UPDATE books SET title=?, author=?, format=?, total_duration=? WHERE id=?;",
            -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text  (st, 1, titleU8.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (st, 2, authorU8.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (st, 3, fmtU8.c_str(),    -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(st, 4, totalDuration);
            sqlite3_bind_int   (st, 5, existingId);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return existingId;
    }

    // Insert new
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "INSERT INTO books(path,title,author,format,total_duration,last_opened,position_clip,position_offset)"
        " VALUES(?,?,?,?,?,0,0,0);",
        -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_text  (st, 1, pathU8.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 2, titleU8.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 3, authorU8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text  (st, 4, fmtU8.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 5, totalDuration);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 0;
    return (int)sqlite3_last_insert_rowid(g_db);
}

bool UpdateBookPosition(int bookId, int clipIndex, double offsetSeconds) {
    if (!g_db || bookId <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "UPDATE books SET position_clip=?, position_offset=? WHERE id=?;",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int   (st, 1, clipIndex);
    sqlite3_bind_double(st, 2, offsetSeconds);
    sqlite3_bind_int   (st, 3, bookId);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool MarkBookOpened(int bookId) {
    if (!g_db || bookId <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "UPDATE books SET last_opened=? WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, NowUnix());
    sqlite3_bind_int  (st, 2, bookId);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool RemoveBook(int bookId) {
    if (!g_db || bookId <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM book_bookmarks WHERE book_id=?;",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, bookId);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(g_db, "DELETE FROM books WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, bookId);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// Fill a BookEntry from the current row of a SELECT statement that returns
// (id, path, title, author, format, total_duration, last_opened, position_clip, position_offset)
static BookEntry RowToBookEntry(sqlite3_stmt* st) {
    BookEntry b;
    b.id            = sqlite3_column_int   (st, 0);
    const char* p   = (const char*)sqlite3_column_text(st, 1); if (p) b.path   = Utf8ToWide(p);
    const char* t   = (const char*)sqlite3_column_text(st, 2); if (t) b.title  = Utf8ToWide(t);
    const char* a   = (const char*)sqlite3_column_text(st, 3); if (a) b.author = Utf8ToWide(a);
    const char* f   = (const char*)sqlite3_column_text(st, 4); if (f) b.format = Utf8ToWide(f);
    b.totalDuration = sqlite3_column_double(st, 5);
    b.lastOpened    = sqlite3_column_int64 (st, 6);
    b.positionClip  = sqlite3_column_int   (st, 7);
    b.positionOffset= sqlite3_column_double(st, 8);
    return b;
}

BookEntry GetBookByPath(const std::wstring& path) {
    BookEntry b;
    if (!g_db || path.empty()) return b;
    std::string pathU8 = WideToUtf8(path);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT id,path,title,author,format,total_duration,last_opened,position_clip,position_offset"
        " FROM books WHERE path=? LIMIT 1;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, pathU8.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) b = RowToBookEntry(st);
        sqlite3_finalize(st);
    }
    return b;
}

BookEntry GetBookById(int bookId) {
    BookEntry b;
    if (!g_db || bookId <= 0) return b;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT id,path,title,author,format,total_duration,last_opened,position_clip,position_offset"
        " FROM books WHERE id=? LIMIT 1;", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, bookId);
        if (sqlite3_step(st) == SQLITE_ROW) b = RowToBookEntry(st);
        sqlite3_finalize(st);
    }
    return b;
}

std::vector<BookEntry> GetAllBooks() {
    std::vector<BookEntry> out;
    if (!g_db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT id,path,title,author,format,total_duration,last_opened,position_clip,position_offset"
        " FROM books ORDER BY last_opened DESC, title COLLATE NOCASE ASC;",
        -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.push_back(RowToBookEntry(st));
        sqlite3_finalize(st);
    }
    return out;
}

int AddBookBookmark(int bookId, int clipIndex, double offsetSeconds,
                    const std::wstring& note) {
    if (!g_db || bookId <= 0) return 0;
    std::string noteU8 = WideToUtf8(note);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "INSERT INTO book_bookmarks(book_id,clip_index,offset_seconds,note,created) VALUES(?,?,?,?,?);",
        -1, &st, nullptr) != SQLITE_OK) return 0;
    sqlite3_bind_int   (st, 1, bookId);
    sqlite3_bind_int   (st, 2, clipIndex);
    sqlite3_bind_double(st, 3, offsetSeconds);
    sqlite3_bind_text  (st, 4, noteU8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64 (st, 5, NowUnix());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return 0;
    return (int)sqlite3_last_insert_rowid(g_db);
}

bool RemoveBookBookmark(int bookmarkId) {
    if (!g_db || bookmarkId <= 0) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM book_bookmarks WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, bookmarkId);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool UpdateBookBookmarkNote(int bookmarkId, const std::wstring& note) {
    if (!g_db || bookmarkId <= 0) return false;
    std::string noteU8 = WideToUtf8(note);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "UPDATE book_bookmarks SET note=? WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, noteU8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, bookmarkId);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::vector<BookBookmark> GetBookBookmarks(int bookId) {
    std::vector<BookBookmark> out;
    if (!g_db || bookId <= 0) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT id,book_id,clip_index,offset_seconds,note,created FROM book_bookmarks "
        "WHERE book_id=? ORDER BY clip_index ASC, offset_seconds ASC;",
        -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, bookId);
        while (sqlite3_step(st) == SQLITE_ROW) {
            BookBookmark bm;
            bm.id            = sqlite3_column_int   (st, 0);
            bm.bookId        = sqlite3_column_int   (st, 1);
            bm.clipIndex     = sqlite3_column_int   (st, 2);
            bm.offsetSeconds = sqlite3_column_double(st, 3);
            const char* n    = (const char*)sqlite3_column_text(st, 4);
            if (n) bm.note   = Utf8ToWide(n);
            bm.created       = sqlite3_column_int64 (st, 5);
            out.push_back(bm);
        }
        sqlite3_finalize(st);
    }
    return out;
}

bool AddBookLibraryFolder(const std::wstring& path) {
    if (!g_db || path.empty()) return false;
    std::string pathU8 = WideToUtf8(path);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "INSERT OR IGNORE INTO book_library_folders(path) VALUES(?);",
        -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, pathU8.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool RemoveBookLibraryFolder(const std::wstring& path) {
    if (!g_db || path.empty()) return false;
    std::string pathU8 = WideToUtf8(path);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM book_library_folders WHERE path=?;",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, pathU8.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::vector<std::wstring> GetBookLibraryFolders() {
    std::vector<std::wstring> out;
    if (!g_db) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db,
        "SELECT path FROM book_library_folders ORDER BY path COLLATE NOCASE ASC;",
        -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char* p = (const char*)sqlite3_column_text(st, 0);
            if (p) out.push_back(Utf8ToWide(p));
        }
        sqlite3_finalize(st);
    }
    return out;
}
