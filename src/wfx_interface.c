#include "wfx_interface.h"
#include "repo_config.h"
#include "restic_process.h"
#include "json_parse.h"
#include "ls_cache.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <shellapi.h>
#include <shlwapi.h>

/* [All Files] virtual snapshot constants */
#define ALL_FILES_ENTRY    "[All Files]"
#define VERSION_PREFIX     "[v] "
#define VERSION_PREFIX_LEN 4

/* Global plugin state */
static int g_PluginNr = 0;
static tProgressProc g_ProgressProc = NULL;
static tLogProc g_LogProc = NULL;
static tRequestProc g_RequestProc = NULL;

/* --- Batch restore state for FsStatusInfo/FsGetFile optimization --- */

static struct {
    BOOL active;                  /* TRUE after restore completed successfully */
    BOOL pending;                 /* TRUE after FsStatusInfo(START), waiting for first FsGetFile */
    char tempDir[MAX_PATH];       /* temp root where restic restored to */
    char resticPrefix[MAX_PATH];  /* restic internal path prefix, e.g. "/D/Fotky/Mix" */
    char repoPath[512];
    char password[256];
    char snapshotPath[MAX_PATH];  /* original path for --path flag (UTF-8) */
    char shortId[16];
} g_BatchRestore = {0};

/* --- Snapshot list cache (TTL-based, per repo) --- */

#define SNAPSHOT_CACHE_TTL_MS 300000  /* 5 minutes */

typedef struct {
    char repoName[MAX_REPO_NAME];
    ResticSnapshot* snapshots;
    int count;
    ULONGLONG fetchTimeMs;
} SnapshotCache;

static SnapshotCache g_SnapCache[MAX_REPOS];
static int g_SnapCacheCount = 0;

/* Deep-copy a snapshot array. Caller must free the returned pointer. */
static ResticSnapshot* CopySnapshots(const ResticSnapshot* src, int count) {
    ResticSnapshot* copy;
    if (count <= 0 || !src) return NULL;
    copy = (ResticSnapshot*)malloc(sizeof(ResticSnapshot) * count);
    if (copy) memcpy(copy, src, sizeof(ResticSnapshot) * count);
    return copy;
}

/* Invalidate snapshot cache for a specific repo (e.g. on password change). */
static void InvalidateSnapshotCache(const char* repoName) {
    int i;
    for (i = 0; i < g_SnapCacheCount; i++) {
        if (strcmp(g_SnapCache[i].repoName, repoName) == 0) {
            free(g_SnapCache[i].snapshots);
            g_SnapCache[i].snapshots = NULL;
            /* Move last entry into this slot */
            g_SnapCacheCount--;
            if (i < g_SnapCacheCount) {
                g_SnapCache[i] = g_SnapCache[g_SnapCacheCount];
            }
            return;
        }
    }
}

/* --- Directory listing cache (immutable, keyed on shortId+path) --- */

#define LS_CACHE_MAX 32

typedef struct {
    char shortId[16];
    char path[MAX_PATH];
    DirEntry* entries;
    int count;
} LsCacheEntry;

static LsCacheEntry g_LsCache[LS_CACHE_MAX];
static int g_LsCacheCount = 0;

/* Deep-copy a DirEntry array. Caller must free the returned pointer. */
static DirEntry* CopyDirEntries(const DirEntry* src, int count) {
    DirEntry* copy;
    if (count <= 0 || !src) return NULL;
    copy = (DirEntry*)malloc(sizeof(DirEntry) * count);
    if (copy) memcpy(copy, src, sizeof(DirEntry) * count);
    return copy;
}

/* Helper: add an entry to a dynamic array. Grows the array as needed. */
static void AddEntry(DirEntry** entries, int* count, int* capacity,
                     const char* name, BOOL isDir,
                     DWORD sizeLow, DWORD sizeHigh, FILETIME ft) {
    if (*count >= *capacity) {
        *capacity = (*capacity == 0) ? 8 : (*capacity * 2);
        *entries = (DirEntry*)realloc(*entries, sizeof(DirEntry) * (*capacity));
        if (!*entries) return;
    }
    DirEntry* e = &(*entries)[*count];
    strncpy(e->name, name, MAX_PATH - 1);
    e->name[MAX_PATH - 1] = '\0';
    e->isDirectory = isDir;
    e->fileSizeLow = sizeLow;
    e->fileSizeHigh = sizeHigh;
    e->lastWriteTime = ft;
    (*count)++;
}

/* Parse path into segments.
   path: e.g. "\\RepoName\\snapshots"
   seg1, seg2, seg3: output buffers (MAX_PATH each), filled with segments or empty string.
   Returns number of segments (0 for root "\\"). */
static int ParsePathSegments(const char* path, char* seg1, char* seg2, char* seg3, char* rest) {
    const char* p;
    int segCount = 0;

    seg1[0] = '\0';
    seg2[0] = '\0';
    seg3[0] = '\0';
    rest[0] = '\0';

    if (!path || strcmp(path, "\\") == 0) return 0;

    p = path;
    if (*p == '\\') p++;

    /* Segment 1 */
    {
        const char* end = strchr(p, '\\');
        if (!end) end = p + strlen(p);
        size_t len = end - p;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(seg1, p, len);
        seg1[len] = '\0';
        segCount = 1;
        p = end;
    }

    if (*p == '\\') p++;
    if (*p == '\0') return segCount;

    /* Segment 2 */
    {
        const char* end = strchr(p, '\\');
        if (!end) end = p + strlen(p);
        size_t len = end - p;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(seg2, p, len);
        seg2[len] = '\0';
        segCount = 2;
        p = end;
    }

    if (*p == '\\') p++;
    if (*p == '\0') return segCount;

    /* Segment 3 */
    {
        const char* end = strchr(p, '\\');
        if (!end) end = p + strlen(p);
        size_t len = end - p;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(seg3, p, len);
        seg3[len] = '\0';
        segCount = 3;
        p = end;
    }

    /* Rest: everything after segment 3 */
    if (*p == '\\') p++;
    if (*p != '\0') {
        strncpy(rest, p, MAX_PATH - 1);
        rest[MAX_PATH - 1] = '\0';
    }

    return segCount;
}

/* Sanitize a backup path for use as a folder name.
   Replaces \ / : with _, strips leading/trailing _. */
static void SanitizePath(const char* raw, char* out, int maxLen) {
    int i, len, start, end;

    len = (int)strlen(raw);
    if (len >= maxLen) len = maxLen - 1;

    for (i = 0; i < len; i++) {
        char c = raw[i];
        if (c == '\\' || c == '/' || c == ':') {
            out[i] = '_';
        } else {
            out[i] = c;
        }
    }
    out[len] = '\0';

    /* Strip leading underscores */
    start = 0;
    while (out[start] == '_') start++;

    /* Strip trailing underscores */
    end = (int)strlen(out) - 1;
    while (end >= start && out[end] == '_') end--;

    if (start > 0 || end < (int)strlen(out) - 1) {
        int newLen = end - start + 1;
        if (newLen <= 0) {
            out[0] = '_';  /* fallback for empty result */
            out[1] = '\0';
        } else {
            memmove(out, out + start, newLen);
            out[newLen] = '\0';
        }
    }
}

/* Fetch and parse all snapshots for a repo. Returns count, caller frees *outSnapshots.
   Uses TTL-based cache to avoid repeated restic calls. */
static int FetchSnapshots(RepoConfig* repo, ResticSnapshot** outSnapshots) {
    char* output;
    DWORD exitCode;
    int numSnaps, i;
    ULONGLONG now;

    *outSnapshots = NULL;

    /* Check snapshot cache */
    now = GetTickCount64();
    for (i = 0; i < g_SnapCacheCount; i++) {
        if (strcmp(g_SnapCache[i].repoName, repo->name) == 0) {
            if (now - g_SnapCache[i].fetchTimeMs < SNAPSHOT_CACHE_TTL_MS) {
                /* Cache hit — return deep copy */
                *outSnapshots = CopySnapshots(g_SnapCache[i].snapshots, g_SnapCache[i].count);
                return (*outSnapshots) ? g_SnapCache[i].count : 0;
            }
            /* Cache expired — remove it */
            free(g_SnapCache[i].snapshots);
            g_SnapCacheCount--;
            if (i < g_SnapCacheCount)
                g_SnapCache[i] = g_SnapCache[g_SnapCacheCount];
            break;
        }
    }

    /* Cache miss — fetch from restic */
    output = RunRestic(repo->path, repo->password, "snapshots --json", &exitCode);
    if (!output) {
        if (g_LogProc)
            g_LogProc(g_PluginNr, MSGTYPE_IMPORTANTERROR,
                      "Error: Could not run restic. Is restic.exe in PATH?");
        return 0;
    }
    if (exitCode != 0) {
        if (g_RequestProc) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Failed to load snapshots. Check password and repository path.\n\n%.*s",
                     256, output);
            g_RequestProc(g_PluginNr, RT_MsgOK, "Restic Error", msg, output, MAX_PATH);
        }
        /* Invalidate cached password so user is re-prompted next time */
        repo->hasPassword = FALSE;
        repo->password[0] = '\0';
        InvalidateSnapshotCache(repo->name);
        free(output);
        return 0;
    }

    numSnaps = ParseSnapshots(output, outSnapshots);
    free(output);
    if (numSnaps <= 0) return 0;

    /* Store in cache */
    if (g_SnapCacheCount < MAX_REPOS) {
        SnapshotCache* sc = &g_SnapCache[g_SnapCacheCount];
        strncpy(sc->repoName, repo->name, MAX_REPO_NAME - 1);
        sc->repoName[MAX_REPO_NAME - 1] = '\0';
        sc->snapshots = CopySnapshots(*outSnapshots, numSnaps);
        sc->count = numSnaps;
        sc->fetchTimeMs = now;
        if (sc->snapshots) g_SnapCacheCount++;
    }

    /* Purge persistent cache for deleted snapshots */
    if (numSnaps > 0) {
        const char* validIds[256];
        int validCount = (numSnaps < 256) ? numSnaps : 256;
        for (i = 0; i < validCount; i++) {
            validIds[i] = (*outSnapshots)[i].shortId;
        }
        LsCache_Purge(repo->name, validIds, validCount);
    }

    return numSnaps;
}

/* List unique backup paths from all snapshots as folder entries */
static DirEntry* GetPathEntries(RepoConfig* repo, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0, capacity = 0;
    ResticSnapshot* snapshots = NULL;
    int numSnaps, i, j, k;
    FILETIME ftNow;
    /* Track unique sanitized paths */
    char (*seen)[MAX_PATH] = NULL;
    int seenCount = 0;

    numSnaps = FetchSnapshots(repo, &snapshots);
    if (numSnaps == 0) {
        *outCount = 0;
        return NULL;
    }

    GetSystemTimeAsFileTime(&ftNow);

    /* Allocate worst-case seen array */
    seen = (char(*)[MAX_PATH])calloc(numSnaps * MAX_SNAP_PATHS, MAX_PATH);
    if (!seen) {
        free(snapshots);
        *outCount = 0;
        return NULL;
    }

    for (i = 0; i < numSnaps; i++) {
        for (j = 0; j < snapshots[i].pathCount; j++) {
            char sanitized[MAX_PATH];
            BOOL duplicate = FALSE;

            SanitizePath(snapshots[i].paths[j], sanitized, MAX_PATH);

            /* Check for duplicate */
            for (k = 0; k < seenCount; k++) {
                if (strcmp(seen[k], sanitized) == 0) {
                    duplicate = TRUE;
                    break;
                }
            }

            if (!duplicate) {
                strncpy(seen[seenCount], sanitized, MAX_PATH - 1);
                seenCount++;
                AddEntry(&entries, &count, &capacity, sanitized, TRUE, 0, 0, ftNow);
            }
        }
    }

    free(seen);
    free(snapshots);
    *outCount = count;
    return entries;
}

/* List snapshots that match a given sanitized path */
static DirEntry* GetSnapshotsForPath(RepoConfig* repo, const char* sanitizedPath, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0, capacity = 0;
    ResticSnapshot* snapshots = NULL;
    int numSnaps, i, j;

    numSnaps = FetchSnapshots(repo, &snapshots);
    if (numSnaps == 0) {
        *outCount = 0;
        return NULL;
    }

    /* Insert [All Files] virtual entry at the top */
    {
        FILETIME ftNow;
        GetSystemTimeAsFileTime(&ftNow);
        AddEntry(&entries, &count, &capacity, ALL_FILES_ENTRY, TRUE, 0, 0, ftNow);
    }

    for (i = 0; i < numSnaps; i++) {
        BOOL matches = FALSE;

        for (j = 0; j < snapshots[i].pathCount; j++) {
            char sanitized[MAX_PATH];
            SanitizePath(snapshots[i].paths[j], sanitized, MAX_PATH);
            if (strcmp(sanitized, sanitizedPath) == 0) {
                matches = TRUE;
                break;
            }
        }

        if (matches) {
            char displayName[MAX_PATH];
            int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;

            sscanf(snapshots[i].time, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc);
            snprintf(displayName, sizeof(displayName), "%04d-%02d-%02d %02d-%02d-%02d (%s)",
                     yr, mo, dy, hr, mn, sc, snapshots[i].shortId);

            FILETIME ft = ParseISOTime(snapshots[i].time);
            AddEntry(&entries, &count, &capacity, displayName, TRUE, 0, 0, ft);
        }
    }

    free(snapshots);
    *outCount = count;
    return entries;
}

/* Extract the short snapshot ID from a display name like "2025-01-28 10-30-05 (196bc576)". */
static BOOL ExtractShortId(const char* displayName, char* shortId, int maxLen) {
    const char* open = strrchr(displayName, '(');
    const char* close = strrchr(displayName, ')');
    int len;

    if (!open || !close || close <= open + 1) return FALSE;

    open++; /* skip '(' */
    len = (int)(close - open);
    if (len >= maxLen) len = maxLen - 1;

    memcpy(shortId, open, len);
    shortId[len] = '\0';
    return TRUE;
}

/* Find the original backup path that matches a sanitized name. */
static BOOL FindOriginalPath(RepoConfig* repo, const char* sanitizedName, char* originalPath) {
    ResticSnapshot* snapshots = NULL;
    int numSnaps, i, j;

    numSnaps = FetchSnapshots(repo, &snapshots);
    if (numSnaps == 0) return FALSE;

    for (i = 0; i < numSnaps; i++) {
        for (j = 0; j < snapshots[i].pathCount; j++) {
            char sanitized[MAX_PATH];
            SanitizePath(snapshots[i].paths[j], sanitized, MAX_PATH);
            if (strcmp(sanitized, sanitizedName) == 0) {
                strncpy(originalPath, snapshots[i].paths[j], MAX_PATH - 1);
                originalPath[MAX_PATH - 1] = '\0';
                free(snapshots);
                return TRUE;
            }
        }
    }

    free(snapshots);
    return FALSE;
}

/* Convert backslashes to forward slashes in-place. */
static void BackslashToForwardSlash(char* path) {
    char* p;
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* Check if a segment is the [All Files] virtual entry. */
static BOOL IsAllFilesPath(const char* seg) {
    return (strcmp(seg, ALL_FILES_ENTRY) == 0);
}

/* Check if a name starts with the version prefix "[v] ". */
static BOOL HasVersionPrefix(const char* name) {
    return (strncmp(name, VERSION_PREFIX, VERSION_PREFIX_LEN) == 0);
}

/* Return the name without the "[v] " prefix, or the original name if no prefix. */
static const char* StripVersionPrefix(const char* name) {
    if (HasVersionPrefix(name)) return name + VERSION_PREFIX_LEN;
    return name;
}

/* Find a path component starting with "[v] " in a rest string (backslash-separated).
   Returns pointer to the "[v] " within rest, or NULL if not found. */
static const char* FindVersionComponent(const char* rest) {
    const char* p = rest;
    if (!p || !*p) return NULL;

    /* Check if rest itself starts with [v] */
    if (HasVersionPrefix(p)) return p;

    /* Scan for \[v] */
    while ((p = strstr(p, "\\" VERSION_PREFIX)) != NULL) {
        return p + 1; /* skip the backslash, return pointer to "[v] " */
    }
    return NULL;
}

/* Split rest at the [v] component.
   rest = "subdir\[v] photo.jpg\2025-01-28 10-30-05 (fb4ed15b)"
   → pathBefore="subdir", vFileName="photo.jpg", afterVersion="2025-01-28 10-30-05 (fb4ed15b)"

   rest = "[v] photo.jpg\2025-01-28..."
   → pathBefore="", vFileName="photo.jpg", afterVersion="2025-01-28..."

   rest = "[v] photo.jpg"
   → pathBefore="", vFileName="photo.jpg", afterVersion="" */
static void SplitAtVersionComponent(const char* rest, char* pathBefore, char* vFileName, char* afterVersion) {
    const char* vComp;
    const char* afterPrefix;
    const char* nextSep;

    pathBefore[0] = '\0';
    vFileName[0] = '\0';
    afterVersion[0] = '\0';

    vComp = FindVersionComponent(rest);
    if (!vComp) return;

    /* pathBefore = everything before the [v] component */
    if (vComp > rest) {
        /* There's a backslash before [v], so pathBefore = rest up to that backslash */
        int len = (int)(vComp - rest - 1); /* -1 to skip the separating backslash */
        if (len < 0) len = 0;
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(pathBefore, rest, len);
        pathBefore[len] = '\0';
    }

    /* Skip "[v] " prefix to get filename */
    afterPrefix = vComp + VERSION_PREFIX_LEN;

    /* Find end of filename (next backslash or end of string) */
    nextSep = strchr(afterPrefix, '\\');
    if (nextSep) {
        int len = (int)(nextSep - afterPrefix);
        if (len >= MAX_PATH) len = MAX_PATH - 1;
        memcpy(vFileName, afterPrefix, len);
        vFileName[len] = '\0';

        /* afterVersion = everything after the backslash */
        strncpy(afterVersion, nextSep + 1, MAX_PATH - 1);
        afterVersion[MAX_PATH - 1] = '\0';
    } else {
        strncpy(vFileName, afterPrefix, MAX_PATH - 1);
        vFileName[MAX_PATH - 1] = '\0';
    }
}

/* Convert a Windows drive path to restic's internal format.
   e.g. "d:\Fotky\Mix" -> "/d/Fotky/Mix"
        "C:\Users"      -> "/C/Users"
   If the path doesn't start with a drive letter, just normalize slashes. */
static void ToResticInternalPath(const char* winPath, char* outPath, int maxLen) {
    /* Check for drive letter pattern: "X:" or "X:\" */
    if (winPath[0] != '\0' && winPath[1] == ':' &&
        ((winPath[0] >= 'A' && winPath[0] <= 'Z') || (winPath[0] >= 'a' && winPath[0] <= 'z'))) {
        /* Convert "D:\Fotky\Mix" -> "/D/Fotky/Mix" */
        snprintf(outPath, maxLen, "/%c%s", winPath[0], winPath + 2);
    } else {
        strncpy(outPath, winPath, maxLen - 1);
        outPath[maxLen - 1] = '\0';
    }
    BackslashToForwardSlash(outPath);
}

/* Build the restic ls subpath from original backup path + TC subpath remainder.
   Converts to restic's internal path format: /d/Fotky/Mix */
static void BuildLsSubpath(const char* originalBackupPath, const char* rest, char* outPath, int maxLen) {
    char temp[MAX_PATH];

    if (rest[0] != '\0') {
        snprintf(temp, sizeof(temp), "%s/%s", originalBackupPath, rest);
    } else {
        strncpy(temp, originalBackupPath, MAX_PATH - 1);
        temp[MAX_PATH - 1] = '\0';
    }
    ToResticInternalPath(temp, outPath, maxLen);
}

/* List directory contents inside a snapshot. Uses cache for repeat visits. */
static DirEntry* GetSnapshotContents(RepoConfig* repo, const char* sanitizedPath,
                                      const char* snapshotDisplayName, const char* subpath,
                                      int* outCount) {
    DirEntry* entries = NULL;
    int count = 0, capacity = 0;
    char shortId[16];
    char originalPath[MAX_PATH];
    char lsSubpath[MAX_PATH];
    char args[MAX_PATH * 2];
    char* output;
    DWORD exitCode;
    ResticLsEntry* lsEntries = NULL;
    int numLsEntries, i;

    *outCount = 0;

    if (!ExtractShortId(snapshotDisplayName, shortId, sizeof(shortId))) {
        return NULL;
    }

    if (!FindOriginalPath(repo, sanitizedPath, originalPath)) {
        return NULL;
    }

    BuildLsSubpath(originalPath, subpath, lsSubpath, MAX_PATH);

    /* Convert ANSI path to UTF-8 for restic command and path comparisons */
    char lsSubpathUtf8[MAX_PATH];
    AnsiToUtf8(lsSubpath, lsSubpathUtf8, MAX_PATH);

    /* Check in-memory directory listing cache (keyed on UTF-8 path) */
    for (i = 0; i < g_LsCacheCount; i++) {
        if (strcmp(g_LsCache[i].shortId, shortId) == 0 &&
            strcmp(g_LsCache[i].path, lsSubpathUtf8) == 0) {
            /* Cache hit — return deep copy */
            *outCount = g_LsCache[i].count;
            return CopyDirEntries(g_LsCache[i].entries, g_LsCache[i].count);
        }
    }

    /* Check persistent SQLite cache */
    {
        int dbCount = 0;
        DirEntry* dbEntries = LsCache_Lookup(repo->name, shortId, lsSubpathUtf8, &dbCount);
        if (dbEntries && dbCount > 0) {
            /* Populate in-memory cache from persistent hit */
            LsCacheEntry* lce;
            if (g_LsCacheCount >= LS_CACHE_MAX) {
                free(g_LsCache[0].entries);
                memmove(&g_LsCache[0], &g_LsCache[1],
                        sizeof(LsCacheEntry) * (LS_CACHE_MAX - 1));
                g_LsCacheCount--;
            }
            lce = &g_LsCache[g_LsCacheCount];
            strncpy(lce->shortId, shortId, sizeof(lce->shortId) - 1);
            lce->shortId[sizeof(lce->shortId) - 1] = '\0';
            strncpy(lce->path, lsSubpathUtf8, MAX_PATH - 1);
            lce->path[MAX_PATH - 1] = '\0';
            lce->entries = CopyDirEntries(dbEntries, dbCount);
            lce->count = dbCount;
            if (lce->entries) g_LsCacheCount++;

            *outCount = dbCount;
            return dbEntries;
        }
    }

    /* Cache miss — fetch from restic (pass UTF-8 path) */
    snprintf(args, sizeof(args), "ls --json %s \"%s\"", shortId, lsSubpathUtf8);

    output = RunRestic(repo->path, repo->password, args, &exitCode);
    if (!output) {
        if (g_LogProc)
            g_LogProc(g_PluginNr, MSGTYPE_IMPORTANTERROR,
                      "Error: Could not run restic. Is restic.exe in PATH?");
        return NULL;
    }
    if (exitCode != 0) {
        if (g_LogProc)
            g_LogProc(g_PluginNr, MSGTYPE_IMPORTANTERROR,
                      "Error: restic ls failed. Check repository and snapshot.");
        free(output);
        return NULL;
    }

    /* Pass UTF-8 parentPath so it matches raw UTF-8 paths from restic JSON */
    numLsEntries = ParseLsOutput(output, lsSubpathUtf8, &lsEntries);
    free(output);

    if (numLsEntries <= 0) {
        free(lsEntries);
        *outCount = 0;
        return NULL;
    }

    for (i = 0; i < numLsEntries; i++) {
        BOOL isDir = (strcmp(lsEntries[i].type, "dir") == 0);
        FILETIME ft = ParseISOTime(lsEntries[i].mtime);
        AddEntry(&entries, &count, &capacity,
                 lsEntries[i].name, isDir,
                 lsEntries[i].sizeLow, lsEntries[i].sizeHigh, ft);
    }

    free(lsEntries);
    *outCount = count;

    /* Store in in-memory directory listing cache */
    if (entries && count > 0) {
        LsCacheEntry* lce;
        if (g_LsCacheCount >= LS_CACHE_MAX) {
            /* Evict oldest entry (index 0) */
            free(g_LsCache[0].entries);
            memmove(&g_LsCache[0], &g_LsCache[1],
                    sizeof(LsCacheEntry) * (LS_CACHE_MAX - 1));
            g_LsCacheCount--;
        }
        lce = &g_LsCache[g_LsCacheCount];
        strncpy(lce->shortId, shortId, sizeof(lce->shortId) - 1);
        lce->shortId[sizeof(lce->shortId) - 1] = '\0';
        strncpy(lce->path, lsSubpathUtf8, MAX_PATH - 1);
        lce->path[MAX_PATH - 1] = '\0';
        lce->entries = CopyDirEntries(entries, count);
        lce->count = count;
        if (lce->entries) g_LsCacheCount++;

        /* Persist to SQLite */
        LsCache_Store(repo->name, shortId, lsSubpathUtf8, entries, count);
    }

    return entries;
}

/* Merge directory contents from all snapshots matching a sanitized path.
   Directories are listed as-is; files get a "[v] " prefix and isDirectory=TRUE
   so the user can Enter them to see version listings. */
static DirEntry* GetAllFilesContents(RepoConfig* repo, const char* sanitizedPath,
                                      const char* subpath, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0, capacity = 0;
    ResticSnapshot* snapshots = NULL;
    int numSnaps, i, j, k;

    *outCount = 0;

    numSnaps = FetchSnapshots(repo, &snapshots);
    if (numSnaps == 0) return NULL;

    for (i = 0; i < numSnaps; i++) {
        BOOL matches = FALSE;

        for (j = 0; j < snapshots[i].pathCount; j++) {
            char sanitized[MAX_PATH];
            SanitizePath(snapshots[i].paths[j], sanitized, MAX_PATH);
            if (strcmp(sanitized, sanitizedPath) == 0) {
                matches = TRUE;
                break;
            }
        }

        if (!matches) continue;

        /* Build display name for this snapshot (needed by GetSnapshotContents) */
        char displayName[MAX_PATH];
        int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
        sscanf(snapshots[i].time, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc);
        snprintf(displayName, sizeof(displayName), "%04d-%02d-%02d %02d-%02d-%02d (%s)",
                 yr, mo, dy, hr, mn, sc, snapshots[i].shortId);

        /* Get contents of this snapshot at the subpath */
        int snapCount = 0;
        DirEntry* snapEntries = GetSnapshotContents(repo, sanitizedPath, displayName, subpath, &snapCount);
        if (!snapEntries || snapCount == 0) {
            free(snapEntries);
            continue;
        }

        /* Merge into result, deduplicating by name */
        for (k = 0; k < snapCount; k++) {
            BOOL duplicate = FALSE;
            int m;
            const char* baseName = snapEntries[k].name;

            /* Check if already in merged result */
            for (m = 0; m < count; m++) {
                const char* existingBase = StripVersionPrefix(entries[m].name);
                if (strcmp(existingBase, baseName) == 0) {
                    duplicate = TRUE;
                    break;
                }
            }

            if (!duplicate) {
                if (snapEntries[k].isDirectory) {
                    /* Directories: add as-is */
                    AddEntry(&entries, &count, &capacity,
                             baseName, TRUE, 0, 0, snapEntries[k].lastWriteTime);
                } else {
                    /* Files: add with [v] prefix, mark as directory so TC allows Enter */
                    char prefixedName[MAX_PATH];
                    snprintf(prefixedName, MAX_PATH, "%s%s", VERSION_PREFIX, baseName);
                    AddEntry(&entries, &count, &capacity,
                             prefixedName, TRUE,
                             snapEntries[k].fileSizeLow, snapEntries[k].fileSizeHigh,
                             snapEntries[k].lastWriteTime);
                }
            }
        }

        free(snapEntries);
    }

    free(snapshots);
    *outCount = count;
    return entries;
}

/* List all versions of a specific file across snapshots.
   Uses `restic find --json` to locate the file in all snapshots. */
static DirEntry* GetFileVersions(RepoConfig* repo, const char* sanitizedPath,
                                  const char* filePath, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0, capacity = 0;
    char originalPath[MAX_PATH];
    char resticPath[MAX_PATH];
    char resticPathUtf8[MAX_PATH];
    char args[MAX_PATH * 3];
    char* output;
    DWORD exitCode;
    ResticFindEntry* findEntries = NULL;
    int numFound, i;

    *outCount = 0;

    if (!FindOriginalPath(repo, sanitizedPath, originalPath))
        return NULL;

    /* Build the full restic path for the file */
    BuildLsSubpath(originalPath, filePath, resticPath, MAX_PATH);
    AnsiToUtf8(resticPath, resticPathUtf8, MAX_PATH);

    /* Build the --path filter using the original backup path (as stored in snapshot) */
    char originalPathUtf8[MAX_PATH];
    AnsiToUtf8(originalPath, originalPathUtf8, MAX_PATH);

    /* Run restic find */
    snprintf(args, sizeof(args), "find --json --path \"%s\" \"%s\"",
             originalPathUtf8, resticPathUtf8);

    output = RunRestic(repo->path, repo->password, args, &exitCode);

    if (!output) return NULL;
    if (exitCode != 0) {
        free(output);
        return NULL;
    }

    numFound = ParseFindOutput(output, &findEntries);
    free(output);

    if (numFound <= 0) {
        free(findEntries);
        return NULL;
    }

    for (i = 0; i < numFound; i++) {
        char displayName[MAX_PATH];
        int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;
        FILETIME ft;

        sscanf(findEntries[i].mtime, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc);

        /* Extract original filename from the end of the path */
        const char* origName = strrchr(findEntries[i].path, '/');
        if (!origName) origName = strrchr(findEntries[i].path, '\\');
        origName = origName ? origName + 1 : findEntries[i].path;

        snprintf(displayName, sizeof(displayName), "%04d-%02d-%02d %02d-%02d-%02d (%s) %s",
                 yr, mo, dy, hr, mn, sc, findEntries[i].shortId, origName);

        ft = ParseISOTime(findEntries[i].mtime);
        AddEntry(&entries, &count, &capacity,
                 displayName, FALSE,
                 findEntries[i].sizeLow, findEntries[i].sizeHigh, ft);
    }

    free(findEntries);
    *outCount = count;
    return entries;
}

/* Returns heap-allocated directory entries for the given path. */
DirEntry* GetEntriesForPath(const char* path, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0;
    int capacity = 0;
    char seg1[MAX_PATH], seg2[MAX_PATH], seg3[MAX_PATH], rest[MAX_PATH];
    int numSegs;
    FILETIME ftNow;

    *outCount = 0;
    numSegs = ParsePathSegments(path, seg1, seg2, seg3, rest);

    /* Get a reasonable "now" timestamp for virtual entries */
    GetSystemTimeAsFileTime(&ftNow);

    if (numSegs == 0) {
        /* Root: list configured repos + [Add Repository] */
        int i;
        for (i = 0; i < g_RepoStore.count; i++) {
            if (g_RepoStore.repos[i].configured) {
                AddEntry(&entries, &count, &capacity,
                         g_RepoStore.repos[i].name, TRUE, 0, 0, ftNow);
            }
        }
        AddEntry(&entries, &count, &capacity,
                 "[Add Repository]", TRUE, 0, 0, ftNow);
    }
    else if (numSegs == 1 && strcmp(seg1, "[Add Repository]") == 0) {
        /* Trigger add-repo dialog */
        if (RepoStore_PromptAdd(g_PluginNr, g_RequestProc)) {
            /* Repo added — return a hint entry so TC doesn't show error.
               User will navigate back to root to see the new repo. */
            AddEntry(&entries, &count, &capacity,
                     "Repository added - go back to see it", FALSE, 0, 0, ftNow);
        }
        /* If cancelled, return empty → TC shows empty folder / goes back */
    }
    else if (numSegs == 1) {
        /* Inside a repo: show unique backup paths as folders */
        RepoConfig* repo = RepoStore_FindByName(seg1);
        if (repo && RepoStore_EnsurePassword(repo, g_PluginNr, g_RequestProc)) {
            entries = GetPathEntries(repo, &count);
        }
    }
    else if (numSegs == 2) {
        /* Inside a backup path: show matching snapshots */
        RepoConfig* repo = RepoStore_FindByName(seg1);
        if (repo && RepoStore_EnsurePassword(repo, g_PluginNr, g_RequestProc)) {
            entries = GetSnapshotsForPath(repo, seg2, &count);
        }
    }
    else if (numSegs == 3) {
        RepoConfig* repo = RepoStore_FindByName(seg1);
        if (repo && RepoStore_EnsurePassword(repo, g_PluginNr, g_RequestProc)) {
            if (IsAllFilesPath(seg3)) {
                const char* vComp = FindVersionComponent(rest);
                if (vComp) {
                    char pathBefore[MAX_PATH], vFileName[MAX_PATH], afterV[MAX_PATH];
                    SplitAtVersionComponent(rest, pathBefore, vFileName, afterV);
                    if (afterV[0] == '\0') {
                        /* Entered [v] file → show version listing */
                        char filePath[MAX_PATH];
                        if (pathBefore[0])
                            snprintf(filePath, MAX_PATH, "%s\\%s", pathBefore, vFileName);
                        else
                            strncpy(filePath, vFileName, MAX_PATH - 1);
                        filePath[MAX_PATH - 1] = '\0';
                        entries = GetFileVersions(repo, seg2, filePath, &count);
                    }
                    /* else: afterV is set = specific version file, TC shouldn't list it */
                } else {
                    /* Pure merged directory browsing */
                    entries = GetAllFilesContents(repo, seg2, rest, &count);
                }
            } else {
                /* Normal snapshot browsing */
                entries = GetSnapshotContents(repo, seg2, seg3, rest, &count);
            }
        }
    }

    *outCount = count;
    return entries;
}

/* Fill WIN32_FIND_DATAA from a DirEntry */
static void FillFindData(WIN32_FIND_DATAA* fd, const DirEntry* entry) {
    memset(fd, 0, sizeof(WIN32_FIND_DATAA));

    if (entry->isDirectory) {
        fd->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    }

    fd->ftLastWriteTime = entry->lastWriteTime;
    fd->nFileSizeLow = entry->fileSizeLow;
    fd->nFileSizeHigh = entry->fileSizeHigh;

    strncpy(fd->cFileName, entry->name, MAX_PATH - 1);
    fd->cFileName[MAX_PATH - 1] = '\0';
}

/* --- Exported WFX functions --- */

int __stdcall FsInit(int PluginNr, tProgressProc pProgressProc,
                     tLogProc pLogProc, tRequestProc pRequestProc) {
    g_PluginNr = PluginNr;
    g_ProgressProc = pProgressProc;
    g_LogProc = pLogProc;
    g_RequestProc = pRequestProc;

    /* Load repo configuration */
    RepoStore_Load();

    /* Initialize persistent directory listing cache */
    LsCache_Init();

    return 0;
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    int count = 0;
    DirEntry* entries = GetEntriesForPath(Path, &count);

    if (!entries || count == 0) {
        free(entries);
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    SearchContext* ctx = (SearchContext*)malloc(sizeof(SearchContext));
    if (!ctx) {
        free(entries);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    strncpy(ctx->path, Path, MAX_PATH - 1);
    ctx->path[MAX_PATH - 1] = '\0';
    ctx->index = 1;
    ctx->count = count;
    ctx->entries = entries;

    FillFindData(FindData, &entries[0]);
    return (HANDLE)ctx;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    SearchContext* ctx = (SearchContext*)Hdl;
    if (!ctx || ctx->index >= ctx->count) return FALSE;

    FillFindData(FindData, &ctx->entries[ctx->index]);
    ctx->index++;
    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl) {
    if (Hdl && Hdl != INVALID_HANDLE_VALUE) {
        SearchContext* ctx = (SearchContext*)Hdl;
        free(ctx->entries);
        free(ctx);
    }
    return 0;
}

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    strncpy(DefRootName, "Restic Repositories", maxlen - 1);
    DefRootName[maxlen - 1] = '\0';
}

/* --- Batch restore helpers --- */

/* Recursively delete a directory and all its contents. */
static void DeleteDirectoryRecursive(const char* dirPath) {
    char searchPath[MAX_PATH];
    char filePath[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    snprintf(searchPath, MAX_PATH, "%s\\*", dirPath);
    hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        snprintf(filePath, MAX_PATH, "%s\\%s", dirPath, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeleteDirectoryRecursive(filePath);
        } else {
            DeleteFileA(filePath);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    RemoveDirectoryA(dirPath);
}

/* Build a local temp file path from the batch restore temp dir and a restic path.
   resticPath is like "/D/Fotky/Mix/subdir/file.jpg"
   Result: tempDir + resticPath with / → \ */
static void BuildBatchTempFilePath(const char* tempDir, const char* resticPath,
                                    char* outPath, int maxLen) {
    char converted[MAX_PATH];
    int i;

    strncpy(converted, resticPath, MAX_PATH - 1);
    converted[MAX_PATH - 1] = '\0';

    /* Convert forward slashes to backslashes */
    for (i = 0; converted[i]; i++) {
        if (converted[i] == '/') converted[i] = '\\';
    }

    /* Remove leading backslash if present */
    if (converted[0] == '\\')
        snprintf(outPath, maxLen, "%s%s", tempDir, converted);
    else
        snprintf(outPath, maxLen, "%s\\%s", tempDir, converted);
}

/* --- File operation helpers --- */

/* Resolved components of a remote file path */
typedef struct {
    RepoConfig* repo;
    char shortId[16];
    char resticPath[MAX_PATH];
} ResolvedPath;

/* Resolve a TC RemoteName into repo, snapshot ID, and restic internal file path.
   Returns TRUE on success. Requires numSegs==3 and non-empty rest (i.e. a file path). */
static BOOL ResolveRemotePath(const char* remoteName, ResolvedPath* out) {
    char seg1[MAX_PATH], seg2[MAX_PATH], seg3[MAX_PATH], rest[MAX_PATH];
    char originalPath[MAX_PATH];
    int numSegs;

    numSegs = ParsePathSegments(remoteName, seg1, seg2, seg3, rest);
    if (numSegs < 3 || rest[0] == '\0') return FALSE;

    out->repo = RepoStore_FindByName(seg1);
    if (!out->repo) return FALSE;

    if (!RepoStore_EnsurePassword(out->repo, g_PluginNr, g_RequestProc))
        return FALSE;

    if (IsAllFilesPath(seg3)) {
        /* [All Files] path: rest = "subdir\[v] photo.jpg\2025-01-28 10-30-05 (fb4ed15b)" */
        char pathBefore[MAX_PATH], vFileName[MAX_PATH], afterV[MAX_PATH];
        const char* vComp = FindVersionComponent(rest);
        if (!vComp) return FALSE;

        SplitAtVersionComponent(rest, pathBefore, vFileName, afterV);
        if (afterV[0] == '\0') return FALSE;  /* no version selected */

        /* afterV = "2025-01-28 10-30-05 (fb4ed15b)" → extract shortId */
        if (!ExtractShortId(afterV, out->shortId, sizeof(out->shortId)))
            return FALSE;

        if (!FindOriginalPath(out->repo, seg2, originalPath))
            return FALSE;

        /* Build file subpath from pathBefore + vFileName */
        char fileSubpath[MAX_PATH];
        if (pathBefore[0])
            snprintf(fileSubpath, MAX_PATH, "%s\\%s", pathBefore, vFileName);
        else {
            strncpy(fileSubpath, vFileName, MAX_PATH - 1);
            fileSubpath[MAX_PATH - 1] = '\0';
        }

        BuildLsSubpath(originalPath, fileSubpath, out->resticPath, MAX_PATH);

        char utf8Path[MAX_PATH];
        AnsiToUtf8(out->resticPath, utf8Path, MAX_PATH);
        strncpy(out->resticPath, utf8Path, MAX_PATH - 1);
        out->resticPath[MAX_PATH - 1] = '\0';
        return TRUE;
    }

    if (!ExtractShortId(seg3, out->shortId, sizeof(out->shortId)))
        return FALSE;

    if (!FindOriginalPath(out->repo, seg2, originalPath))
        return FALSE;

    BuildLsSubpath(originalPath, rest, out->resticPath, MAX_PATH);

    /* Convert ANSI path to UTF-8 for restic command */
    char utf8Path[MAX_PATH];
    AnsiToUtf8(out->resticPath, utf8Path, MAX_PATH);
    strncpy(out->resticPath, utf8Path, MAX_PATH - 1);
    out->resticPath[MAX_PATH - 1] = '\0';

    return TRUE;
}

/* Data passed through the dump progress callback */
typedef struct {
    char remoteName[MAX_PATH];
    char localName[MAX_PATH];
    BOOL aborted;
} ProgressUserData;

/* Progress callback bridging RunResticDump to TC's g_ProgressProc */
static BOOL DumpProgressCallback(LONGLONG bytesWritten, LONGLONG totalSize, void* userData) {
    ProgressUserData* pud = (ProgressUserData*)userData;
    int percent = 0;

    if (totalSize > 0) {
        percent = (int)((bytesWritten * 100) / totalSize);
        if (percent > 100) percent = 100;
    }

    /* g_ProgressProc returns 1 if user wants to abort */
    if (g_ProgressProc(g_PluginNr, pud->remoteName, pud->localName, percent)) {
        pud->aborted = TRUE;
        return FALSE;
    }
    return TRUE;
}

/* --- FsGetFile: copy file from snapshot to local filesystem (F5 in TC) --- */

int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags,
                        RemoteInfoStruct* ri) {
    ResolvedPath resolved;
    ProgressUserData pud;
    LONGLONG totalSize = 0;
    DWORD exitCode;
    BOOL ok;

    /* Resume not supported for restic dump */
    if ((CopyFlags & FS_COPYFLAGS_RESUME) && !(CopyFlags & FS_COPYFLAGS_OVERWRITE))
        return FS_FILE_NOTSUPPORTED;

    /* Check if destination exists and overwrite not requested */
    if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE)) {
        if (GetFileAttributesA(LocalName) != INVALID_FILE_ATTRIBUTES)
            return FS_FILE_EXISTS;
    }

    /* Resolve the remote path into repo + snapshot + restic path */
    if (!ResolveRemotePath(RemoteName, &resolved))
        return FS_FILE_NOTFOUND;

    /* Initial progress report */
    if (g_ProgressProc(g_PluginNr, RemoteName, LocalName, 0))
        return FS_FILE_USERABORT;

    /* Deferred batch restore: on first FsGetFile, derive the --include path
       from the actual file path and run restic restore now */
    if (g_BatchRestore.pending && !g_BatchRestore.active) {
        DWORD batchExitCode;
        /* Build include path: take the first subdirectory after the prefix.
           prefix = "/d/Martin/Mapy", resticPath = "/d/Martin/Mapy/Gpx/file.gpx"
           → includePath = "/d/Martin/Mapy/Gpx" */
        char includePath[MAX_PATH];
        size_t prefixLen = strlen(g_BatchRestore.resticPrefix);
        const char* afterPrefix = resolved.resticPath + prefixLen;

        /* Skip leading '/' after prefix */
        if (*afterPrefix == '/') afterPrefix++;

        /* Find the next '/' — that ends the subfolder name */
        {
            const char* nextSlash = strchr(afterPrefix, '/');
            if (nextSlash) {
                size_t includeLen = nextSlash - resolved.resticPath;
                if (includeLen >= MAX_PATH) includeLen = MAX_PATH - 1;
                memcpy(includePath, resolved.resticPath, includeLen);
                includePath[includeLen] = '\0';
            } else {
                /* File is directly in the prefix dir — use the full file path */
                strncpy(includePath, resolved.resticPath, MAX_PATH - 1);
                includePath[MAX_PATH - 1] = '\0';
            }
        }

        g_BatchRestore.pending = FALSE;

        {
            BOOL restoreOk = RunResticRestore(g_BatchRestore.repoPath,
                                 g_BatchRestore.password,
                                 g_BatchRestore.shortId, g_BatchRestore.snapshotPath,
                                 includePath, g_BatchRestore.tempDir,
                                 &batchExitCode);

            /* DEBUG: log restore result */
            {
                char debugPath[MAX_PATH];
                GetTempPathA(MAX_PATH, debugPath);
                strcat(debugPath, "restic_wfx_debug.log");
                FILE* dbg = fopen(debugPath, "a");
                if (dbg) {
                    fprintf(dbg, "DEFERRED: restoreOk=%d exitCode=%lu include=[%s] target=[%s]\n",
                            restoreOk, batchExitCode, includePath, g_BatchRestore.tempDir);
                    fclose(dbg);
                }
            }

            if (restoreOk && batchExitCode == 0) {
                g_BatchRestore.active = TRUE;
            }
        }
        /* On failure, active stays FALSE → per-file dump fallback */
    }

    /* Check if batch restore has this file pre-extracted.
       Use wide APIs because restic creates Unicode filenames and
       the temp path is built from UTF-8 resticPath. */
    if (g_BatchRestore.active) {
        char tempFileUtf8[MAX_PATH];
        WCHAR wTempFile[MAX_PATH];
        WCHAR wLocalName[MAX_PATH];

        BuildBatchTempFilePath(g_BatchRestore.tempDir, resolved.resticPath,
                               tempFileUtf8, MAX_PATH);

        /* Convert UTF-8 temp path to wide */
        MultiByteToWideChar(CP_UTF8, 0, tempFileUtf8, -1, wTempFile, MAX_PATH);
        /* Convert ANSI local path to wide */
        MultiByteToWideChar(CP_ACP, 0, LocalName, -1, wLocalName, MAX_PATH);

        if (GetFileAttributesW(wTempFile) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(wTempFile, wLocalName, FALSE)) {
                g_ProgressProc(g_PluginNr, RemoteName, LocalName, 100);
                return FS_FILE_OK;
            }
        }
        /* Fall through to per-file dump if temp file missing */
    }

    /* Get total size for progress reporting */
    if (ri) {
        totalSize = ((LONGLONG)ri->SizeHigh << 32) | ri->SizeLow;
    }

    /* Set up progress user data */
    strncpy(pud.remoteName, RemoteName, MAX_PATH - 1);
    pud.remoteName[MAX_PATH - 1] = '\0';
    strncpy(pud.localName, LocalName, MAX_PATH - 1);
    pud.localName[MAX_PATH - 1] = '\0';
    pud.aborted = FALSE;

    /* Run restic dump, streaming to local file */
    ok = RunResticDump(resolved.repo->path, resolved.repo->password,
                       resolved.shortId, resolved.resticPath,
                       LocalName, totalSize,
                       DumpProgressCallback, &pud, &exitCode);

    if (!ok) {
        if (pud.aborted) return FS_FILE_USERABORT;
        return FS_FILE_READERROR;
    }

    /* Final progress */
    g_ProgressProc(g_PluginNr, RemoteName, LocalName, 100);

    return FS_FILE_OK;
}

/* --- FsExecuteFile: open/view file from snapshot (Enter/double-click in TC) --- */

int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb) {
    ResolvedPath resolved;
    char tempDir[MAX_PATH];
    char tempFile[MAX_PATH];
    const char* fileName;
    DWORD exitCode;
    BOOL ok;

    /* Only handle "open" verb */
    if (strcmp(Verb, "open") != 0)
        return FS_EXEC_YOURSELF;

    /* Check if this is a file (ResolveRemotePath requires non-empty rest) */
    if (!ResolveRemotePath(RemoteName, &resolved))
        return FS_EXEC_YOURSELF;

    /* Extract filename from RemoteName (last component after backslash) */
    fileName = strrchr(RemoteName, '\\');
    if (fileName) fileName++;
    else fileName = RemoteName;

    /* Build temp directory: %TEMP%\restic_wfx\ */
    GetTempPathA(MAX_PATH, tempDir);
    PathAppendA(tempDir, "restic_wfx");
    CreateDirectoryA(tempDir, NULL);

    /* Build temp file path: %TEMP%\restic_wfx\<shortId>_<filename> */
    snprintf(tempFile, MAX_PATH, "%s\\%s_%s", tempDir, resolved.shortId, fileName);

    /* Skip extraction if temp file already exists (cache hit) */
    if (GetFileAttributesA(tempFile) == INVALID_FILE_ATTRIBUTES) {
        ok = RunResticDump(resolved.repo->path, resolved.repo->password,
                           resolved.shortId, resolved.resticPath,
                           tempFile, 0, NULL, NULL, &exitCode);
        if (!ok) return FS_EXEC_ERROR;
    }

    /* Open with default application */
    if ((INT_PTR)ShellExecuteA(MainWin, "open", tempFile,
                                NULL, NULL, SW_SHOWNORMAL) <= 32) {
        return FS_EXEC_ERROR;
    }

    return FS_EXEC_OK;
}

/* --- FsDisconnect: cleanup on plugin disconnect --- */

/* Delete all files in %TEMP%\restic_wfx\ and remove the directory. */
static void DeleteTempDir(void) {
    char tempDir[MAX_PATH];
    char searchPath[MAX_PATH];
    char filePath[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    GetTempPathA(MAX_PATH, tempDir);
    PathAppendA(tempDir, "restic_wfx");

    snprintf(searchPath, MAX_PATH, "%s\\*", tempDir);
    hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        snprintf(filePath, MAX_PATH, "%s\\%s", tempDir, fd.cFileName);
        DeleteFileA(filePath);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    RemoveDirectoryA(tempDir);
}

int __stdcall FsDisconnect(char* DisconnectRoot) {
    int i;

    /* Clean up any active batch restore */
    if (g_BatchRestore.active) {
        DeleteDirectoryRecursive(g_BatchRestore.tempDir);
        memset(&g_BatchRestore, 0, sizeof(g_BatchRestore));
    }

    /* Free snapshot cache */
    for (i = 0; i < g_SnapCacheCount; i++) {
        free(g_SnapCache[i].snapshots);
        g_SnapCache[i].snapshots = NULL;
    }
    g_SnapCacheCount = 0;

    /* Free directory listing cache */
    for (i = 0; i < g_LsCacheCount; i++) {
        free(g_LsCache[i].entries);
        g_LsCache[i].entries = NULL;
    }
    g_LsCacheCount = 0;

    /* Zero all passwords */
    for (i = 0; i < g_RepoStore.count; i++) {
        SecureZeroMemory(g_RepoStore.repos[i].password, MAX_REPO_PASS);
        g_RepoStore.repos[i].hasPassword = FALSE;
    }

    /* Shut down persistent directory listing cache */
    LsCache_Shutdown();

    /* Delete temp files */
    DeleteTempDir();

    return 0;
}

/* --- FsStatusInfo: batch restore optimization for multi-file copy --- */

void __stdcall FsStatusInfo(char* RemoteName, int InfoStartEnd, int InfoOperation) {
    if (InfoOperation == FS_STATUS_OP_GET_MULTI ||
        InfoOperation == FS_STATUS_OP_GET_MULTI_THREAD) {

        if (InfoStartEnd == FS_STATUS_START) {
            char seg1[MAX_PATH], seg2[MAX_PATH], seg3[MAX_PATH], rest[MAX_PATH];
            char originalPath[MAX_PATH];
            char resticPrefix[MAX_PATH];
            char resticPrefixUtf8[MAX_PATH];
            char tempDir[MAX_PATH];
            char restoreSub[MAX_PATH];
            char originalPathUtf8[MAX_PATH];
            int numSegs;
            RepoConfig* repo;
            char shortId[16];

            numSegs = ParsePathSegments(RemoteName, seg1, seg2, seg3, rest);
            if (numSegs < 3) return;

            /* Skip [All Files] paths — files come from different snapshots */
            if (IsAllFilesPath(seg3)) return;

            repo = RepoStore_FindByName(seg1);
            if (!repo) return;
            if (!RepoStore_EnsurePassword(repo, g_PluginNr, g_RequestProc)) return;

            if (!ExtractShortId(seg3, shortId, sizeof(shortId))) return;
            if (!FindOriginalPath(repo, seg2, originalPath)) return;

            /* Strip trailing backslash from rest (TC passes "Mapy\" not "Mapy") */
            {
                size_t rlen = strlen(rest);
                if (rlen > 0 && rest[rlen - 1] == '\\')
                    rest[rlen - 1] = '\0';
            }

            /* Build the restic internal path prefix for the current directory */
            BuildLsSubpath(originalPath, rest, resticPrefix, MAX_PATH);
            AnsiToUtf8(resticPrefix, resticPrefixUtf8, MAX_PATH);
            AnsiToUtf8(originalPath, originalPathUtf8, MAX_PATH);

            /* Create temp directory: %TEMP%\restic_wfx\restore_XXXXXXXX */
            GetTempPathA(MAX_PATH, tempDir);
            PathAppendA(tempDir, "restic_wfx");
            CreateDirectoryA(tempDir, NULL);

            snprintf(restoreSub, MAX_PATH, "%s\\restore_%s_%u",
                     tempDir, shortId, (unsigned)GetTickCount());
            CreateDirectoryA(restoreSub, NULL);

            /* Defer actual restore to first FsGetFile — we don't know
               the selected subfolder yet (TC only gives us the parent dir).
               Store everything needed so FsGetFile can run the restore. */
            g_BatchRestore.pending = TRUE;
            g_BatchRestore.active = FALSE;
            strncpy(g_BatchRestore.tempDir, restoreSub, MAX_PATH - 1);
            g_BatchRestore.tempDir[MAX_PATH - 1] = '\0';
            strncpy(g_BatchRestore.resticPrefix, resticPrefixUtf8, MAX_PATH - 1);
            g_BatchRestore.resticPrefix[MAX_PATH - 1] = '\0';
            strncpy(g_BatchRestore.repoPath, repo->path, sizeof(g_BatchRestore.repoPath) - 1);
            g_BatchRestore.repoPath[sizeof(g_BatchRestore.repoPath) - 1] = '\0';
            strncpy(g_BatchRestore.password, repo->password, sizeof(g_BatchRestore.password) - 1);
            g_BatchRestore.password[sizeof(g_BatchRestore.password) - 1] = '\0';
            strncpy(g_BatchRestore.snapshotPath, originalPathUtf8, MAX_PATH - 1);
            g_BatchRestore.snapshotPath[MAX_PATH - 1] = '\0';
            strncpy(g_BatchRestore.shortId, shortId, sizeof(g_BatchRestore.shortId) - 1);
            g_BatchRestore.shortId[sizeof(g_BatchRestore.shortId) - 1] = '\0';
        }
        else if (InfoStartEnd == FS_STATUS_END) {
            if (g_BatchRestore.active || g_BatchRestore.pending) {
                if (g_BatchRestore.tempDir[0])
                    DeleteDirectoryRecursive(g_BatchRestore.tempDir);
                SecureZeroMemory(g_BatchRestore.password, sizeof(g_BatchRestore.password));
                memset(&g_BatchRestore, 0, sizeof(g_BatchRestore));
            }
        }
    }
}
