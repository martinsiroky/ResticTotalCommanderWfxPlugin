#ifndef LS_CACHE_H
#define LS_CACHE_H

#include "wfx_interface.h"

/* Initialize the persistent directory listing cache subsystem. */
void LsCache_Init(void);

/* Look up a cached directory listing.
   Returns a malloc'd DirEntry array (caller must free), or NULL on miss.
   Sets *outCount to the number of entries. */
DirEntry* LsCache_Lookup(const char* repoName, const char* shortId,
                          const char* path, int* outCount);

/* Store a directory listing in the persistent cache.
   Wraps all inserts in a single transaction. */
void LsCache_Store(const char* repoName, const char* shortId,
                   const char* path, const DirEntry* entries, int count);

/* Purge cached entries for snapshots no longer present.
   Deletes rows where short_id is not in validShortIds[0..validCount-1].
   Returns the number of rows deleted, or -1 on error. */
int LsCache_Purge(const char* repoName, const char** validShortIds, int validCount);

/* Delete the entire database for a repository. */
void LsCache_DeleteRepo(const char* repoName);

/* Check if a snapshot has been fully loaded (bulk-cached). */
BOOL LsCache_IsSnapshotLoaded(const char* repoName, const char* shortId);

/* Mark a snapshot as fully loaded after bulk caching. */
void LsCache_MarkSnapshotLoaded(const char* repoName, const char* shortId);

/* Shut down the persistent cache: close all open DB connections. */
void LsCache_Shutdown(void);

#endif /* LS_CACHE_H */
