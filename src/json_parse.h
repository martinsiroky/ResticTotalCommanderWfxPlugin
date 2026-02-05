#ifndef JSON_PARSE_H
#define JSON_PARSE_H

#include <windows.h>

#define MAX_SNAP_PATHS 8

typedef struct {
    char id[65];            /* full 64-char hex snapshot ID */
    char shortId[16];       /* short ID e.g. "196bc576" */
    char time[32];          /* ISO 8601 timestamp */
    char hostname[128];
    char paths[MAX_SNAP_PATHS][MAX_PATH];  /* individual backup paths */
    int pathCount;
} ResticSnapshot;

/* Parse restic snapshots JSON output.
   json: the raw JSON string from `restic snapshots --json`
   outSnapshots: receives a malloc'd array of ResticSnapshot (caller must free)
   Returns the number of snapshots parsed, or -1 on error. */
int ParseSnapshots(const char* json, ResticSnapshot** outSnapshots);

/* Parse an ISO 8601 time string into a FILETIME.
   timeStr: e.g. "2025-01-28T10:30:05.310764668Z"
   Returns the FILETIME (zeroed on parse failure). */
FILETIME ParseISOTime(const char* timeStr);

/* Convert UTF-8 string to the system ANSI codepage. */
void Utf8ToAnsi(const char* utf8, char* ansi, int ansiSize);

/* Convert system ANSI codepage string to UTF-8. */
void AnsiToUtf8(const char* ansi, char* utf8, int utf8Size);

/* A single entry from `restic ls --json` output */
typedef struct {
    char name[MAX_PATH];      /* file/folder name (last path component) */
    char path[MAX_PATH];      /* full path within the snapshot */
    char type[16];            /* "file", "dir", or "symlink" */
    DWORD sizeLow;
    DWORD sizeHigh;
    char mtime[32];           /* ISO 8601 modification time */
} ResticLsEntry;

/* Parse NDJSON output from `restic ls --json`.
   Filters to only direct children of parentPath.
   ndjson: raw NDJSON string (one JSON object per line)
   parentPath: directory path to list children for (forward slashes)
   outEntries: receives a malloc'd array of ResticLsEntry (caller must free)
   Returns the number of entries, or -1 on error. */
int ParseLsOutput(const char* ndjson, const char* parentPath, ResticLsEntry** outEntries);

/* Parse ALL entries from restic ls NDJSON output (no parent filtering).
   Returns count of entries, or -1 on error. Caller must free *outEntries. */
int ParseLsOutputAll(const char* ndjson, ResticLsEntry** outEntries);

/* A single entry from `restic find --json` output */
typedef struct {
    char snapshotId[65];   /* full snapshot ID */
    char shortId[16];      /* first 8 chars */
    char path[MAX_PATH];
    char type[16];         /* "file", "dir" */
    DWORD sizeLow;
    DWORD sizeHigh;
    char mtime[32];        /* ISO 8601 */
} ResticFindEntry;

/* Parse JSON array output from `restic find --json`.
   json: the raw JSON string
   outEntries: receives a malloc'd array of ResticFindEntry (caller must free)
   Returns the number of entries, or -1 on error. */
int ParseFindOutput(const char* json, ResticFindEntry** outEntries);

#endif /* JSON_PARSE_H */
