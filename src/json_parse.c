#include "json_parse.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Convert UTF-8 string to the system ANSI codepage.
   Restic outputs JSON in UTF-8; the WFX ANSI API expects CP_ACP. */
void Utf8ToAnsi(const char* utf8, char* ansi, int ansiSize) {
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

/* Convert system ANSI codepage string to UTF-8.
   Inverse of Utf8ToAnsi: needed when passing paths back to restic. */
void AnsiToUtf8(const char* ansi, char* utf8, int utf8Size) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi, -1, NULL, 0);
    if (wlen > 0) {
        WCHAR* wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
        if (wbuf) {
            MultiByteToWideChar(CP_ACP, 0, ansi, -1, wbuf, wlen);
            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8, utf8Size, NULL, NULL);
            free(wbuf);
            return;
        }
    }
    /* Fallback: copy as-is */
    strncpy(utf8, ansi, utf8Size - 1);
    utf8[utf8Size - 1] = '\0';
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

/* Returns TRUE if entryPath is a direct child of parentDir.
   Both paths use forward slashes. */
static BOOL IsDirectChild(const char* entryPath, const char* parentDir, int parentLen) {
    const char* child;

    /* Root level: direct children are "/something" with no further slash */
    if (parentLen <= 1) {
        if (entryPath[0] != '/') return FALSE;
        return (strchr(entryPath + 1, '/') == NULL);
    }

    /* Must start with parentDir + "/" */
    if (strncmp(entryPath, parentDir, parentLen) != 0) return FALSE;
    if (entryPath[parentLen] != '/') return FALSE;

    /* The part after parentDir+"/" must contain no more slashes */
    child = entryPath + parentLen + 1;
    if (*child == '\0') return FALSE;  /* This IS the parent dir itself */
    return (strchr(child, '/') == NULL);
}

int ParseLsOutput(const char* ndjson, const char* parentPath, ResticLsEntry** outEntries) {
    ResticLsEntry* entries = NULL;
    int count = 0, capacity = 0;
    const char* lineStart;
    const char* lineEnd;
    int parentLen;

    if (!ndjson || !outEntries) return -1;
    *outEntries = NULL;

    parentLen = parentPath ? (int)strlen(parentPath) : 0;
    /* Remove trailing slash from parentPath if present */
    while (parentLen > 1 && parentPath[parentLen - 1] == '/') parentLen--;

    lineStart = ndjson;
    while (*lineStart) {
        char* lineBuf;
        cJSON* obj;
        int lineLen;

        lineEnd = strchr(lineStart, '\n');
        if (!lineEnd) lineEnd = lineStart + strlen(lineStart);
        lineLen = (int)(lineEnd - lineStart);

        /* Skip empty lines */
        if (lineLen == 0) {
            lineStart = lineEnd + (*lineEnd ? 1 : 0);
            continue;
        }

        lineBuf = (char*)malloc(lineLen + 1);
        if (!lineBuf) break;
        memcpy(lineBuf, lineStart, lineLen);
        lineBuf[lineLen] = '\0';

        obj = cJSON_Parse(lineBuf);
        free(lineBuf);

        if (obj) {
            const cJSON* nameItem = cJSON_GetObjectItemCaseSensitive(obj, "name");
            const cJSON* pathItem = cJSON_GetObjectItemCaseSensitive(obj, "path");
            const cJSON* typeItem = cJSON_GetObjectItemCaseSensitive(obj, "type");

            /* Skip snapshot summary line (has no "name" field) */
            if (cJSON_IsString(nameItem) && cJSON_IsString(pathItem) && cJSON_IsString(typeItem)) {
                /* Normalize path separators to forward slashes for comparison */
                char normalizedPath[MAX_PATH];
                char* np;
                strncpy(normalizedPath, pathItem->valuestring, MAX_PATH - 1);
                normalizedPath[MAX_PATH - 1] = '\0';
                for (np = normalizedPath; *np; np++) {
                    if (*np == '\\') *np = '/';
                }

                if (IsDirectChild(normalizedPath, parentPath, parentLen)) {
                    /* Grow array */
                    if (count >= capacity) {
                        capacity = (capacity == 0) ? 32 : (capacity * 2);
                        entries = (ResticLsEntry*)realloc(entries, sizeof(ResticLsEntry) * capacity);
                        if (!entries) { cJSON_Delete(obj); break; }
                    }

                    ResticLsEntry* e = &entries[count];
                    memset(e, 0, sizeof(ResticLsEntry));

                    Utf8ToAnsi(nameItem->valuestring, e->name, MAX_PATH);
                    strncpy(e->path, normalizedPath, MAX_PATH - 1);
                    strncpy(e->type, typeItem->valuestring, sizeof(e->type) - 1);

                    /* Size (may be absent for directories) */
                    const cJSON* sizeItem = cJSON_GetObjectItemCaseSensitive(obj, "size");
                    if (cJSON_IsNumber(sizeItem)) {
                        unsigned long long sz = (unsigned long long)sizeItem->valuedouble;
                        e->sizeLow = (DWORD)(sz & 0xFFFFFFFF);
                        e->sizeHigh = (DWORD)(sz >> 32);
                    }

                    /* Modification time */
                    const cJSON* mtimeItem = cJSON_GetObjectItemCaseSensitive(obj, "mtime");
                    if (cJSON_IsString(mtimeItem)) {
                        strncpy(e->mtime, mtimeItem->valuestring, sizeof(e->mtime) - 1);
                    }

                    count++;
                }
            }
            cJSON_Delete(obj);
        }

        lineStart = lineEnd + (*lineEnd ? 1 : 0);
    }

    *outEntries = entries;
    return count;
}
