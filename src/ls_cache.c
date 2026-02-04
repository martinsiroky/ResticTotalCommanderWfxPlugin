#include "ls_cache.h"
#include "sqlite3.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <shlobj.h>
#include <shlwapi.h>

/* Maximum number of simultaneously open repo databases */
#define MAX_DBS 16

typedef struct {
    char repoName[64];
    sqlite3* db;
    /* Prepared statements */
    sqlite3_stmt* stmtLookupSentinel;
    sqlite3_stmt* stmtLookupEntries;
    sqlite3_stmt* stmtInsertSentinel;
    sqlite3_stmt* stmtInsertEntry;
} DbConn;

static DbConn g_Dbs[MAX_DBS];
static int g_DbCount = 0;
static BOOL g_Initialized = FALSE;
static char g_CacheDir[MAX_PATH] = {0};

/* Build the cache directory path: %APPDATA%\GHISLER\plugins\wfx\restic_wfx\cache\ */
static BOOL EnsureCacheDir(void) {
    char appData[MAX_PATH];
    char dir[MAX_PATH];

    if (g_CacheDir[0] != '\0') return TRUE;

    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData)))
        return FALSE;

    /* Create intermediate directories */
    snprintf(dir, MAX_PATH, "%s\\GHISLER", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins\\wfx", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins\\wfx\\restic_wfx", appData);
    CreateDirectoryA(dir, NULL);

    snprintf(g_CacheDir, MAX_PATH, "%s\\GHISLER\\plugins\\wfx\\restic_wfx\\cache", appData);
    CreateDirectoryA(g_CacheDir, NULL);

    return (GetFileAttributesA(g_CacheDir) != INVALID_FILE_ATTRIBUTES);
}

/* Build DB file path for a repo */
static void GetDbPath(const char* repoName, char* outPath, int maxLen) {
    snprintf(outPath, maxLen, "%s\\%s.db", g_CacheDir, repoName);
}

/* Finalize all prepared statements for a connection */
static void FinalizeStatements(DbConn* conn) {
    if (conn->stmtLookupSentinel) { sqlite3_finalize(conn->stmtLookupSentinel); conn->stmtLookupSentinel = NULL; }
    if (conn->stmtLookupEntries)  { sqlite3_finalize(conn->stmtLookupEntries);  conn->stmtLookupEntries = NULL; }
    if (conn->stmtInsertSentinel) { sqlite3_finalize(conn->stmtInsertSentinel); conn->stmtInsertSentinel = NULL; }
    if (conn->stmtInsertEntry)    { sqlite3_finalize(conn->stmtInsertEntry);    conn->stmtInsertEntry = NULL; }
}

/* Create schema tables if they don't exist */
static BOOL CreateSchema(sqlite3* db) {
    const char* sql =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA busy_timeout=1000;"
        "CREATE TABLE IF NOT EXISTS cached_dirs ("
        "  short_id TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  entry_count INTEGER NOT NULL,"
        "  cached_at INTEGER NOT NULL,"
        "  PRIMARY KEY (short_id, path)"
        ");"
        "CREATE TABLE IF NOT EXISTS dir_entries ("
        "  short_id TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  is_dir INTEGER NOT NULL,"
        "  size_low INTEGER NOT NULL,"
        "  size_high INTEGER NOT NULL,"
        "  mtime_low INTEGER NOT NULL,"
        "  mtime_high INTEGER NOT NULL,"
        "  PRIMARY KEY (short_id, path, name)"
        ");";

    char* errMsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return FALSE;
    }

    /* Set schema version */
    sqlite3_exec(db, "PRAGMA user_version=1;", NULL, NULL, NULL);
    return TRUE;
}

/* Prepare all reusable statements */
static BOOL PrepareStatements(DbConn* conn) {
    int rc;

    rc = sqlite3_prepare_v2(conn->db,
        "SELECT entry_count FROM cached_dirs WHERE short_id=?1 AND path=?2",
        -1, &conn->stmtLookupSentinel, NULL);
    if (rc != SQLITE_OK) return FALSE;

    rc = sqlite3_prepare_v2(conn->db,
        "SELECT name, is_dir, size_low, size_high, mtime_low, mtime_high "
        "FROM dir_entries WHERE short_id=?1 AND path=?2",
        -1, &conn->stmtLookupEntries, NULL);
    if (rc != SQLITE_OK) return FALSE;

    rc = sqlite3_prepare_v2(conn->db,
        "INSERT OR REPLACE INTO cached_dirs (short_id, path, entry_count, cached_at) "
        "VALUES (?1, ?2, ?3, ?4)",
        -1, &conn->stmtInsertSentinel, NULL);
    if (rc != SQLITE_OK) return FALSE;

    rc = sqlite3_prepare_v2(conn->db,
        "INSERT OR REPLACE INTO dir_entries "
        "(short_id, path, name, is_dir, size_low, size_high, mtime_low, mtime_high) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
        -1, &conn->stmtInsertEntry, NULL);
    if (rc != SQLITE_OK) return FALSE;

    return TRUE;
}

/* Open (or reuse) a database connection for the given repo.
   Returns NULL on failure. */
static DbConn* GetConnection(const char* repoName) {
    int i;
    char dbPath[MAX_PATH];
    int rc;

    /* Check for existing connection */
    for (i = 0; i < g_DbCount; i++) {
        if (strcmp(g_Dbs[i].repoName, repoName) == 0)
            return &g_Dbs[i];
    }

    /* Ensure cache directory exists */
    if (!EnsureCacheDir()) return NULL;

    /* No room for more connections */
    if (g_DbCount >= MAX_DBS) return NULL;

    GetDbPath(repoName, dbPath, MAX_PATH);

    /* Open database */
    {
        DbConn* conn = &g_Dbs[g_DbCount];
        memset(conn, 0, sizeof(DbConn));
        strncpy(conn->repoName, repoName, sizeof(conn->repoName) - 1);

        rc = sqlite3_open(dbPath, &conn->db);
        if (rc != SQLITE_OK) {
            /* Try to delete corrupt DB and retry once */
            if (conn->db) sqlite3_close(conn->db);
            conn->db = NULL;
            DeleteFileA(dbPath);

            rc = sqlite3_open(dbPath, &conn->db);
            if (rc != SQLITE_OK) {
                if (conn->db) sqlite3_close(conn->db);
                conn->db = NULL;
                return NULL;
            }
        }

        if (!CreateSchema(conn->db)) {
            /* Schema creation failed — possibly corrupt; delete and retry */
            sqlite3_close(conn->db);
            conn->db = NULL;
            DeleteFileA(dbPath);

            rc = sqlite3_open(dbPath, &conn->db);
            if (rc != SQLITE_OK || !CreateSchema(conn->db)) {
                if (conn->db) sqlite3_close(conn->db);
                conn->db = NULL;
                return NULL;
            }
        }

        if (!PrepareStatements(conn)) {
            FinalizeStatements(conn);
            sqlite3_close(conn->db);
            conn->db = NULL;
            return NULL;
        }

        g_DbCount++;
        return conn;
    }
}

/* --- Public API --- */

void LsCache_Init(void) {
    g_Initialized = TRUE;
    g_DbCount = 0;
    g_CacheDir[0] = '\0';
}

DirEntry* LsCache_Lookup(const char* repoName, const char* shortId,
                          const char* path, int* outCount) {
    DbConn* conn;
    DirEntry* entries = NULL;
    int entryCount = 0;
    int rc;

    *outCount = 0;
    if (!g_Initialized) return NULL;

    conn = GetConnection(repoName);
    if (!conn) return NULL;

    /* Check sentinel: is this (shortId, path) cached? */
    sqlite3_reset(conn->stmtLookupSentinel);
    sqlite3_bind_text(conn->stmtLookupSentinel, 1, shortId, -1, SQLITE_STATIC);
    sqlite3_bind_text(conn->stmtLookupSentinel, 2, path, -1, SQLITE_STATIC);

    rc = sqlite3_step(conn->stmtLookupSentinel);
    if (rc != SQLITE_ROW) {
        /* Not cached */
        return NULL;
    }

    entryCount = sqlite3_column_int(conn->stmtLookupSentinel, 0);

    /* Directory with 0 entries is a valid cache hit */
    if (entryCount == 0) {
        *outCount = 0;
        return NULL;  /* 0 entries but sentinel exists — caller checks outCount */
    }

    /* Fetch actual entries */
    entries = (DirEntry*)malloc(sizeof(DirEntry) * entryCount);
    if (!entries) return NULL;

    sqlite3_reset(conn->stmtLookupEntries);
    sqlite3_bind_text(conn->stmtLookupEntries, 1, shortId, -1, SQLITE_STATIC);
    sqlite3_bind_text(conn->stmtLookupEntries, 2, path, -1, SQLITE_STATIC);

    {
        int idx = 0;
        while ((rc = sqlite3_step(conn->stmtLookupEntries)) == SQLITE_ROW && idx < entryCount) {
            DirEntry* e = &entries[idx];
            const char* name = (const char*)sqlite3_column_text(conn->stmtLookupEntries, 0);

            strncpy(e->name, name ? name : "", MAX_PATH - 1);
            e->name[MAX_PATH - 1] = '\0';
            e->isDirectory = sqlite3_column_int(conn->stmtLookupEntries, 1);
            e->fileSizeLow = (DWORD)sqlite3_column_int64(conn->stmtLookupEntries, 2);
            e->fileSizeHigh = (DWORD)sqlite3_column_int64(conn->stmtLookupEntries, 3);
            e->lastWriteTime.dwLowDateTime = (DWORD)sqlite3_column_int64(conn->stmtLookupEntries, 4);
            e->lastWriteTime.dwHighDateTime = (DWORD)sqlite3_column_int64(conn->stmtLookupEntries, 5);
            idx++;
        }

        if (idx == 0) {
            /* Sentinel said entries exist but none found — stale sentinel */
            free(entries);
            return NULL;
        }

        *outCount = idx;
    }

    return entries;
}

void LsCache_Store(const char* repoName, const char* shortId,
                   const char* path, const DirEntry* entries, int count) {
    DbConn* conn;
    int i;
    char* errMsg = NULL;

    if (!g_Initialized) return;

    conn = GetConnection(repoName);
    if (!conn) return;

    /* Begin transaction */
    if (sqlite3_exec(conn->db, "BEGIN", NULL, NULL, &errMsg) != SQLITE_OK) {
        sqlite3_free(errMsg);
        return;
    }

    /* Insert directory entries */
    for (i = 0; i < count; i++) {
        sqlite3_reset(conn->stmtInsertEntry);
        sqlite3_bind_text(conn->stmtInsertEntry, 1, shortId, -1, SQLITE_STATIC);
        sqlite3_bind_text(conn->stmtInsertEntry, 2, path, -1, SQLITE_STATIC);
        sqlite3_bind_text(conn->stmtInsertEntry, 3, entries[i].name, -1, SQLITE_STATIC);
        sqlite3_bind_int(conn->stmtInsertEntry, 4, entries[i].isDirectory);
        sqlite3_bind_int64(conn->stmtInsertEntry, 5, (sqlite3_int64)entries[i].fileSizeLow);
        sqlite3_bind_int64(conn->stmtInsertEntry, 6, (sqlite3_int64)entries[i].fileSizeHigh);
        sqlite3_bind_int64(conn->stmtInsertEntry, 7, (sqlite3_int64)entries[i].lastWriteTime.dwLowDateTime);
        sqlite3_bind_int64(conn->stmtInsertEntry, 8, (sqlite3_int64)entries[i].lastWriteTime.dwHighDateTime);

        if (sqlite3_step(conn->stmtInsertEntry) != SQLITE_DONE) {
            sqlite3_exec(conn->db, "ROLLBACK", NULL, NULL, NULL);
            return;
        }
    }

    /* Insert sentinel */
    sqlite3_reset(conn->stmtInsertSentinel);
    sqlite3_bind_text(conn->stmtInsertSentinel, 1, shortId, -1, SQLITE_STATIC);
    sqlite3_bind_text(conn->stmtInsertSentinel, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_int(conn->stmtInsertSentinel, 3, count);
    sqlite3_bind_int64(conn->stmtInsertSentinel, 4, (sqlite3_int64)GetTickCount64());

    if (sqlite3_step(conn->stmtInsertSentinel) != SQLITE_DONE) {
        sqlite3_exec(conn->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    /* Commit */
    sqlite3_exec(conn->db, "COMMIT", NULL, NULL, NULL);
}

int LsCache_Purge(const char* repoName, const char** validShortIds, int validCount) {
    DbConn* conn;
    int totalDeleted = 0;
    char* sql = NULL;
    int sqlLen, offset, i;
    char* errMsg = NULL;
    sqlite3_stmt* stmt = NULL;

    if (!g_Initialized) return -1;
    if (validCount <= 0) return 0;

    conn = GetConnection(repoName);
    if (!conn) return -1;

    /* Build parameterized query: DELETE FROM ... WHERE short_id NOT IN (?1, ?2, ...) */
    /* Estimate buffer: base SQL (~80) + 4 chars per param */
    sqlLen = 128 + validCount * 4;
    sql = (char*)malloc(sqlLen);
    if (!sql) return -1;

    offset = snprintf(sql, sqlLen, "DELETE FROM dir_entries WHERE short_id NOT IN (");
    for (i = 0; i < validCount; i++) {
        if (i > 0) offset += snprintf(sql + offset, sqlLen - offset, ",");
        offset += snprintf(sql + offset, sqlLen - offset, "?%d", i + 1);
    }
    snprintf(sql + offset, sqlLen - offset, ")");

    /* Execute for dir_entries */
    if (sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        for (i = 0; i < validCount; i++) {
            sqlite3_bind_text(stmt, i + 1, validShortIds[i], -1, SQLITE_STATIC);
        }
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            totalDeleted = sqlite3_changes(conn->db);
        }
        sqlite3_finalize(stmt);
    }

    /* Same for cached_dirs */
    offset = snprintf(sql, sqlLen, "DELETE FROM cached_dirs WHERE short_id NOT IN (");
    for (i = 0; i < validCount; i++) {
        if (i > 0) offset += snprintf(sql + offset, sqlLen - offset, ",");
        offset += snprintf(sql + offset, sqlLen - offset, "?%d", i + 1);
    }
    snprintf(sql + offset, sqlLen - offset, ")");

    if (sqlite3_prepare_v2(conn->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        for (i = 0; i < validCount; i++) {
            sqlite3_bind_text(stmt, i + 1, validShortIds[i], -1, SQLITE_STATIC);
        }
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            totalDeleted += sqlite3_changes(conn->db);
        }
        sqlite3_finalize(stmt);
    }

    free(sql);
    return totalDeleted;
}

void LsCache_DeleteRepo(const char* repoName) {
    int i;
    char dbPath[MAX_PATH];

    if (!g_Initialized) return;

    /* Close connection if open */
    for (i = 0; i < g_DbCount; i++) {
        if (strcmp(g_Dbs[i].repoName, repoName) == 0) {
            FinalizeStatements(&g_Dbs[i]);
            sqlite3_close(g_Dbs[i].db);
            /* Shift remaining connections down */
            g_DbCount--;
            if (i < g_DbCount) {
                g_Dbs[i] = g_Dbs[g_DbCount];
            }
            break;
        }
    }

    /* Delete the DB file */
    if (EnsureCacheDir()) {
        GetDbPath(repoName, dbPath, MAX_PATH);
        DeleteFileA(dbPath);
        /* Also delete WAL and SHM files */
        snprintf(dbPath, MAX_PATH, "%s\\%s.db-wal", g_CacheDir, repoName);
        DeleteFileA(dbPath);
        snprintf(dbPath, MAX_PATH, "%s\\%s.db-shm", g_CacheDir, repoName);
        DeleteFileA(dbPath);
    }
}

void LsCache_Shutdown(void) {
    int i;

    for (i = 0; i < g_DbCount; i++) {
        FinalizeStatements(&g_Dbs[i]);
        if (g_Dbs[i].db) {
            sqlite3_close(g_Dbs[i].db);
            g_Dbs[i].db = NULL;
        }
    }
    g_DbCount = 0;
    g_Initialized = FALSE;
}
