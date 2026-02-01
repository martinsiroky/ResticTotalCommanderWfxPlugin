#ifndef WFX_INTERFACE_H
#define WFX_INTERFACE_H

#include "fsplugin.h"

#define MAX_ENTRIES 10

/* Search context used as the HANDLE returned by FsFindFirst */
typedef struct {
    char path[MAX_PATH];
    int index;
    int count;
} SearchContext;

/* A single entry in a directory listing */
typedef struct {
    char name[MAX_PATH];
    BOOL isDirectory;
    DWORD fileSizeLow;
    DWORD fileSizeHigh;
    FILETIME lastWriteTime;
} DirEntry;

/* Get dummy directory entries for a given path.
   Returns count of entries filled into the entries array. */
int GetEntriesForPath(const char* path, DirEntry* entries, int maxEntries);

#endif /* WFX_INTERFACE_H */
