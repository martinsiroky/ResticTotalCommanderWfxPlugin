/*
 * restic-wfx - Total Commander plugin for browsing restic backup repositories
 * Copyright (c) 2026 Martin Široký
 * SPDX-License-Identifier: MIT
 */

#ifndef WFX_INTERFACE_H
#define WFX_INTERFACE_H

#include "fsplugin.h"

/* A single entry in a directory listing */
typedef struct {
    char name[MAX_PATH];
    BOOL isDirectory;
    DWORD fileSizeLow;
    DWORD fileSizeHigh;
    FILETIME lastWriteTime;
} DirEntry;

/* Search context used as the HANDLE returned by FsFindFirst.
   Owns the entries array — freed in FsFindClose. */
typedef struct {
    char path[MAX_PATH];
    int index;              /* next item to return */
    int count;              /* total entries */
    DirEntry *entries;      /* heap-allocated array */
} SearchContext;

/* Get directory entries for a given path.
   Returns heap-allocated DirEntry array (caller must free).
   Sets *outCount to the number of entries. Returns NULL if none. */
DirEntry* GetEntriesForPath(const char* path, int* outCount);

#endif /* WFX_INTERFACE_H */
