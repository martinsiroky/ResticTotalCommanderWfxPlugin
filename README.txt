restic-wfx - Total Commander Plugin for Restic Backups
======================================================

A Total Commander WFX filesystem plugin for browsing restic backup repositories.

New versions and details on https://github.com/martinsiroky/ResticTotalCommanderWfxPlugin


REQUIREMENTS
------------

- Total Commander 9.x or later (32-bit or 64-bit)
- restic.exe must be in your system PATH
  Download from: https://github.com/restic/restic/releases


INSTALLATION
------------

1. Copy the plugin files and README.txt to your Total Commander plugins directory
   (e.g., C:\Program Files\Total Commander\plugins\wfx\restic_wfx\)
   - 64-bit Total Commander: use restic_wfx.wfx64
   - 32-bit Total Commander: use restic_wfx.wfx

2. In Total Commander, go to Configuration > Options > Plugins > File System Plugins

3. Click "Add" and select the plugin file matching your TC version

4. The plugin will appear as "Restic Backups" in the Network Neighborhood


ADDING A REPOSITORY
-------------------

1. Open Network Neighborhood (or press Ctrl+\)

2. Navigate to "Restic Backups"

3. Double-click "[Add Repository]"

4. Enter the repository path (local path or remote URL)
   Examples:
   - C:\Backups\myrepo
   - /mnt/backup/repo
   - sftp:user@host:/path/to/repo
   - rest:https://user:pass@host:8000/

5. Enter a display name for the repository

6. Enter the repository password
   OR leave blank and provide a path to a password file


BROWSING BACKUPS
----------------

Repository structure:
  \RepoName\                     - List of backup paths
  \RepoName\PathName\            - List of snapshots + [All Files]
  \RepoName\PathName\Snapshot\   - Directory listing from that snapshot

[All Files] view:
  Shows a merged view of files across all snapshots for the selected path.
  Directories are listed first. Files appear with a "[show all versions]"
  label before their extension (e.g., "photo [show all versions].jpg").
  Press Enter on such a file to see all its versions across snapshots.
  Each version is shown as "photo - 2025-01-28 10-30-05 (fb4ed15b).jpg".


CUSTOM COLUMNS
--------------

The plugin provides a "Cache Status" custom column that shows whether a
snapshot's directory listing has been cached locally:
  - Individual snapshots show "cached" if their listing is stored locally
  - [All Files] shows "cached 3 of 5 snapshots" (how many are cached)

To add it in Total Commander:
  1. Right-click the column header and select "Custom Columns"
  2. Click "Add Column" and select "[=fs.Cache Status]"

Cached snapshots load instantly; uncached ones are fetched from restic
on first access.


COPYING FILES (F5)
------------------

1. Navigate to a file or directory in a snapshot

2. Press F5 to copy to the other panel

3. Multi-file copy is optimized: the plugin pre-extracts files to a temp
   directory for faster copying when selecting multiple files


OPENING FILES (Enter)
---------------------

Double-click or press Enter on a file to:
- Extract it to a temporary directory
- Open it with the associated application


REMOVING FILES FROM BACKUPS (Properties)
----------------------------------------

To permanently remove a file from all snapshots:

1. Navigate to the file in the [All Files] view or a specific snapshot

2. Right-click and select "Properties" (or press Alt+Enter)

3. Confirm the rewrite operation

4. After removal, run 'restic prune' externally to reclaim disk space

WARNING: This operation modifies your backup repository and cannot be undone!


CONFIGURATION
-------------

Plugin data is stored in:
  %APPDATA%\GHISLER\plugins\wfx\restic_wfx\

- restic_wfx.ini         - Repository configuration (paths, names)
- cache\*.db             - SQLite cache for directory listings
- restic_commands.log    - Log of restic commands (for troubleshooting)

Passwords are never stored on disk. They are kept in memory only for the
duration of the Total Commander session.


TROUBLESHOOTING
---------------

"Could not connect to repository":
  - Verify the repository path is correct
  - Check that restic.exe is in your PATH (run 'restic version' in cmd)
  - Verify the password is correct

Slow browsing:
  - First access to a snapshot fetches data from restic (may take time)
  - Subsequent access uses cached data for faster browsing
  - Large repositories with many files may take longer to cache initially

Cache issues:
  - Delete the cache directory to force refresh:
    %APPDATA%\GHISLER\plugins\wfx\restic_wfx\cache\


LICENSE
-------

This software is licensed under the MIT License.
See LICENSE.txt for the full license text.


THIRD-PARTY COMPONENTS
----------------------

This plugin uses the following third-party libraries:

- cJSON (https://github.com/DaveGamble/cJSON)
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
  Licensed under the MIT License

- SQLite (https://www.sqlite.org/)
  Public domain - no license required
