# Total Commander WFX Plugin for Restic Repository Access

## Project Overview

A WFX (Wacker File System) plugin that allows Total Commander to access restic repositories. This enables browsing snapshots and files within a restic repository as if they were a regular file system.

## Core Architecture

### Plugin Structure
- **WFX Interface Layer**: Implements Total Commander's WFX API (C/C++ DLL)
- **Restic Bridge**: Spawns and communicates with restic CLI process
- **Cache Layer**: Stores snapshot metadata and file listings for performance
- **Authentication Manager**: Handles repository passwords/key files

## Key Components

### 1. WFX API Implementation

Critical functions to implement:
- `FsInit()` - Initialize plugin, load configuration
- `FsFindFirst()`/`FsFindNext()` - Directory enumeration
- `FsExecuteFile()` - Open files (trigger restic restore to temp)
- `FsGetFile()` - Copy files from repository (restore operation)
- `FsDisconnect()` - Clean up connections
- `FsSetCryptCallback()` - Handle password input from Total Commander

### 2. Repository Navigation Structure

```
/
├── [Repository Name]/
│   ├── snapshots/
│   │   ├── 2025-01-28_10-30-00/
│   │   │   ├── home/
│   │   │   └── etc/
│   │   └── 2025-01-27_10-30-00/
│   ├── stats.txt (virtual file showing repo stats)
│   └── config/ (virtual folder for repository info)
```

### 3. Restic Integration Strategy

#### Process Communication
- Use `restic snapshots --json` for snapshot listing
- Use `restic ls --json <snapshot>` for file listings
- Use `restic restore` to temp directory for file access
- Use `restic stats` for repository information

#### Caching Strategy
- Cache snapshot list (refresh on demand or timer)
- Cache directory listings per snapshot
- Implement TTL for cache entries
- Store cache in `%APPDATA%\TotalCommander\ResticWFX\`

## Implementation Plan

### Phase 1: Foundation (Week 1-2)
1. Set up C++ project with WFX plugin template
2. Implement basic WFX skeleton with dummy data
3. Test plugin loads in Total Commander
4. Implement configuration dialog for repository path and password

### Phase 2: Restic Integration (Week 3-4)
1. Implement process spawning for restic commands
2. Parse JSON output from restic
3. Build snapshot listing functionality
4. Implement directory traversal for snapshots

### Phase 3: File Operations (Week 5-6)
1. Implement file restoration to temporary directory
2. Handle file copying from repository
3. Implement file preview/opening
4. Add progress callbacks for long operations

### Phase 4: Polish & Features (Week 7-8)
1. Add caching layer for performance
2. Implement background refresh
3. Add repository statistics view
4. Error handling and logging
5. Configuration persistence

## Technical Considerations

### Language Choice
- **C++** is recommended for WFX plugins (native Total Commander API)
- Consider using Java via JNI if you prefer, but adds complexity
- Alternative: Use C++ for WFX wrapper, shell out to Java for restic logic

### Challenges
- **Read-only nature**: Restic repositories are append-only; implement accordingly
- **Performance**: File listings can be slow; aggressive caching needed
- **Password management**: Secure storage of repository passwords
- **Large repositories**: Handle repositories with thousands of snapshots
- **Network repositories**: Support for REST, S3, SFTP backend types

### Dependencies
- WFX Plugin SDK from Total Commander
- JSON parsing library (e.g., nlohmann/json for C++)
- Process execution library
- Restic binary (must be in PATH or configured)

## Configuration File Format

```ini
[Repository1]
Path=C:\backup\restic-repo
Type=local
Password=encrypted_password_hash
CacheTimeout=300

[Repository2]
Path=s3:s3.amazonaws.com/my-bucket
Type=s3
Password=encrypted_password_hash
CacheTimeout=300
```

## Java Integration Option

If you prefer to use Java for business logic:

1. Create JNI wrapper DLL in C++ (implements WFX API)
2. Java application handles restic communication
3. Use JNI to call Java methods from C++ WFX callbacks
4. Package JRE with plugin or require Java installation

### Architecture with Java
```
Total Commander
    ↓
WFX Plugin (C++ DLL)
    ↓ JNI
Java Layer
    ↓
Restic CLI Process
```

## File Structure

```
restic-wfx-plugin/
├── src/
│   ├── cpp/
│   │   ├── wfx_interface.cpp      # WFX API implementation
│   │   ├── plugin_main.cpp        # DLL entry point
│   │   └── jni_bridge.cpp         # Optional: JNI integration
│   ├── java/                      # Optional: If using Java
│   │   ├── ResticBridge.java
│   │   ├── SnapshotManager.java
│   │   └── CacheManager.java
│   └── include/
│       ├── wfxplugin.h           # Total Commander WFX header
│       └── restic_types.h
├── resources/
│   ├── config.ini
│   └── icon.ico
├── docs/
│   ├── API_REFERENCE.md
│   └── USER_GUIDE.md
└── CMakeLists.txt
```

## Next Steps

1. Download Total Commander WFX Plugin SDK
2. Set up development environment (Visual Studio or MinGW)
3. Create basic plugin skeleton
4. Test with Total Commander
5. Implement restic command execution
6. Add snapshot browsing functionality
7. Implement file restoration and access

## Resources

- Total Commander WFX Plugin SDK: https://www.ghisler.com/sdk.htm
- Restic Documentation: https://restic.readthedocs.io/
- WFX Plugin Examples: Available in Total Commander SDK

## Security Considerations

- Never store passwords in plaintext
- Use Windows DPAPI for password encryption on Windows
- Implement secure memory handling for sensitive data
- Clear temporary files after use
- Validate all restic command outputs
- Implement timeout for long-running operations

## Performance Optimization

- Implement lazy loading for large directory trees
- Cache aggressively but with proper invalidation
- Use background threads for restic operations
- Implement progress callbacks for user feedback
- Consider pagination for very large snapshot lists

## Error Handling

- Gracefully handle missing restic binary
- Handle corrupted repositories
- Provide meaningful error messages to users
- Log errors for debugging
- Implement retry logic for network operations

## Future Enhancements

- Support for restic backup creation
- Snapshot comparison functionality
- Search within snapshots
- Mount snapshots as drive letters
- Integration with Windows Explorer context menu
- Multi-repository management
- Snapshot tagging and annotation
