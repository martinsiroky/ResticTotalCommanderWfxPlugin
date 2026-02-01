#include "wfx_interface.h"
#include <string.h>
#include <stdlib.h>

/* Global plugin state */
static int g_PluginNr = 0;
static tProgressProc g_ProgressProc = NULL;
static tLogProc g_LogProc = NULL;
static tRequestProc g_RequestProc = NULL;

/* Helper: create a FILETIME from a simple date */
static FILETIME MakeFileTime(WORD year, WORD month, WORD day,
                             WORD hour, WORD minute, WORD second) {
    SYSTEMTIME st;
    FILETIME ft;
    memset(&st, 0, sizeof(st));
    memset(&ft, 0, sizeof(ft));
    st.wYear = year;
    st.wMonth = month;
    st.wDay = day;
    st.wHour = hour;
    st.wMinute = minute;
    st.wSecond = second;
    SystemTimeToFileTime(&st, &ft);
    return ft;
}

/* Helper: add a directory entry */
static void AddEntry(DirEntry* entries, int* count, const char* name,
                     BOOL isDir, DWORD sizeLow, DWORD sizeHigh, FILETIME ft) {
    strncpy(entries[*count].name, name, MAX_PATH - 1);
    entries[*count].name[MAX_PATH - 1] = '\0';
    entries[*count].isDirectory = isDir;
    entries[*count].fileSizeLow = sizeLow;
    entries[*count].fileSizeHigh = sizeHigh;
    entries[*count].lastWriteTime = ft;
    (*count)++;
}

/* Returns dummy directory entries for the given path */
int GetEntriesForPath(const char* path, DirEntry* entries, int maxEntries) {
    int count = 0;

    if (strcmp(path, "\\") == 0) {
        AddEntry(entries, &count, "MyRepo", TRUE, 0, 0,
                 MakeFileTime(2025, 1, 28, 12, 0, 0));
    }
    else if (strcmp(path, "\\MyRepo") == 0) {
        AddEntry(entries, &count, "snapshots", TRUE, 0, 0,
                 MakeFileTime(2025, 1, 28, 12, 0, 0));
    }
    else if (strcmp(path, "\\MyRepo\\snapshots") == 0) {
        AddEntry(entries, &count, "2025-01-28_10-30-00", TRUE, 0, 0,
                 MakeFileTime(2025, 1, 28, 10, 30, 0));
        AddEntry(entries, &count, "2025-01-27_10-30-00", TRUE, 0, 0,
                 MakeFileTime(2025, 1, 27, 10, 30, 0));
    }
    else if (strcmp(path, "\\MyRepo\\snapshots\\2025-01-28_10-30-00") == 0 ||
             strcmp(path, "\\MyRepo\\snapshots\\2025-01-27_10-30-00") == 0) {
        AddEntry(entries, &count, "readme.txt", FALSE, 1024, 0,
                 MakeFileTime(2025, 1, 28, 10, 30, 0));
        AddEntry(entries, &count, "config.ini", FALSE, 512, 0,
                 MakeFileTime(2025, 1, 28, 10, 30, 0));
    }

    return count;
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
    return 0;
}

HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATAA* FindData) {
    SearchContext* ctx;
    DirEntry entries[MAX_ENTRIES];
    int count;

    count = GetEntriesForPath(Path, entries, MAX_ENTRIES);
    if (count == 0) {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    ctx = (SearchContext*)malloc(sizeof(SearchContext));
    if (!ctx) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }

    strncpy(ctx->path, Path, MAX_PATH - 1);
    ctx->path[MAX_PATH - 1] = '\0';
    ctx->index = 1;
    ctx->count = count;

    FillFindData(FindData, &entries[0]);

    return (HANDLE)ctx;
}

BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATAA* FindData) {
    SearchContext* ctx = (SearchContext*)Hdl;
    DirEntry entries[MAX_ENTRIES];
    int count;

    if (!ctx) return FALSE;

    count = GetEntriesForPath(ctx->path, entries, MAX_ENTRIES);
    if (ctx->index >= count) {
        return FALSE;
    }

    FillFindData(FindData, &entries[ctx->index]);
    ctx->index++;

    return TRUE;
}

int __stdcall FsFindClose(HANDLE Hdl) {
    if (Hdl && Hdl != INVALID_HANDLE_VALUE) {
        free((void*)Hdl);
    }
    return 0;
}

void __stdcall FsGetDefRootName(char* DefRootName, int maxlen) {
    strncpy(DefRootName, "Restic Repositories", maxlen - 1);
    DefRootName[maxlen - 1] = '\0';
}
