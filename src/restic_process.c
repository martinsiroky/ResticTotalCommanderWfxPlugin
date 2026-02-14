/*
 * restic-wfx - Total Commander plugin for browsing restic backup repositories
 * Copyright (c) 2026 Martin Široký
 * SPDX-License-Identifier: MIT
 */

#include "restic_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <shlobj.h>

/* Convert a UTF-8 string to a malloc'd wide string. Caller must free. */
static WCHAR* Utf8ToWide(const char* utf8) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (wbuf) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    }
    return wbuf;
}

/* Command logging infrastructure */
static char g_LogFilePath[MAX_PATH] = {0};
static BOOL g_LogInitialized = FALSE;

/* Build log file path: %APPDATA%\GHISLER\plugins\wfx\restic_wfx\restic_commands.log */
static void EnsureLogPath(void) {
    char appData[MAX_PATH];
    char dir[MAX_PATH];

    if (g_LogInitialized) return;

    if (FAILED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        g_LogFilePath[0] = '\0';
        g_LogInitialized = TRUE;
        return;
    }

    /* Create intermediate directories */
    snprintf(dir, MAX_PATH, "%s\\GHISLER", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins\\wfx", appData);
    CreateDirectoryA(dir, NULL);
    snprintf(dir, MAX_PATH, "%s\\GHISLER\\plugins\\wfx\\restic_wfx", appData);
    CreateDirectoryA(dir, NULL);

    snprintf(g_LogFilePath, MAX_PATH,
             "%s\\GHISLER\\plugins\\wfx\\restic_wfx\\restic_commands.log", appData);
    g_LogInitialized = TRUE;
}

/* Log a restic command with timestamp. Appends to log file. */
static void LogResticCommand(const char* cmdLine) {
    FILE* f;
    time_t now;
    struct tm* tm_info;
    char timestamp[32];

    EnsureLogPath();
    if (g_LogFilePath[0] == '\0') return;

    f = fopen(g_LogFilePath, "a");
    if (!f) return;

    time(&now);
    tm_info = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "[%s] %s\n", timestamp, cmdLine);
    fclose(f);
}

char* RunResticWithProgress(const char* repoPath, const char* password,
                            const char* args, DWORD* exitCode,
                            ResticCancelFunc cancelCb, void* userData) {
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    WCHAR* wCmdLine = NULL;
    char* buffer = NULL;
    DWORD bufSize = 4096;
    DWORD totalRead = 0;
    DWORD bytesRead;
    BOOL ok;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Convert ANSI repo path to UTF-8 so the entire cmdLine is UTF-8 */
    char repoPathUtf8[MAX_PATH];
    {
        int wlen = MultiByteToWideChar(CP_ACP, 0, repoPath, -1, NULL, 0);
        if (wlen > 0) {
            WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
            if (wbuf) {
                MultiByteToWideChar(CP_ACP, 0, repoPath, -1, wbuf, wlen);
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, repoPathUtf8, MAX_PATH, NULL, NULL);
                free(wbuf);
            } else {
                strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
                repoPathUtf8[MAX_PATH - 1] = '\0';
            }
        } else {
            strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
            repoPathUtf8[MAX_PATH - 1] = '\0';
        }
    }

    /* Build command line (fully UTF-8, will be converted to wide) */
    snprintf(cmdLine, sizeof(cmdLine), "restic -r \"%s\" %s", repoPathUtf8, args);
    LogResticCommand(cmdLine);

    /* Create pipe for stdout capture */
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return NULL;
    }

    /* Prevent the read end from being inherited */
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Set RESTIC_PASSWORD environment variable */
    SetEnvironmentVariableA("RESTIC_PASSWORD", password);

    /* Set up process startup info */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    memset(&pi, 0, sizeof(pi));

    /* Convert UTF-8 command line to wide for CreateProcessW */
    wCmdLine = Utf8ToWide(cmdLine);
    if (!wCmdLine) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);
        return NULL;
    }

    /* Create the restic process (wide version for correct Unicode paths) */
    ok = CreateProcessW(
        NULL,           /* lpApplicationName */
        wCmdLine,       /* lpCommandLine */
        NULL,           /* lpProcessAttributes */
        NULL,           /* lpThreadAttributes */
        TRUE,           /* bInheritHandles */
        CREATE_NO_WINDOW, /* dwCreationFlags */
        NULL,           /* lpEnvironment (inherit current) */
        NULL,           /* lpCurrentDirectory */
        &si,
        &pi
    );

    free(wCmdLine);

    /* Clear the password from environment immediately */
    SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);

    /* Close write end in parent so ReadFile will eventually return 0 */
    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    if (!ok) {
        CloseHandle(hReadPipe);
        return NULL;
    }

    /* Read stdout into growing buffer */
    buffer = (char*)malloc(bufSize);
    if (!buffer) {
        CloseHandle(hReadPipe);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return NULL;
    }

    while (ReadFile(hReadPipe, buffer + totalRead, bufSize - totalRead - 1, &bytesRead, NULL)
           && bytesRead > 0) {
        totalRead += bytesRead;

        /* Check cancellation callback after each read chunk */
        if (cancelCb && !cancelCb(userData)) {
            free(buffer);
            CloseHandle(hReadPipe);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (exitCode) *exitCode = (DWORD)-1;
            return NULL;
        }

        if (totalRead + 1 >= bufSize) {
            bufSize *= 2;
            char* newBuf = (char*)realloc(buffer, bufSize);
            if (!newBuf) {
                free(buffer);
                CloseHandle(hReadPipe);
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return NULL;
            }
            buffer = newBuf;
        }
    }
    buffer[totalRead] = '\0';

    CloseHandle(hReadPipe);

    /* Wait for process to finish (120 second timeout for large snapshot listings) */
    WaitForSingleObject(pi.hProcess, 120000);

    if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return buffer;
}

char* RunRestic(const char* repoPath, const char* password,
                const char* args, DWORD* exitCode) {
    return RunResticWithProgress(repoPath, password, args, exitCode, NULL, NULL);
}

BOOL RunResticDump(const char* repoPath, const char* password,
                   const char* snapshotId, const char* filePath,
                   const char* outputPath, LONGLONG totalSize,
                   DumpProgressFunc progressCb, void* userData,
                   DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    HANDLE hOutFile = INVALID_HANDLE_VALUE;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    WCHAR* wCmdLine = NULL;
    BYTE buf[65536];
    DWORD bytesRead, bytesWritten;
    LONGLONG totalWritten = 0;
    BOOL ok, aborted = FALSE;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Convert ANSI repo path to UTF-8 so the entire cmdLine is UTF-8 */
    char repoPathUtf8[MAX_PATH];
    {
        int wlen = MultiByteToWideChar(CP_ACP, 0, repoPath, -1, NULL, 0);
        if (wlen > 0) {
            WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
            if (wbuf) {
                MultiByteToWideChar(CP_ACP, 0, repoPath, -1, wbuf, wlen);
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, repoPathUtf8, MAX_PATH, NULL, NULL);
                free(wbuf);
            } else {
                strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
                repoPathUtf8[MAX_PATH - 1] = '\0';
            }
        } else {
            strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
            repoPathUtf8[MAX_PATH - 1] = '\0';
        }
    }

    /* Build command line (fully UTF-8, will be converted to wide) */
    snprintf(cmdLine, sizeof(cmdLine),
             "restic -r \"%s\" dump %s \"%s\"", repoPathUtf8, snapshotId, filePath);
    LogResticCommand(cmdLine);

    /* Create pipe for stdout capture */
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return FALSE;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    /* Set RESTIC_PASSWORD environment variable */
    SetEnvironmentVariableA("RESTIC_PASSWORD", password);

    /* Set up process startup info */
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    memset(&pi, 0, sizeof(pi));

    /* Convert UTF-8 command line to wide for CreateProcessW */
    wCmdLine = Utf8ToWide(cmdLine);
    if (!wCmdLine) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);
        return FALSE;
    }

    ok = CreateProcessW(NULL, wCmdLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    free(wCmdLine);

    /* Clear password from environment immediately */
    SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);

    CloseHandle(hWritePipe);
    hWritePipe = NULL;

    if (!ok) {
        CloseHandle(hReadPipe);
        return FALSE;
    }

    /* Open output file */
    hOutFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOutFile == INVALID_HANDLE_VALUE) {
        CloseHandle(hReadPipe);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return FALSE;
    }

    /* Stream pipe to file */
    while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
        if (!WriteFile(hOutFile, buf, bytesRead, &bytesWritten, NULL)) {
            break;
        }
        totalWritten += bytesWritten;

        if (progressCb) {
            if (!progressCb(totalWritten, totalSize, userData)) {
                aborted = TRUE;
                break;
            }
        }
    }

    CloseHandle(hReadPipe);
    CloseHandle(hOutFile);

    if (aborted) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        DeleteFileA(outputPath);
        if (exitCode) *exitCode = (DWORD)-1;
        return FALSE;
    }

    /* Wait for process to finish (5 min timeout for large files) */
    WaitForSingleObject(pi.hProcess, 300000);

    if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* Check exit code — delete partial file on error */
    if (exitCode && *exitCode != 0) {
        DeleteFileA(outputPath);
        return FALSE;
    }

    return TRUE;
}

BOOL RunResticRestore(const char* repoPath, const char* password,
                      const char* snapshotId, const char* snapshotPath,
                      const char* includePath,
                      const char* targetDir, DWORD* exitCode) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    WCHAR* wCmdLine = NULL;
    BOOL ok;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Convert ANSI repo path to UTF-8 */
    char repoPathUtf8[MAX_PATH];
    {
        int wlen = MultiByteToWideChar(CP_ACP, 0, repoPath, -1, NULL, 0);
        if (wlen > 0) {
            WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
            if (wbuf) {
                MultiByteToWideChar(CP_ACP, 0, repoPath, -1, wbuf, wlen);
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, repoPathUtf8, MAX_PATH, NULL, NULL);
                free(wbuf);
            } else {
                strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
                repoPathUtf8[MAX_PATH - 1] = '\0';
            }
        } else {
            strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
            repoPathUtf8[MAX_PATH - 1] = '\0';
        }
    }

    /* Build command line */
    snprintf(cmdLine, sizeof(cmdLine),
             "restic -r \"%s\" restore %s --path \"%s\" --include \"%s\" --target \"%s\"",
             repoPathUtf8, snapshotId, snapshotPath, includePath, targetDir);
    LogResticCommand(cmdLine);

    /* Set RESTIC_PASSWORD environment variable */
    SetEnvironmentVariableA("RESTIC_PASSWORD", password);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    memset(&pi, 0, sizeof(pi));

    wCmdLine = Utf8ToWide(cmdLine);
    if (!wCmdLine) {
        SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);
        return FALSE;
    }

    ok = CreateProcessW(NULL, wCmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    free(wCmdLine);
    SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);

    if (!ok) return FALSE;

    /* Wait for restore to finish (10 min timeout for large trees) */
    WaitForSingleObject(pi.hProcess, 600000);

    if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode && *exitCode == 0) ? TRUE : (exitCode ? FALSE : TRUE);
}

BOOL RunResticRewrite(const char* repoPath, const char* password,
                      const char* snapshotPath, const char* excludePath,
                      DWORD* exitCode) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    WCHAR* wCmdLine = NULL;
    BOOL ok;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Convert ANSI repo path to UTF-8 */
    char repoPathUtf8[MAX_PATH];
    {
        int wlen = MultiByteToWideChar(CP_ACP, 0, repoPath, -1, NULL, 0);
        if (wlen > 0) {
            WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
            if (wbuf) {
                MultiByteToWideChar(CP_ACP, 0, repoPath, -1, wbuf, wlen);
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, repoPathUtf8, MAX_PATH, NULL, NULL);
                free(wbuf);
            } else {
                strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
                repoPathUtf8[MAX_PATH - 1] = '\0';
            }
        } else {
            strncpy(repoPathUtf8, repoPath, MAX_PATH - 1);
            repoPathUtf8[MAX_PATH - 1] = '\0';
        }
    }

    /* Build command line */
    snprintf(cmdLine, sizeof(cmdLine),
             "restic -r \"%s\" rewrite --exclude \"%s\" --path \"%s\" --forget",
             repoPathUtf8, excludePath, snapshotPath);
    LogResticCommand(cmdLine);

    /* Set RESTIC_PASSWORD environment variable */
    SetEnvironmentVariableA("RESTIC_PASSWORD", password);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    memset(&pi, 0, sizeof(pi));

    wCmdLine = Utf8ToWide(cmdLine);
    if (!wCmdLine) {
        SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);
        return FALSE;
    }

    ok = CreateProcessW(NULL, wCmdLine, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    free(wCmdLine);
    SetEnvironmentVariableA("RESTIC_PASSWORD", NULL);

    if (!ok) return FALSE;

    /* Wait for rewrite to finish (10 min timeout) */
    WaitForSingleObject(pi.hProcess, 600000);

    if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exitCode && *exitCode == 0) ? TRUE : (exitCode ? FALSE : TRUE);
}
