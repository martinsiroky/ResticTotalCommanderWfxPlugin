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

#endif /* JSON_PARSE_H */
