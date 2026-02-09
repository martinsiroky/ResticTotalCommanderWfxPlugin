# Project: restic-wfx

Total Commander WFX filesystem plugin for browsing restic backup repositories.

## Build

- **Language:** C11
- **Toolchain:** MinGW GCC
- **Build system:** CMake with `MinGW Makefiles` generator
- **Output:** `restic_wfx.wfx64` (64-bit) or `restic_wfx.wfx` (32-bit), ~1.5MB
- **Linked libraries:** shlwapi, shell32, static libgcc
- **Version:** Defined in `CMakeLists.txt` (`RESTIC_WFX_VERSION`)

### Release Build (recommended)

Run `build_release.bat` to build both 32-bit and 64-bit plugins and create a release zip:

```cmd
build_release.bat
```

Output: `release/restic_wfx_<version>.zip` containing both plugins and README.txt.

**Prerequisites:** Install MSYS2 toolchains (or have CLion installed for 64-bit fallback):

```bash
# In MSYS2 terminal
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-make   # 64-bit
pacman -S mingw-w64-i686-gcc mingw-w64-i686-make       # 32-bit
```

### Manual Build

**64-bit** (using CLion's bundled MinGW):
```bash
export PATH="/c/Program Files/JetBrains/CLion 2025.3.2/bin/mingw/bin:$PATH"
cmake -B build64 -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build64
```

**32-bit** (using MSYS2):
```bash
export PATH="/c/msys64/mingw32/bin:$PATH"
cmake -B build32 -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build32
```

### Notes

- Use separate build directories (`build32`/`build64`) to avoid CMake cache conflicts
- CMake auto-detects architecture from the compiler and sets the correct suffix

## Architecture

### Source files (`src/`)

| File | Purpose |
|------|---------|
| `plugin_main.c` | DLL entry point |
| `wfx_interface.c` | Core WFX plugin interface: path routing, caching, file operations |
| `wfx_interface.h` | `DirEntry` and `SearchContext` types, `GetEntriesForPath()` |
| `restic_process.c` | Process management: `RunRestic()`, `RunResticDump()`, `RunResticRestore()` |
| `json_parse.c` | JSON parsing for restic output (snapshots, ls, find) |
| `repo_config.c` | Repository config persistence via INI file |
| `ls_cache.c` | Persistent directory listing cache via SQLite |
| `ls_cache.h` | Public API: Init, Lookup, Store, Purge, DeleteRepo, InvalidateFile, Shutdown |

### Vendor libraries (`vendor/`)

- **cJSON** — JSON parser (used by `json_parse.c`)
- **SQLite 3.47.2** — Amalgamation build with `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_THREADSAFE=1`, `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1`

### Key types

- `DirEntry` — directory entry with name, isDirectory, size (low/high DWORDs), lastWriteTime (FILETIME)
- `SearchContext` — iteration handle for FsFindFirst/FsFindNext (owns DirEntry array)
- `ResticSnapshot` — parsed snapshot with id, shortId, time, hostname, paths[]
- `RepoConfig` — repo name, path, password (in-memory only)

### Virtual filesystem path structure

```
\                                    → list repos + [Add Repository]
\RepoName                            → list backup paths (sanitized)
\RepoName\PathName                   → list snapshots + [All Files] + [Refresh snapshot list]
\RepoName\PathName\SnapshotDisplay   → directory listing from restic ls
\RepoName\PathName\[All Files]       → merged view across all snapshots
\RepoName\PathName\[All Files]\[v] file.txt → version listing via restic find
```

### Caching layers

1. **Snapshot cache** (`g_SnapCache[]`) — TTL-based (5 min), per-repo, max 16 entries
2. **Directory listing cache** (`g_LsCache[]`) — in-memory, immutable, FIFO eviction, max 32 entries
3. **Persistent directory listing cache** (SQLite) — `%APPDATA%\GHISLER\plugins\wfx\restic_wfx\cache\<repo>.db`
   - Schema: `cached_dirs` (sentinel table) + `dir_entries` (actual entries)
   - Keyed on `(short_id, path)`
   - Lookup flow: in-memory -> SQLite -> restic CLI
   - Purged when FetchSnapshots() refreshes (removes deleted snapshot entries)
   - `InvalidateFile()` for targeted invalidation after file removal
   - WAL journal mode for crash safety

### Batch restore optimization

`FsStatusInfo(FS_STATUS_OP_GET_MULTI)` triggers deferred `restic restore` on first `FsGetFile`, pre-extracting files to a temp directory for fast multi-file copy (F5 in TC).

### Path encoding

- TC passes ANSI (system codepage) paths
- Restic uses UTF-8 internally
- `AnsiToUtf8()` / `Utf8ToAnsi()` convert at boundaries
- Windows drive paths converted to restic format: `D:\Fotky\Mix` -> `/d/Fotky/Mix`
- Sanitized path names replace `\ / :` with `_` for display

### Important conventions

- All cache lookups return deep copies (caller must free)
- Errors in persistent cache degrade gracefully (log + continue without cache)
- Passwords are never persisted to disk; zeroed with `SecureZeroMemory` on disconnect
- Plugin exports defined in `restic_wfx.def`
- All persistent data lives under `%APPDATA%\GHISLER\plugins\wfx\restic_wfx\`
  - INI config: `restic_wfx.ini`
  - SQLite cache: `cache\<repo>.db`
