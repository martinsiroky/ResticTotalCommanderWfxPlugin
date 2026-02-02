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

/* Cancellation check callback for RunResticWithProgress.
   Return TRUE to continue, FALSE to abort. */
typedef BOOL (*ResticCancelFunc)(void* userData);

/* Same as RunRestic, but periodically calls cancelCb during the read loop.
   If cancelCb returns FALSE, the process is terminated and NULL is returned.
   cancelCb may be NULL (behaves identically to RunRestic). */
char* RunResticWithProgress(const char* repoPath, const char* password,
                            const char* args, DWORD* exitCode,
                            ResticCancelFunc cancelCb, void* userData);

/* Progress callback for RunResticDump.
   bytesWritten: total bytes written so far
   totalSize:    expected total size (0 if unknown)
   userData:     opaque pointer passed through from caller
   Return TRUE to continue, FALSE to abort. */
typedef BOOL (*DumpProgressFunc)(LONGLONG bytesWritten, LONGLONG totalSize, void* userData);

/* Run "restic dump <snapshotId> <filePath>" and write stdout to outputPath.
   Streams data directly to file (no in-memory buffering).
   progressCb may be NULL. exitCode may be NULL.
   On failure or abort, partial output file is deleted.
   Returns TRUE on success, FALSE on failure/abort. */
BOOL RunResticDump(const char* repoPath, const char* password,
                   const char* snapshotId, const char* filePath,
                   const char* outputPath, LONGLONG totalSize,
                   DumpProgressFunc progressCb, void* userData,
                   DWORD* exitCode);

#endif /* RESTIC_PROCESS_H */
