#ifndef RESTIC_PROCESS_H
#define RESTIC_PROCESS_H

#include <windows.h>

/* Run a restic command and capture its stdout.
   repoPath: restic repository path (e.g. "C:\backup\repo" or "s3:...")
   password: repository password (passed via RESTIC_PASSWORD env var)
   args:     additional arguments (e.g. "snapshots --json")
   exitCode: receives the process exit code (may be NULL)

   Returns a malloc'd buffer containing stdout output (caller must free).
   Returns NULL on failure (process creation failed, etc.). */
char* RunRestic(const char* repoPath, const char* password,
                const char* args, DWORD* exitCode);

#endif /* RESTIC_PROCESS_H */
