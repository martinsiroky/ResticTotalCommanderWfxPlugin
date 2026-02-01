#include "restic_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* RunRestic(const char* repoPath, const char* password,
                const char* args, DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    char* buffer = NULL;
    DWORD bufSize = 4096;
    DWORD totalRead = 0;
    DWORD bytesRead;
    BOOL ok;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Build command line */
    snprintf(cmdLine, sizeof(cmdLine), "restic -r \"%s\" %s", repoPath, args);

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

    /* Create the restic process */
    ok = CreateProcessA(
        NULL,           /* lpApplicationName */
        cmdLine,        /* lpCommandLine */
        NULL,           /* lpProcessAttributes */
        NULL,           /* lpThreadAttributes */
        TRUE,           /* bInheritHandles */
        CREATE_NO_WINDOW, /* dwCreationFlags */
        NULL,           /* lpEnvironment (inherit current) */
        NULL,           /* lpCurrentDirectory */
        &si,
        &pi
    );

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

BOOL RunResticDump(const char* repoPath, const char* password,
                   const char* snapshotId, const char* filePath,
                   const char* outputPath, LONGLONG totalSize,
                   DumpProgressFunc progressCb, void* userData,
                   DWORD* exitCode) {
    SECURITY_ATTRIBUTES sa;
    HANDLE hReadPipe = NULL, hWritePipe = NULL;
    HANDLE hOutFile = INVALID_HANDLE_VALUE;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmdLine[2048];
    BYTE buf[65536];
    DWORD bytesRead, bytesWritten;
    LONGLONG totalWritten = 0;
    BOOL ok, aborted = FALSE;

    if (exitCode) *exitCode = (DWORD)-1;

    /* Build command line */
    snprintf(cmdLine, sizeof(cmdLine),
             "restic -r \"%s\" dump %s \"%s\"", repoPath, snapshotId, filePath);

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

    ok = CreateProcessA(NULL, cmdLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

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
