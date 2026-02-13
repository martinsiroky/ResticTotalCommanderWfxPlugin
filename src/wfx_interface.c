/*
 * restic-wfx - Total Commander plugin for browsing restic backup repositories
 * Copyright (c) 2026 Martin Široký
 * SPDX-License-Identifier: MIT
 */

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
#include <wincrypt.h>

/* Module handle from plugin_main.c for finding DLL directory */
extern HMODULE g_hModule;

/* Generate a cryptographically secure random 32-bit value for temp dir names.
   Falls back to combining multiple entropy sources if CryptGenRandom fails. */
static DWORD GetSecureRandomValue(void) {
    DWORD randomValue = 0;
    HCRYPTPROV hProv;

    if (CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        CryptGenRandom(hProv, sizeof(randomValue), (BYTE*)&randomValue);
        CryptReleaseContext(hProv, 0);
        return randomValue;
    }

    /* Fallback: combine multiple entropy sources */
    {
        LARGE_INTEGER perfCounter;
        FILETIME ft;
        DWORD pid = GetCurrentProcessId();
        DWORD tid = GetCurrentThreadId();

        QueryPerformanceCounter(&perfCounter);
        GetSystemTimeAsFileTime(&ft);

        randomValue = (DWORD)perfCounter.LowPart;
        randomValue ^= (DWORD)perfCounter.HighPart;
        randomValue ^= ft.dwLowDateTime;
        randomValue ^= ft.dwHighDateTime;
        randomValue ^= pid;
        randomValue ^= tid << 16;
        randomValue ^= GetTickCount();
    }
    return randomValue;
}

/* [All Files] virtual snapshot constants */
#define ALL_FILES_ENTRY    "[All Files]"
#define VERSION_PREFIX     "[versions] "
#define VERSION_PREFIX_LEN 11

/* Get the path to README.txt next to the plugin DLL.
   Returns TRUE if the file exists, FALSE otherwise. */
static BOOL GetReadmePath(char* outPath, size_t maxLen) {
    char dllPath[MAX_PATH];
    char* lastSlash;

    if (!GetModuleFileNameA(g_hModule, dllPath, MAX_PATH)) {
        return FALSE;
    }

    lastSlash = strrchr(dllPath, '\\');
    if (!lastSlash) return FALSE;

    *lastSlash = '\0';
    snprintf(outPath, maxLen, "%s\\README.txt", dllPath);

    return (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES);
}

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
        AddEntry(&entries, &count, &capacity, "[Refresh snapshot list]", TRUE, 0, 0, ftNow);
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
/* Collapse consecutive forward slashes in-place: "//a///b" -> "/a/b" */
static void CollapseSlashes(char* path) {
    char* src = path;
    char* dst = path;
    BOOL lastWasSlash = FALSE;

    while (*src) {
        if (*src == '/') {
            if (!lastWasSlash) {
                *dst++ = *src;
            }
            lastWasSlash = TRUE;
        } else {
            *dst++ = *src;
            lastWasSlash = FALSE;
        }
        src++;
    }
    *dst = '\0';
}

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
    CollapseSlashes(outPath);

    /* Remove trailing slash (except for root "/") */
    {
        size_t len = strlen(outPath);
        if (len > 1 && outPath[len - 1] == '/') {
            outPath[len - 1] = '\0';
        }
    }
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

/* Extract parent directory from a UTF-8 forward-slash path.
   "/d/Fotky/Mix/subdir/file.txt" → "/d/Fotky/Mix/subdir"
   "/d/file.txt" → "/d"
   "/file.txt" → "/" */
static void GetParentPath(const char* path, char* parent, int maxLen) {
    const char* lastSlash = strrchr(path, '/');
    int len;
    if (!lastSlash || lastSlash == path) {
        strncpy(parent, "/", maxLen - 1);
        parent[maxLen - 1] = '\0';
        return;
    }
    len = (int)(lastSlash - path);
    if (len >= maxLen) len = maxLen - 1;
    memcpy(parent, path, len);
    parent[len] = '\0';
}

/* qsort comparator for ResticLsEntry by parent directory of the path field. */
static int CompareByParentPath(const void* a, const void* b) {
    char parentA[MAX_PATH], parentB[MAX_PATH];
    GetParentPath(((const ResticLsEntry*)a)->path, parentA, MAX_PATH);
    GetParentPath(((const ResticLsEntry*)b)->path, parentB, MAX_PATH);
    return strcmp(parentA, parentB);
}

/* Parse all entries from a restic ls call and bulk-cache every subdirectory
   into SQLite. Returns the direct children of requestedPathUtf8 via outDirectChildren. */
static void BulkCacheSubdirectories(
    const char* repoName, const char* shortId,
    const char* requestedPathUtf8,
    ResticLsEntry* allEntries, int allCount,
    DirEntry** outDirectChildren, int* outDirectCount)
{
    char** parentPathList = NULL;
    int numParents = 0;
    int i;

    *outDirectChildren = NULL;
    *outDirectCount = 0;

    if (allCount <= 0) return;

    /* Sort all entries by parent path */
    qsort(allEntries, allCount, sizeof(ResticLsEntry), CompareByParentPath);

    /* Allocate array to track unique parent paths (for empty dir detection) */
    parentPathList = (char**)malloc(sizeof(char*) * allCount);
    if (!parentPathList) return;

    /* Walk sorted array, grouping consecutive entries with same parent */
    i = 0;
    while (i < allCount) {
        char currentParent[MAX_PATH];
        int groupStart, groupCount, j;
        DirEntry* dirEntries;

        GetParentPath(allEntries[i].path, currentParent, MAX_PATH);
        groupStart = i;

        /* Find end of this group */
        while (i < allCount) {
            char thisParent[MAX_PATH];
            GetParentPath(allEntries[i].path, thisParent, MAX_PATH);
            if (strcmp(thisParent, currentParent) != 0) break;
            i++;
        }
        groupCount = i - groupStart;

        /* Record this parent path for empty dir detection */
        {
            char* dup = (char*)malloc(strlen(currentParent) + 1);
            if (dup) {
                strcpy(dup, currentParent);
                parentPathList[numParents++] = dup;
            }
        }

        /* Convert ResticLsEntry group → DirEntry array */
        dirEntries = (DirEntry*)malloc(sizeof(DirEntry) * groupCount);
        if (!dirEntries) continue;

        for (j = 0; j < groupCount; j++) {
            ResticLsEntry* le = &allEntries[groupStart + j];
            DirEntry* de = &dirEntries[j];
            strncpy(de->name, le->name, MAX_PATH - 1);
            de->name[MAX_PATH - 1] = '\0';
            de->isDirectory = (strcmp(le->type, "dir") == 0);
            de->fileSizeLow = le->sizeLow;
            de->fileSizeHigh = le->sizeHigh;
            de->lastWriteTime = ParseISOTime(le->mtime);
        }

        /* Store in SQLite persistent cache */
        LsCache_Store(repoName, shortId, currentParent, dirEntries, groupCount);

        /* If this group's parent matches the requested path, return these entries */
        if (strcmp(currentParent, requestedPathUtf8) == 0) {
            *outDirectChildren = dirEntries;  /* transfer ownership */
            *outDirectCount = groupCount;
        } else {
            free(dirEntries);
        }
    }

    /* Handle empty directories: dir entries whose path is not a parent of any other entry.
       parentPathList is already sorted (since allEntries was sorted by parent). */
    for (i = 0; i < allCount; i++) {
        BOOL found;
        int lo, hi;

        if (strcmp(allEntries[i].type, "dir") != 0) continue;

        /* Binary search for this dir's path in parentPathList */
        found = FALSE;
        lo = 0;
        hi = numParents - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int cmp = strcmp(allEntries[i].path, parentPathList[mid]);
            if (cmp == 0) { found = TRUE; break; }
            else if (cmp < 0) hi = mid - 1;
            else lo = mid + 1;
        }

        if (!found) {
            /* Empty directory — store sentinel so cache recognizes it */
            LsCache_Store(repoName, shortId, allEntries[i].path, NULL, 0);
        }
    }

    /* Cleanup parent path list */
    for (i = 0; i < numParents; i++) free(parentPathList[i]);
    free(parentPathList);
}

/* List directory contents inside a snapshot. Uses cache for repeat visits. */
static DirEntry* GetSnapshotContents(RepoConfig* repo, const char* sanitizedPath,
                                      const char* snapshotDisplayName, const char* subpath,
                                      int* outCount) {
    DirEntry* entries = NULL;
    int count = 0;
    char shortId[16];
    char originalPath[MAX_PATH];
    char lsSubpath[MAX_PATH];
    char args[MAX_PATH * 2];
    char* output;
    DWORD exitCode;
    int i;

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

    /* Check persistent SQLite cache.
       LsCache_Lookup returns non-NULL for any cache hit (even empty dirs). */
    {
        int dbCount = 0;
        DirEntry* dbEntries = LsCache_Lookup(repo->name, shortId, lsSubpathUtf8, &dbCount);
        if (dbEntries) {
            if (dbCount > 0) {
                /* Non-empty cache hit — populate in-memory cache */
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
            /* Empty directory cache hit — don't fetch from restic */
            free(dbEntries);
            *outCount = 0;
            return NULL;
        }
    }

    /* Check if snapshot was already fully loaded (bulk-cached).
       If so, and we got here (cache miss), the folder doesn't exist. */
    if (LsCache_IsSnapshotLoaded(repo->name, shortId)) {
        *outCount = 0;
        return NULL;
    }

    /* Cache miss — fetch full recursive listing from restic (no path filter,
       so we get ALL entries and can bulk-cache every subdirectory at once) */
    snprintf(args, sizeof(args), "ls --json %s", shortId);

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

    /* Parse ALL entries from restic ls and bulk-cache every subdirectory */
    {
        ResticLsEntry* allEntries = NULL;
        int allCount = ParseLsOutputAll(output, &allEntries);
        free(output);

        if (allCount <= 0) {
            free(allEntries);
            *outCount = 0;
            return NULL;
        }

        BulkCacheSubdirectories(repo->name, shortId, lsSubpathUtf8,
                                allEntries, allCount, &entries, &count);
        free(allEntries);

        /* Mark this snapshot as fully loaded so we don't re-fetch for non-existent paths */
        LsCache_MarkSnapshotLoaded(repo->name, shortId);
    }

    if (count <= 0 || !entries) {
        free(entries);
        *outCount = 0;
        return NULL;
    }

    *outCount = count;

    /* Store in in-memory directory listing cache (SQLite already done by BulkCacheSubdirectories) */
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

    /* Build the --path filter using the original backup path.
       For drive-root paths like "P:\", we need to escape the trailing backslash
       as "\\" to prevent it from escaping the closing quote in the command. */
    char originalPathUtf8[MAX_PATH];
    AnsiToUtf8(originalPath, originalPathUtf8, MAX_PATH);
    {
        int len = (int)strlen(originalPathUtf8);
        /* Double the trailing backslash for drive-root paths (e.g., "P:\" -> "P:\\") */
        if (len == 3 && originalPathUtf8[1] == ':' &&
            originalPathUtf8[2] == '\\' && len + 1 < MAX_PATH) {
            originalPathUtf8[3] = '\\';
            originalPathUtf8[4] = '\0';
        }
    }

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

        /* Skip if this mtime was already seen (same file version in multiple snapshots) */
        int duplicate = 0;
        int j;
        for (j = 0; j < i; j++) {
            if (strcmp(findEntries[i].mtime, findEntries[j].mtime) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) continue;

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
        /* Root: list configured repos + [Add Repository] + README.txt */
        int i;
        char readmePath[MAX_PATH];

        for (i = 0; i < g_RepoStore.count; i++) {
            if (g_RepoStore.repos[i].configured) {
                AddEntry(&entries, &count, &capacity,
                         g_RepoStore.repos[i].name, TRUE, 0, 0, ftNow);
            }
        }
        AddEntry(&entries, &count, &capacity,
                 "[Add Repository]", TRUE, 0, 0, ftNow);

        /* Add README.txt if it exists next to the DLL */
        if (GetReadmePath(readmePath, MAX_PATH)) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExA(readmePath, GetFileExInfoStandard, &fad)) {
                AddEntry(&entries, &count, &capacity,
                         "README.txt", FALSE, fad.nFileSizeLow, fad.nFileSizeHigh,
                         fad.ftLastWriteTime);
            }
        }
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
            if (strcmp(seg3, "[Refresh snapshot list]") == 0) {
                /* Invalidate cache - show hint so user knows to refresh */
                InvalidateSnapshotCache(repo->name);
                AddEntry(&entries, &count, &capacity,
                         "Snapshot cache cleared - go back to see it", FALSE, 0, 0, ftNow);
            }
            else if (IsAllFilesPath(seg3)) {
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

/* --- Rewrite helper --- */

/* Resolve a TC RemoteName into repo, original backup path, and restic file path
   for the rewrite command. Works from snapshot files, [All Files] files, and version entries.
   Does NOT require a specific snapshot ID (rewrite targets all snapshots). */
static BOOL ResolveFileForRewrite(const char* remoteName,
                                   RepoConfig** outRepo,
                                   char* outOriginalPath,   /* e.g. "D:\Fotky\Mix" */
                                   char* outResticFilePath)  /* e.g. "/D/Fotky/Mix/photo.jpg" */
{
    char seg1[MAX_PATH], seg2[MAX_PATH], seg3[MAX_PATH], rest[MAX_PATH];
    int numSegs = ParsePathSegments(remoteName, seg1, seg2, seg3, rest);
    if (numSegs < 3 || rest[0] == '\0') return FALSE;

    *outRepo = RepoStore_FindByName(seg1);
    if (!*outRepo) return FALSE;
    if (!RepoStore_EnsurePassword(*outRepo, g_PluginNr, g_RequestProc)) return FALSE;

    /* Get the original backup path from sanitized seg2 */
    if (!FindOriginalPath(*outRepo, seg2, outOriginalPath)) return FALSE;

    if (IsAllFilesPath(seg3)) {
        /* [All Files] path: rest might be "subdir\file.txt" or "[v] file.txt\..." */
        const char* vComp = FindVersionComponent(rest);
        char fileSubpath[MAX_PATH];

        if (vComp) {
            /* Version entry: extract actual filename */
            char pathBefore[MAX_PATH], vFileName[MAX_PATH], afterV[MAX_PATH];
            SplitAtVersionComponent(rest, pathBefore, vFileName, afterV);
            if (pathBefore[0])
                snprintf(fileSubpath, MAX_PATH, "%s\\%s", pathBefore, vFileName);
            else {
                strncpy(fileSubpath, vFileName, MAX_PATH - 1);
                fileSubpath[MAX_PATH - 1] = '\0';
            }
        } else {
            /* Direct file in [All Files] */
            strncpy(fileSubpath, rest, MAX_PATH - 1);
            fileSubpath[MAX_PATH - 1] = '\0';
        }

        BuildLsSubpath(outOriginalPath, fileSubpath, outResticFilePath, MAX_PATH);
    } else {
        /* Snapshot path: seg3 = "2025-01-28 10-30-05 (fb4ed15b)", rest = "subdir\file.txt" */
        BuildLsSubpath(outOriginalPath, rest, outResticFilePath, MAX_PATH);
    }

    /* Convert ANSI -> UTF-8 */
    char utf8Path[MAX_PATH];
    AnsiToUtf8(outResticFilePath, utf8Path, MAX_PATH);
    strncpy(outResticFilePath, utf8Path, MAX_PATH - 1);
    outResticFilePath[MAX_PATH - 1] = '\0';
    return TRUE;
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

    /* Handle README.txt at root */
    if (strcmp(RemoteName, "\\README.txt") == 0) {
        char readmePath[MAX_PATH];
        if (!GetReadmePath(readmePath, MAX_PATH))
            return FS_FILE_NOTFOUND;

        if (!(CopyFlags & FS_COPYFLAGS_OVERWRITE)) {
            if (GetFileAttributesA(LocalName) != INVALID_FILE_ATTRIBUTES)
                return FS_FILE_EXISTS;
        }

        if (CopyFileA(readmePath, LocalName, FALSE))
            return FS_FILE_OK;
        return FS_FILE_READERROR;
    }

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

    /* Handle README.txt at root - just open it */
    if (strcmp(RemoteName, "\\README.txt") == 0) {
        char readmePath[MAX_PATH];
        if (strcmp(Verb, "open") == 0 && GetReadmePath(readmePath, MAX_PATH)) {
            ShellExecuteA(NULL, "open", readmePath, NULL, NULL, SW_SHOWNORMAL);
            return FS_EXEC_OK;
        }
        return FS_EXEC_YOURSELF;
    }

    if (strcmp(Verb, "properties") == 0) {
        /* Rewrite: remove file from all snapshots in this backup path */
        char originalPath[MAX_PATH], resticFilePath[MAX_PATH];
        RepoConfig* repo;

        if (!ResolveFileForRewrite(RemoteName, &repo, originalPath, resticFilePath))
            return FS_EXEC_YOURSELF;

        /* Convert original path to restic format for --path flag */
        char originalPathUtf8[MAX_PATH];
        AnsiToUtf8(originalPath, originalPathUtf8, MAX_PATH);

        /* Build the command string for display */
        char cmdDisplay[2048];
        snprintf(cmdDisplay, sizeof(cmdDisplay),
                 "restic -r \"%s\" rewrite --exclude \"%s\" --path \"%s\" --forget",
                 repo->path, resticFilePath, originalPathUtf8);

        /* Show confirmation dialog with exact command */
        char confirmMsg[2048];
        snprintf(confirmMsg, sizeof(confirmMsg),
                 "Remove this file from ALL snapshots?\n\nCommand:\n%s", cmdDisplay);

        char buf[MAX_PATH] = {0};
        if (!g_RequestProc(g_PluginNr, RT_MsgYesNo,
                           "Confirm Rewrite", confirmMsg, buf, MAX_PATH))
            return FS_EXEC_OK;  /* User cancelled */

        /* Execute rewrite */
        DWORD rwExitCode;
        BOOL rwOk = RunResticRewrite(repo->path, repo->password,
                                      originalPathUtf8, resticFilePath, &rwExitCode);

        if (!rwOk || rwExitCode != 0) {
            g_RequestProc(g_PluginNr, RT_MsgOK, "Rewrite Failed",
                          "restic rewrite command failed. Check the repository.",
                          buf, MAX_PATH);
            return FS_EXEC_ERROR;
        }

        /* Invalidate caches since snapshot data changed */
        InvalidateSnapshotCache(repo->name);
        LsCache_InvalidateFile(repo->name, resticFilePath);

        /* Clear matching entries from in-memory cache */
        {
            char parentPath[MAX_PATH];
            const char* lastSlash = strrchr(resticFilePath, '/');
            if (lastSlash) {
                int len = (int)(lastSlash - resticFilePath);
                int i;
                if (len >= MAX_PATH) len = MAX_PATH - 1;
                memcpy(parentPath, resticFilePath, len);
                parentPath[len] = '\0';

                /* Remove all in-memory cache entries matching this parent path */
                i = 0;
                while (i < g_LsCacheCount) {
                    if (strcmp(g_LsCache[i].path, parentPath) == 0) {
                        free(g_LsCache[i].entries);
                        g_LsCacheCount--;
                        if (i < g_LsCacheCount) {
                            memmove(&g_LsCache[i], &g_LsCache[i + 1],
                                    sizeof(LsCacheEntry) * (g_LsCacheCount - i));
                        }
                    } else {
                        i++;
                    }
                }
            }
        }

        g_RequestProc(g_PluginNr, RT_MsgOK, "Rewrite Complete",
                      "File removed from snapshots. Run 'restic prune' to reclaim space.",
                      buf, MAX_PATH);
        return FS_EXEC_OK;
    }

    /* Only handle "open" verb */
    if (strcmp(Verb, "open") != 0)
        return FS_EXEC_YOURSELF;

    /* Handle [Refresh snapshot list] click */
    {
        char seg1[MAX_REPO_NAME], seg2[MAX_PATH], seg3[MAX_PATH], rest[MAX_PATH];
        int numSegs = ParsePathSegments(RemoteName, seg1, seg2, seg3, rest);

        if (numSegs == 3 && strcmp(seg3, "[Refresh snapshot list]") == 0) {
            RepoConfig* repo = RepoStore_FindByName(seg1);
            if (repo) {
                InvalidateSnapshotCache(repo->name);
            }
            /* Show message and let user refresh manually */
            if (g_RequestProc) {
                char buf[MAX_PATH] = {0};
                g_RequestProc(g_PluginNr, RT_MsgOK, "Cache Cleared",
                              "Snapshot cache cleared. Go back to see it.",
                              buf, MAX_PATH);
            }
            return FS_EXEC_OK;
        }
    }

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

            snprintf(restoreSub, MAX_PATH, "%s\\restore_%s_%08X",
                     tempDir, shortId, GetSecureRandomValue());
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
