/*
 * restic-wfx - Total Commander plugin for browsing restic backup repositories
 * Copyright (c) 2026 Martin Široký
 * SPDX-License-Identifier: MIT
 */

#include <windows.h>

/* Global module handle for finding the DLL's directory */
HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_hModule = hModule;
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
