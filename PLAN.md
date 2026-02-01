# Total Commander WFX Plugin for Restic Repository Access

## Project Overview

A WFX plugin (C, DLL) that allows Total Commander to browse restic backup repositories — navigating snapshots and their file trees as a virtual file system. Read-only by design (restic is append-only).

## Current Architecture

```
Total Commander
    ↓ WFX API (stdcall)
restic_wfx.wfx64  (64-bit DLL, C11, MinGW)
    ↓ CreateProcessA + pipe
restic.exe CLI  (must be in PATH)
    ↓ JSON stdout
cJSON parser
```

### Source Files

```
restic-wfx-plugin/
├── CMakeLists.txt              # CMake 3.15+, C11, output: restic_wfx.wfx64
├── restic_wfx.def              # DLL exports: FsInit, FsFindFirst/Next/Close, FsGetDefRootName
├── include/
│   └── fsplugin.h              # TC WFX SDK header (v2.1)
├── src/
│   ├── plugin_main.c           # DllMain entry point
│   ├── wfx_interface.h         # DirEntry, SearchContext structs
│   ├── wfx_interface.c         # WFX API + navigation logic + snapshot browsing
│   ├── restic_process.h        # RunRestic() declaration
│   ├── restic_process.c        # Spawn restic, capture stdout via pipe
│   ├── json_parse.h            # ResticSnapshot, ResticLsEntry structs, parse functions
│   ├── json_parse.c            # Parse snapshots JSON + ls NDJSON output
│   ├── repo_config.h           # RepoConfig, RepoStore structs
│   └── repo_config.c           # INI config read/write, add-repo dialog
└── vendor/
    ├── cJSON.c                 # Third-party JSON library
    └── cJSON.h
```

## Navigation Structure (Implemented)

```
\                                           → Level 0: list repositories + [Add Repository]
\RepoName\                                  → Level 1: unique sanitized backup paths
\RepoName\D_Fotky_Mix\                      → Level 2: snapshots matching that path (newest first)
\RepoName\D_Fotky_Mix\2025-01-28 10-30-05 (fb4ed15b)\          → Level 3+: files & folders
\RepoName\D_Fotky_Mix\2025-01-28 10-30-05 (fb4ed15b)\subdir\   → deeper subdirectories
```

### Path Handling

- **Sanitization** (display): `D:\Fotky\Mix` → `D_Fotky_Mix` (replace `\/:` with `_`, strip edges)
- **Restic internal format**: `D:\Fotky\Mix` → `/D/Fotky/Mix` (strip colon, prepend `/`, forward slashes)
- **Snapshot display name**: `YYYY-MM-DD HH-MM-SS (shortId)` — short ID extracted for restic commands

## Restic Commands Used

| Command | Purpose | Called From |
|---------|---------|-------------|
| `restic -r "<repo>" snapshots --json` | List all snapshots | `FetchSnapshots()` |
| `restic -r "<repo>" ls --json <shortId> "<path>"` | List files in snapshot directory | `GetSnapshotContents()` |
| `restic -r "<repo>" snapshots` | Validate repo on add (no --json) | `RepoStore_PromptAdd()` |

Password passed via `RESTIC_PASSWORD` env var, cleared immediately after process spawn.

## Implemented Features (Phase 1 + 2 complete)

- [x] Plugin skeleton: FsInit, FsFindFirst/Next/Close, FsGetDefRootName
- [x] Repository management: add repos via dialog, persist in INI, password prompt
- [x] Snapshot listing: parse `restic snapshots --json`, sort newest-first
- [x] Backup path grouping: deduplicate paths across snapshots, sanitize for display
- [x] Snapshot content browsing: `restic ls --json`, NDJSON parsing, direct-child filtering
- [x] Nested directory navigation: arbitrary depth via `rest` path segment
- [x] File metadata: sizes (64-bit), modification times, directory vs file distinction
- [x] UTF-8 → ANSI conversion for international filenames
- [x] Secure password handling: in-memory only, `SecureZeroMemory`, env var cleared
- [x] Config persistence: `%APPDATA%\TotalCmd\restic_wfx.ini`

## Data Structures

```c
DirEntry        { name, isDirectory, fileSizeLow, fileSizeHigh, lastWriteTime }
SearchContext   { path, index, count, *entries }
ResticSnapshot  { id[65], shortId[16], time[32], hostname[128], paths[8][MAX_PATH], pathCount }
ResticLsEntry   { name[MAX_PATH], path[MAX_PATH], type[16], sizeLow, sizeHigh, mtime[32] }
RepoConfig      { name[64], path[512], password[256], configured, hasPassword }
RepoStore       { repos[16], count, configFilePath }
```

## Next Phase: File Operations (Phase 3)

### Required WFX Functions

1. **`FsGetFile()`** — Copy file from restic snapshot to local filesystem
   - Run `restic restore <snapshotId> --target <tempDir> --include "<filePath>"`
   - Copy restored file to destination
   - Clean up temp directory
   - Report progress via `g_ProgressProc` callback

2. **`FsExecuteFile()`** — Open/view file from snapshot
   - Restore to temp directory
   - Return `FS_EXEC_YOURSELF` or launch with `ShellExecute`
   - Consider: keep temp files until plugin unloads?

### Implementation Details

- Extract snapshot ID + file path from TC path segments
- Convert TC path back to restic internal format (`/D/Fotky/Mix/file.jpg`)
- Use `restic restore` with `--include` for single-file extraction
- Temp directory: `%TEMP%\restic_wfx\<shortId>\`
- Progress callbacks for large file restores
- Export new functions in `restic_wfx.def`

## Phase 4: Polish & Performance

- [ ] Cache snapshot list per-repo (TTL-based, avoid re-fetching on every navigation)
- [ ] Cache directory listings per snapshot+path
- [ ] Progress callbacks for long `restic ls` operations
- [ ] Better error messages (missing restic binary, wrong password, network errors)
- [ ] FsDisconnect() for cleanup
- [ ] FsSetCryptCallback() for TC password manager integration
- [ ] Repository statistics virtual file

## Limits

| Constant | Value |
|----------|-------|
| MAX_REPOS | 16 |
| MAX_SNAP_PATHS | 8 per snapshot |
| MAX_REPO_NAME | 64 bytes |
| MAX_REPO_PATH | 512 bytes |
| MAX_REPO_PASS | 256 bytes |
| Process timeout | 120 seconds |

## Build

```bash
cmake -B build -G "MinGW Makefiles"
cmake --build build
# Output: build/restic_wfx.wfx64
```

## Dependencies

- **Build**: CMake 3.15+, MinGW-w64 (C11)
- **Runtime**: restic.exe in PATH, Total Commander (64-bit for .wfx64)
- **Bundled**: cJSON (vendor/), WFX SDK header (include/fsplugin.h)
- **Linked**: shlwapi, shell32, static libgcc (MinGW)

## Future Enhancements

- Snapshot comparison / diff view
- Search within snapshots
- Restic backup creation from TC
- Mount snapshots as drive letters (via restic mount)
- Support for restic password files / key files
- Windows DPAPI for encrypted password storage
