#include "repo_config.h"
#include "restic_process.h"
#include <stdio.h>
#include <string.h>
#include <shlobj.h>

RepoStore g_RepoStore;

/* Build the config file path in %APPDATA%\TotalCmd\restic_wfx.ini */
static void BuildConfigPath(void) {
    char appData[MAX_PATH];

    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData) != S_OK) {
        /* Fallback: use current directory */
        strcpy(g_RepoStore.configFilePath, "restic_wfx.ini");
        return;
    }

    snprintf(g_RepoStore.configFilePath, MAX_PATH, "%s\\TotalCmd", appData);
    CreateDirectoryA(g_RepoStore.configFilePath, NULL); /* ensure dir exists */
    strncat(g_RepoStore.configFilePath, "\\restic_wfx.ini",
            MAX_PATH - strlen(g_RepoStore.configFilePath) - 1);
}

void RepoStore_Load(void) {
    char section[32];
    char buf[MAX_REPO_PATH];
    int i;

    memset(&g_RepoStore, 0, sizeof(g_RepoStore));
    BuildConfigPath();

    /* Read repo count */
    g_RepoStore.count = GetPrivateProfileIntA("General", "Count", 0,
                                               g_RepoStore.configFilePath);
    if (g_RepoStore.count > MAX_REPOS) {
        g_RepoStore.count = MAX_REPOS;
    }

    for (i = 0; i < g_RepoStore.count; i++) {
        snprintf(section, sizeof(section), "Repo%d", i);

        GetPrivateProfileStringA(section, "Name", "", g_RepoStore.repos[i].name,
                                  MAX_REPO_NAME, g_RepoStore.configFilePath);
        GetPrivateProfileStringA(section, "Path", "", g_RepoStore.repos[i].path,
                                  MAX_REPO_PATH, g_RepoStore.configFilePath);

        g_RepoStore.repos[i].configured =
            (g_RepoStore.repos[i].name[0] != '\0' &&
             g_RepoStore.repos[i].path[0] != '\0');
        g_RepoStore.repos[i].hasPassword = FALSE;
        g_RepoStore.repos[i].password[0] = '\0';
    }
}

void RepoStore_Save(void) {
    char section[32];
    char countStr[16];
    int i;

    snprintf(countStr, sizeof(countStr), "%d", g_RepoStore.count);
    WritePrivateProfileStringA("General", "Count", countStr,
                                g_RepoStore.configFilePath);

    for (i = 0; i < g_RepoStore.count; i++) {
        snprintf(section, sizeof(section), "Repo%d", i);
        WritePrivateProfileStringA(section, "Name", g_RepoStore.repos[i].name,
                                    g_RepoStore.configFilePath);
        WritePrivateProfileStringA(section, "Path", g_RepoStore.repos[i].path,
                                    g_RepoStore.configFilePath);
        /* Never persist password */
    }
}

RepoConfig* RepoStore_FindByName(const char* name) {
    int i;
    for (i = 0; i < g_RepoStore.count; i++) {
        if (g_RepoStore.repos[i].configured &&
            strcmp(g_RepoStore.repos[i].name, name) == 0) {
            return &g_RepoStore.repos[i];
        }
    }
    return NULL;
}

BOOL RepoStore_PromptAdd(int pluginNr, tRequestProc requestProc) {
    char repoPath[MAX_REPO_PATH];
    char repoName[MAX_REPO_NAME];
    char repoPass[MAX_REPO_PASS];
    RepoConfig* repo;
    DWORD exitCode;
    char* output;

    if (!requestProc) return FALSE;
    if (g_RepoStore.count >= MAX_REPOS) return FALSE;

    memset(repoPath, 0, sizeof(repoPath));
    memset(repoName, 0, sizeof(repoName));
    memset(repoPass, 0, sizeof(repoPass));

    /* Ask for repository path */
    if (!requestProc(pluginNr, RT_Other, "Add Repository",
                     "Enter restic repository path:", repoPath, MAX_REPO_PATH)) {
        return FALSE;
    }
    if (repoPath[0] == '\0') return FALSE;

    /* Ask for display name */
    if (!requestProc(pluginNr, RT_Other, "Repository Name",
                     "Enter a display name:", repoName, MAX_REPO_NAME)) {
        return FALSE;
    }
    if (repoName[0] == '\0') return FALSE;

    /* Check for duplicate name */
    if (RepoStore_FindByName(repoName)) {
        requestProc(pluginNr, RT_MsgOK, "Error",
                     "A repository with this name already exists.", repoName, MAX_REPO_NAME);
        return FALSE;
    }

    /* Ask for password */
    if (!requestProc(pluginNr, RT_Password, "Repository Password",
                     "Enter restic repository password:", repoPass, MAX_REPO_PASS)) {
        return FALSE;
    }

    /* Test connection by running restic snapshots */
    output = RunRestic(repoPath, repoPass, "snapshots", &exitCode);
    if (!output || exitCode != 0) {
        requestProc(pluginNr, RT_MsgOK, "Connection Failed",
                     "Could not connect to repository. Check path and password.",
                     repoPath, MAX_REPO_PATH);
        free(output);
        /* Clear password from stack */
        SecureZeroMemory(repoPass, sizeof(repoPass));
        return FALSE;
    }
    free(output);

    /* Add the new repo */
    repo = &g_RepoStore.repos[g_RepoStore.count];
    strncpy(repo->name, repoName, MAX_REPO_NAME - 1);
    strncpy(repo->path, repoPath, MAX_REPO_PATH - 1);
    strncpy(repo->password, repoPass, MAX_REPO_PASS - 1);
    repo->configured = TRUE;
    repo->hasPassword = TRUE;
    g_RepoStore.count++;

    /* Save to INI */
    RepoStore_Save();

    /* Clear password from stack */
    SecureZeroMemory(repoPass, sizeof(repoPass));

    return TRUE;
}

BOOL RepoStore_EnsurePassword(RepoConfig* repo, int pluginNr, tRequestProc requestProc) {
    char buf[MAX_REPO_PASS];

    if (!repo || !requestProc) return FALSE;

    /* Already have password in memory */
    if (repo->hasPassword && repo->password[0] != '\0') {
        return TRUE;
    }

    memset(buf, 0, sizeof(buf));
    if (!requestProc(pluginNr, RT_Password, "Repository Password",
                     "Enter restic repository password:", buf, MAX_REPO_PASS)) {
        return FALSE;
    }

    strncpy(repo->password, buf, MAX_REPO_PASS - 1);
    repo->hasPassword = TRUE;

    SecureZeroMemory(buf, sizeof(buf));
    return TRUE;
}
