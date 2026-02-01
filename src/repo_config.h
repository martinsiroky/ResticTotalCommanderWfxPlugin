#ifndef REPO_CONFIG_H
#define REPO_CONFIG_H

#include <windows.h>
#include "fsplugin.h"

#define MAX_REPOS 16
#define MAX_REPO_NAME 64
#define MAX_REPO_PATH 512
#define MAX_REPO_PASS 256

typedef struct {
    char name[MAX_REPO_NAME];       /* display name */
    char path[MAX_REPO_PATH];       /* restic repo path */
    char password[MAX_REPO_PASS];   /* in-memory only, never persisted */
    BOOL configured;                /* TRUE if this slot is active */
    BOOL hasPassword;               /* TRUE if password is cached in memory */
} RepoConfig;

typedef struct {
    RepoConfig repos[MAX_REPOS];
    int count;
    char configFilePath[MAX_PATH];
} RepoStore;

/* Global repo store */
extern RepoStore g_RepoStore;

/* Load repo config from INI file. Call once from FsInit. */
void RepoStore_Load(void);

/* Save repo config to INI file (names and paths only, no passwords). */
void RepoStore_Save(void);

/* Find a repo by name. Returns pointer or NULL. */
RepoConfig* RepoStore_FindByName(const char* name);

/* Prompt user to add a new repository using TC request dialogs.
   pluginNr: the plugin number from FsInit
   requestProc: the tRequestProc callback from FsInit
   Returns TRUE if a repo was successfully added. */
BOOL RepoStore_PromptAdd(int pluginNr, tRequestProc requestProc);

/* Prompt user for password if not already cached.
   Returns TRUE if password is available (already cached or just entered). */
BOOL RepoStore_EnsurePassword(RepoConfig* repo, int pluginNr, tRequestProc requestProc);

#endif /* REPO_CONFIG_H */
