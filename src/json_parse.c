#include "json_parse.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convert UTF-8 string to the system ANSI codepage.
   Restic outputs JSON in UTF-8; the WFX ANSI API expects CP_ACP. */
static void Utf8ToAnsi(const char* utf8, char* ansi, int ansiSize) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen > 0) {
        WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
        if (wbuf) {
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
            WideCharToMultiByte(CP_ACP, 0, wbuf, -1, ansi, ansiSize, NULL, NULL);
            free(wbuf);
            return;
        }
    }
    /* Fallback: copy as-is */
    strncpy(ansi, utf8, ansiSize - 1);
    ansi[ansiSize - 1] = '\0';
}

/* Compare snapshots by time descending (newest first) */
static int CompareSnapshotsDesc(const void* a, const void* b) {
    return strcmp(((const ResticSnapshot*)b)->time,
                  ((const ResticSnapshot*)a)->time);
}

FILETIME ParseISOTime(const char* timeStr) {
    SYSTEMTIME st;
    FILETIME ft;
    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0;

    memset(&st, 0, sizeof(st));
    memset(&ft, 0, sizeof(ft));

    if (timeStr && sscanf(timeStr, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hr, &mn, &sc) >= 3) {
        st.wYear = (WORD)yr;
        st.wMonth = (WORD)mo;
        st.wDay = (WORD)dy;
        st.wHour = (WORD)hr;
        st.wMinute = (WORD)mn;
        st.wSecond = (WORD)sc;
        SystemTimeToFileTime(&st, &ft);
    }

    return ft;
}

static const char* GetJsonString(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

int ParseSnapshots(const char* json, ResticSnapshot** outSnapshots) {
    cJSON* root = NULL;
    cJSON* item = NULL;
    int count, i;
    ResticSnapshot* snapshots = NULL;

    if (!json || !outSnapshots) return -1;
    *outSnapshots = NULL;

    root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }

    count = cJSON_GetArraySize(root);
    if (count == 0) {
        cJSON_Delete(root);
        return 0;
    }

    snapshots = (ResticSnapshot*)calloc(count, sizeof(ResticSnapshot));
    if (!snapshots) {
        cJSON_Delete(root);
        return -1;
    }

    i = 0;
    cJSON_ArrayForEach(item, root) {
        const char* id = GetJsonString(item, "id");
        const char* shortId = GetJsonString(item, "short_id");
        const char* time = GetJsonString(item, "time");
        const char* hostname = GetJsonString(item, "hostname");

        strncpy(snapshots[i].id, id, sizeof(snapshots[i].id) - 1);
        strncpy(snapshots[i].shortId, shortId, sizeof(snapshots[i].shortId) - 1);
        strncpy(snapshots[i].time, time, sizeof(snapshots[i].time) - 1);
        Utf8ToAnsi(hostname, snapshots[i].hostname, sizeof(snapshots[i].hostname));

        /* Store individual paths (converted from UTF-8 to ANSI) */
        snapshots[i].pathCount = 0;
        const cJSON* paths = cJSON_GetObjectItemCaseSensitive(item, "paths");
        if (cJSON_IsArray(paths)) {
            const cJSON* p;
            cJSON_ArrayForEach(p, paths) {
                if (snapshots[i].pathCount >= MAX_SNAP_PATHS) break;
                if (cJSON_IsString(p) && p->valuestring) {
                    Utf8ToAnsi(p->valuestring,
                               snapshots[i].paths[snapshots[i].pathCount], MAX_PATH);
                    snapshots[i].pathCount++;
                }
            }
        }

        i++;
    }

    cJSON_Delete(root);

    /* Sort newest-first by time (ISO 8601 is lexicographically sortable) */
    if (count > 1) {
        qsort(snapshots, count, sizeof(ResticSnapshot), CompareSnapshotsDesc);
    }

    *outSnapshots = snapshots;
    return count;
}
