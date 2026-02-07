# restic-wfx - Total Commander Plugin for Restic Backups

A Total Commander WFX filesystem plugin for browsing restic backup repositories.

New versions and details on https://github.com/martinsiroky/ResticTotalCommanderWfxPlugin

## Requirements

- Total Commander 9.x or later (64-bit)
- restic.exe must be in your system PATH
  Download from: https://github.com/restic/restic/releases

## Screenshots

### Repository list and configuration
![Manage restic repositories.png](doc/Manage%20restic%20repositories.png)

### List of snapshots for a backup path
![List of snapshots for path.png](doc/List%20of%20snapshots%20for%20path.png)

### Simple snapshot view (files/directories as of that snapshot)
![Snapshot detail.png](doc/Snapshot%20detail.png)

### [All Files] can show all versions of files across snapshots for a path
![List of files (with multiple versions).png](doc/List%20of%20files%20%28with%20multiple%20versions%29.png)

### File versions across snapshots in the [All Files] view
![File versions in multiple snapshots.png](doc/File%20versions%20in%20multiple%20snapshots.png)

## Installation

1. Copy `restic_wfx.wfx64` and `README.txt` to your Total Commander plugins directory
   (e.g., `C:\Program Files\Total Commander\plugins\wfx\restic_wfx\`)

2. In Total Commander, go to **Configuration > Options > Plugins > File System Plugins**

3. Click "Add" and select `restic_wfx.wfx64`

4. The plugin will appear as "Restic Backups" in the Network Neighborhood

## Adding a Repository

1. Open Network Neighborhood (or press `Ctrl+\`)

2. Navigate to "Restic Backups"

3. Double-click `[Add Repository]`

4. Enter the repository path (local path or remote URL)
   Examples:
   - `C:\Backups\myrepo`
   - `/mnt/backup/repo`
   - `sftp:user@host:/path/to/repo`
   - `rest:https://user:pass@host:8000/`

5. Enter a display name for the repository

6. Enter the repository password
   OR leave blank and provide a path to a password file

## Browsing Backups

Repository structure:

```
\RepoName\                     - List of backup paths
\RepoName\PathName\            - List of snapshots + [All Files]
\RepoName\PathName\Snapshot\   - Directory listing from that snapshot
```

**[All Files] view:**
Shows a merged view of files across all snapshots for the selected path.
When browsing a file, you'll see its versions from different snapshots.

## Copying Files (F5)

1. Navigate to a file or directory in a snapshot

2. Press F5 to copy to the other panel

3. Multi-file copy is optimized: the plugin pre-extracts files to a temp
   directory for faster copying when selecting multiple files

## Opening Files (Enter)

Double-click or press Enter on a file to:
- Extract it to a temporary directory
- Open it with the associated application

## Removing Files from Backups (Properties)

To permanently remove a file from all snapshots:

1. Navigate to the file in the `[All Files]` view or a specific snapshot

2. Right-click and select "Properties" (or press `Alt+Enter`)

3. Confirm the rewrite operation

4. After removal, run `restic prune` externally to reclaim disk space

> **WARNING:** This operation modifies your backup repository and cannot be undone!

## Configuration

Plugin data is stored in:
```
%APPDATA%\GHISLER\plugins\wfx\restic_wfx\
```

- `restic_wfx.ini` - Repository configuration (paths, names)
- `cache\*.db` - SQLite cache for directory listings

Passwords are never stored on disk. They are kept in memory only for the
duration of the Total Commander session.

## Troubleshooting

**"Could not connect to repository":**
- Verify the repository path is correct
- Check that restic.exe is in your PATH (run `restic version` in cmd)
- Verify the password is correct

**Slow browsing:**
- First access to a snapshot fetches data from restic (may take time)
- Subsequent access uses cached data for faster browsing
- Large repositories with many files may take longer to cache initially

**Cache issues:**
- Delete the cache directory to force refresh:
  `%APPDATA%\GHISLER\plugins\wfx\restic_wfx\cache\`

## License

This software is licensed under the MIT License.
See [LICENSE.txt](LICENSE.txt) for the full license text.

## Third-Party Components

This plugin uses the following third-party libraries:

- **cJSON** (https://github.com/DaveGamble/cJSON)
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  Licensed under the MIT License

- **SQLite** (https://www.sqlite.org/)
  Public domain - no license required
