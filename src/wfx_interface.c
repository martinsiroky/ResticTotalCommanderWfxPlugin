#include "wfx_interface.h"
#include "repo_config.h"
#include "restic_process.h"
#include "json_parse.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Global plugin state */
static int g_PluginNr = 0;
static tProgressProc g_ProgressProc = NULL;
static tLogProc g_LogProc = NULL;
static tRequestProc g_RequestProc = NULL;

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
static int ParsePathSegments(const char* path, char* seg1, char* seg2, char* seg3) {
    const char* p;
    int segCount = 0;

    seg1[0] = '\0';
    seg2[0] = '\0';
    seg3[0] = '\0';

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

/* Fetch and parse all snapshots for a repo. Returns count, caller frees *outSnapshots. */
static int FetchSnapshots(RepoConfig* repo, ResticSnapshot** outSnapshots) {
    char* output;
    DWORD exitCode;
    int numSnaps;

    *outSnapshots = NULL;
    output = RunRestic(repo->path, repo->password, "snapshots", &exitCode);
    if (!output || exitCode != 0) {
        free(output);
        return 0;
    }

    numSnaps = ParseSnapshots(output, outSnapshots);
    free(output);
    return (numSnaps > 0) ? numSnaps : 0;
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

/* Returns heap-allocated directory entries for the given path. */
DirEntry* GetEntriesForPath(const char* path, int* outCount) {
    DirEntry* entries = NULL;
    int count = 0;
    int capacity = 0;
    char seg1[MAX_PATH], seg2[MAX_PATH], seg3[MAX_PATH];
    int numSegs;
    FILETIME ftNow;

    *outCount = 0;
    numSegs = ParsePathSegments(path, seg1, seg2, seg3);

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
    /* numSegs >= 3: inside a snapshot — Phase 3 will handle this */

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
