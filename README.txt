restic-wfx - Total Commander Plugin for Restic Backups
======================================================

A Total Commander WFX filesystem plugin for browsing restic backup repositories.


REQUIREMENTS
------------

- Total Commander 9.x or later (64-bit)
- restic.exe must be in your system PATH
  Download from: https://github.com/restic/restic/releases


INSTALLATION
------------

1. Copy restic_wfx.wfx64 to your Total Commander plugins directory
   (e.g., C:\Program Files\Total Commander\plugins\wfx\restic_wfx\)

2. In Total Commander, go to Configuration > Options > Plugins > File System Plugins

3. Click "Add" and select restic_wfx.wfx64

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
  When browsing a file, you'll see its versions from different snapshots.


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

- restic_wfx.ini    - Repository configuration (paths, names)
- cache\*.db        - SQLite cache for directory listings

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

This plugin is provided as-is for personal use with Total Commander.
