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
    snprintf(cmdLine, sizeof(cmdLine), "restic -r \"%s\" --json %s", repoPath, args);

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

    /* Wait for process to finish (30 second timeout) */
    WaitForSingleObject(pi.hProcess, 30000);

    if (exitCode) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return buffer;
}
