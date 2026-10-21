#ifndef _SHELLFIND_PCH_
#define _SHELLFIND_PCH_

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#include <windef.h>
#include <winbase.h>
#include <shlobj.h>
#include <shlobj_undoc.h>
#include <shlguid_undoc.h>
#include <shdeprecated.h>
#include <tchar.h>
#include <atlbase.h>
#include <atlcom.h>
#include <atlwin.h>
#include <atlsimpcoll.h>
#include <atlstr.h>
#include <shlwapi.h>
#include <shlwapi_undoc.h>
#include <undocshell.h>
#include <shellutils.h>
#include <strsafe.h>
#include <wine/debug.h>

#include "../resource.h"

typedef struct tagLOCATIONITEM
{
    struct tagLOCATIONITEM *pNext;
    WCHAR szPath[ANYSIZE_ARRAY];
} LOCATIONITEM;

void FreeList(LOCATIONITEM *pLI);

struct ScopedFreeLocationItems
{
    LOCATIONITEM *m_ptr;
    ScopedFreeLocationItems(LOCATIONITEM *ptr) : m_ptr(ptr) {}
    ~ScopedFreeLocationItems() { FreeList(m_ptr); }
    void Detach() { m_ptr = NULL; }
};

struct SearchStart
{
    LOCATIONITEM *pPaths;
    WCHAR szFileName[MAX_PATH];
    WCHAR szQuery[MAX_PATH];
    BOOL  SearchHidden;
};

/*
 * ReactOS-private interface the search band uses to control the search results folder.
 * In-process only: it is never marshaled and is not a Windows interface. It replaces
 * window messages that carried raw pointers to the results window, which any process
 * on the desktop could post.
 */
// {31375A94-9FBB-40EA-AE48-323993D7B326}
static const IID IID_IROSFindFolderControl =
    { 0x31375a94, 0x9fbb, 0x40ea, { 0xae, 0x48, 0x32, 0x39, 0x93, 0xd7, 0xb3, 0x26 } };

struct IROSFindFolderControl : public IUnknown
{
    // Starts a new search, replacing any running one. The callee copies pRequest.
    STDMETHOD(BeginSearch)(_In_ const SearchStart *pRequest) PURE;
    // Cancels the running search, if any
    STDMETHOD(CancelSearch)() PURE;
};

template<class T, class F, class R>
static INT_PTR FindItemInComboEx(HWND hCombo, T &FindItem, F CompareFunc, R RetMatch)
{
    COMBOBOXEXITEMW item;
    item.mask = CBEIF_LPARAM;
    item.cchTextMax = 0;
    for (item.iItem = 0; SendMessageW(hCombo, CBEM_GETITEMW, 0, (LPARAM)&item); item.iItem++)
    {
        if (CompareFunc((T&)item.lParam, FindItem) == RetMatch)
            return item.iItem;
    }
    return -1;
}

static inline bool PathIsOnDrive(PCWSTR Path)
{
    return PathGetDriveNumberW(Path) >= 0 && (Path[2] == '\\' || !Path[2]);
}

static inline BOOL PathIsOnUnc(PCWSTR Path)
{
    return PathIsUNCW(Path); // FIXME: Verify the path starts with <\\Server\Share>[\]
}

static inline bool FindFiles_PathIsAbsolute(PCWSTR Path)
{
    // Note: PathIsRelativeW is too forgiving
    return PathIsOnDrive(Path) || PathIsOnUnc(Path);
}

#endif /* _SHELLFIND_PCH_ */
