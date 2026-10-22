/*
 * PROJECT:     ReactOS Search Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Private protocol between the search band (browseui) and the
 *              search results folder (shell32)
 * COPYRIGHT:   Copyright 2019 Brock Mammen
 */

#pragma once

#define WM_SEARCH_START         (WM_USER + 0) // Must be ordered before any message with a payload
#define WM_SEARCH_STOP          (WM_USER + 1)
#define WM_SEARCH_ADD_RESULT    (WM_USER + 2)
#define WM_SEARCH_UPDATE_STATUS (WM_USER + 3)
#define WM_SEARCH_STOPPED       (WM_USER + 9) // Must be ordered after any message with a payload

typedef struct tagLOCATIONITEM
{
    struct tagLOCATIONITEM *pNext;
    WCHAR szPath[ANYSIZE_ARRAY];
} LOCATIONITEM;

static inline void FreeList(LOCATIONITEM *pItems)
{
    while (pItems)
    {
        LOCATIONITEM *pNext = pItems->pNext;
        CoTaskMemFree(pItems);
        pItems = pNext;
    }
}

struct SearchStart
{
    LOCATIONITEM *pPaths;
    WCHAR szFileName[MAX_PATH];
    WCHAR szQuery[MAX_PATH];
    BOOL  SearchHidden;
};
