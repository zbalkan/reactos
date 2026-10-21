/*
 * PROJECT:     ReactOS Search Shell Extension
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Search results folder
 * COPYRIGHT:   Copyright 2019 Brock Mammen
 */

#include "CFindFolder.h"
#include <exdispid.h>

WINE_DEFAULT_DEBUG_CHANNEL(shellfind);

#ifndef _SHELL32_

static HRESULT WINAPI DisplayNameOfW(_In_ IShellFolder *psf, _In_ LPCITEMIDLIST pidl,
                                     _In_ DWORD dwFlags, _Out_ LPWSTR pszBuf, _In_ UINT cchBuf)
{
    *pszBuf = UNICODE_NULL;
    STRRET sr;
    HRESULT hr = psf->GetDisplayNameOf(pidl, dwFlags, &sr);
    return FAILED(hr) ? hr : StrRetToBufW(&sr, pidl, pszBuf, cchBuf);
}

static HRESULT
GetCommandStringA(_In_ IContextMenu *pCM, _In_ UINT_PTR Id, _In_ UINT GCS,
                  _Out_writes_(cchMax) LPSTR Buf, _In_ UINT cchMax)
{
    HRESULT hr = pCM->GetCommandString(Id, GCS & ~GCS_UNICODE, NULL, Buf, cchMax);
    if (FAILED(hr))
    {
        WCHAR buf[MAX_PATH];
        hr = pCM->GetCommandString(Id, GCS | GCS_UNICODE, NULL, (LPSTR)buf, _countof(buf));
        if (SUCCEEDED(hr))
            hr = SHUnicodeToAnsi(buf, Buf, cchMax) > 0 ? S_OK : E_FAIL;
    }
    return hr;
}

static HRESULT SHELL32_CoCreateInitSF(LPCITEMIDLIST pidlRoot, PERSIST_FOLDER_TARGET_INFO* ppfti,
                                LPCITEMIDLIST pidlChild, const GUID* clsid, REFIID riid, LPVOID *ppvOut)
{
    HRESULT hr;
    CComPtr<IShellFolder> pShellFolder;

    hr = SHCoCreateInstance(NULL, clsid, NULL, IID_PPV_ARG(IShellFolder, &pShellFolder));
    if (FAILED(hr))
        return hr;

    LPITEMIDLIST pidlAbsolute = ILCombine (pidlRoot, pidlChild);
    CComPtr<IPersistFolder> ppf;
    CComPtr<IPersistFolder3> ppf3;

    if (ppfti && SUCCEEDED(pShellFolder->QueryInterface(IID_PPV_ARG(IPersistFolder3, &ppf3))))
    {
        ppf3->InitializeEx(NULL, pidlAbsolute, ppfti);
    }
    else if (SUCCEEDED(pShellFolder->QueryInterface(IID_PPV_ARG(IPersistFolder, &ppf))))
    {
        ppf->Initialize(pidlAbsolute);
    }
    ILFree (pidlAbsolute);

    return pShellFolder->QueryInterface(riid, ppvOut);
}

#endif // _SHELL32_

static void WINAPI _InsertMenuItemW(
        HMENU hMenu,
        UINT indexMenu,
        BOOL fByPosition,
        UINT wID,
        UINT fType,
        LPCWSTR dwTypeData,
        UINT fState)
{
    MENUITEMINFOW mii;
    WCHAR wszText[100];

    ZeroMemory(&mii, sizeof(mii));
    mii.cbSize = sizeof(mii);
    if (fType == MFT_SEPARATOR)
        mii.fMask = MIIM_ID | MIIM_TYPE;
    else if (fType == MFT_STRING)
    {
        mii.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE;
        if (IS_INTRESOURCE(dwTypeData))
        {
            if (LoadStringW(_AtlBaseModule.GetResourceInstance(), LOWORD((ULONG_PTR)dwTypeData), wszText, _countof(wszText)))
                mii.dwTypeData = wszText;
            else
            {
                ERR("failed to load string %p\n", dwTypeData);
                return;
            }
        }
        else
            mii.dwTypeData = (LPWSTR)dwTypeData;
        mii.fState = fState;
    }

    mii.wID = wID;
    mii.fType = fType;
    InsertMenuItemW(hMenu, indexMenu, fByPosition, &mii);
}

static HRESULT QueryActiveShellView(IUnknown *pUnkSite, REFGUID rService, IShellView **ppSV)
{
    CComPtr<IShellBrowser> pSB;
    HRESULT hr = IUnknown_QueryService(pUnkSite, rService, IID_PPV_ARG(IShellBrowser, &pSB));
    return SUCCEEDED(hr) ? pSB->QueryActiveShellView(ppSV) : hr;
}

static HRESULT BeginRenameOfShellViewSelection(IUnknown *pUnkSite)
{
    CComPtr<IShellView> pSV;
    HRESULT hr = QueryActiveShellView(pUnkSite, SID_SShellBrowser, &pSV);
    if (FAILED(hr))
        return hr;
    CComPtr<IFolderView2> pFV2;
    if (SUCCEEDED(hr = pSV->QueryInterface(IID_PPV_ARG(IFolderView2, &pFV2))))
        return pFV2->DoRename();
    CComPtr<IShellView2> pSV2;
    if (SUCCEEDED(hr = pSV->QueryInterface(IID_PPV_ARG(IShellView2, &pSV2))))
        return pSV2->HandleRename(NULL);
    return hr;
}

struct FolderViewColumns
{
    int iResource;
    DWORD dwDefaultState;
    int fmt;
    int cxChar;
};

static const FolderViewColumns g_ColumnDefs[] =
{
    {IDS_COL_NAME,      SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 30},
    {IDS_COL_LOCATION,  SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT, 30},
    {IDS_COL_RELEVANCE, SHCOLSTATE_TYPE_STR,                          LVCFMT_LEFT, 0}
};

CFindFolder::CFindFolder() :
    m_pidl(NULL),
    m_pSearchData(NULL)
{
}

CFindFolder::~CFindFolder()
{
    EndSearch(FALSE);
    ILFree(m_pidl);
}

static LPITEMIDLIST _ILCreate(LPCWSTR lpszPath)
{
    CComHeapPtr<ITEMIDLIST> lpFSPidl(ILCreateFromPathW(lpszPath));
    if (!lpFSPidl)
    {
        ERR("Failed to create pidl from path\n");
        return NULL;
    }
    LPITEMIDLIST lpLastFSPidl = ILFindLastID(lpFSPidl);

    SIZE_T cbPath = (PathFindFileNameW(lpszPath) - lpszPath + 1) * sizeof(WCHAR);
    SIZE_T cbData = sizeof(WORD) + cbPath + lpLastFSPidl->mkid.cb;
    if (cbData > 0xffff)
        return NULL;
    LPITEMIDLIST pidl = (LPITEMIDLIST) SHAlloc(cbData + sizeof(WORD));
    if (!pidl)
        return NULL;

    LPBYTE p = (LPBYTE) pidl;
    p += sizeof(WORD); // mkid.cb

    PWSTR path = (PWSTR)p;
    memcpy(p, lpszPath, cbPath);
    p += cbPath;
    ((PWSTR)p)[-1] = UNICODE_NULL; // "C:\" not "C:" (required by ILCreateFromPathW and matches Windows)
    if (!PathIsRootW(path))
    {
        p -= sizeof(WCHAR);
        ((PWSTR)p)[-1] = UNICODE_NULL; // "C:\folder"
    }

    memcpy(p, lpLastFSPidl, lpLastFSPidl->mkid.cb);
    p += lpLastFSPidl->mkid.cb;

    pidl->mkid.cb = p - (LPBYTE)pidl;
    ((LPITEMIDLIST)p)->mkid.cb = 0; // Terminator
    return pidl;
}

static LPCWSTR _ILGetPath(LPCITEMIDLIST pidl)
{
    if (!pidl || !pidl->mkid.cb)
        return NULL;
    return (LPCWSTR) pidl->mkid.abID;
}

static LPCITEMIDLIST _ILGetFSPidl(LPCITEMIDLIST pidl)
{
    if (!pidl || !pidl->mkid.cb)
        return pidl;
    return (LPCITEMIDLIST) ((LPBYTE) pidl->mkid.abID
                            + ((wcslen((LPCWSTR) pidl->mkid.abID) + 1) * sizeof(WCHAR)));
}

static PIDLIST_ABSOLUTE _ILCreateAbsolute(LPCITEMIDLIST pidlChild)
{
    PIDLIST_ABSOLUTE pidl = NULL;
    if (PIDLIST_ABSOLUTE pidlFolder = SHSimpleIDListFromPath(_ILGetPath(pidlChild))) // FIXME: SHELL32_CreateSimpleIDListFromPath(, DIRECTORY)
    {
        pidl = ILCombine(pidlFolder, _ILGetFSPidl(pidlChild));
        ILFree(pidlFolder);
    }
    return pidl;
}

HRESULT CFindFolder::GetFSFolderAndChild(LPCITEMIDLIST pidl, IShellFolder **ppSF, PCUITEMID_CHILD *ppidlLast)
{
    ATLASSERT(m_pSfDesktop);
    PCWSTR path = _ILGetPath(pidl);
    if (!path || !path[0])
        return E_INVALIDARG;
    PIDLIST_ABSOLUTE pidlFolder = ILCreateFromPathW(path); // FIXME: SHELL32_CreateSimpleIDListFromPath(, DIRECTORY);
    if (!pidlFolder)
        return E_FAIL;
    HRESULT hr = m_pSfDesktop->BindToObject(pidlFolder, NULL, IID_PPV_ARG(IShellFolder, ppSF));
    ILFree(pidlFolder);
    if (ppidlLast)
        *ppidlLast = _ILGetFSPidl(pidl);
    return hr;
}

HRESULT CFindFolder::GetFSFolder2AndChild(LPCITEMIDLIST pidl, IShellFolder2 **ppSF, PCUITEMID_CHILD *ppidlLast)
{
    CComPtr<IShellFolder> pSF1;
    HRESULT hr = GetFSFolderAndChild(pidl, &pSF1, ppidlLast);
    if (SUCCEEDED(hr))
        hr = pSF1->QueryInterface(IID_PPV_ARG(IShellFolder2, ppSF));
    return hr;
}

static int CALLBACK ILFreeHelper(void *pItem, void *pCaller)
{
    ILFree((LPITEMIDLIST)pItem);
    return TRUE;
}

void CFindFolder::FreePidlArray(HDPA hDpa)
{
    DPA_DestroyCallback(hDpa, ILFreeHelper, NULL);
}

HDPA CFindFolder::CreateAbsolutePidlArray(UINT cidl, PCUITEMID_CHILD_ARRAY apidl)
{
    HDPA hDpa = DPA_Create(cidl / 2);
    if (hDpa)
    {
        for (UINT i = 0; i < cidl; ++i)
        {
            PIDLIST_ABSOLUTE pidl = _ILCreateAbsolute(apidl[i]);
            if (pidl)
            {
                if (DPA_InsertPtr(hDpa, i, pidl) >= 0)
                    continue;
                ILFree(pidl);
            }
            FreePidlArray(hDpa);
            return NULL;
        }
    }
    return hDpa;
}

/*
 * One search session. The results folder holds a reference to its active session
 * and the worker thread holds another; the session never references the folder.
 * The worker only queues results and status text here and posts a wake-up message
 * without payload; the owner thread drains the queue. The last Release frees
 * everything, so neither side depends on the other's lifetime.
 */
struct _SearchData
{
    HWND hwnd;              // Window to wake up; never used to pass data
    UINT uWakeMsg;          // Registered on the owner thread before the worker starts
    HANDLE hStopEvent;      // Owned by the session
    LOCATIONITEM *pPaths;
    CStringW szFileName;
    CStringA szQueryA;
    CStringW szQueryW;
    CStringW szQueryU16BE;
    CStringA szQueryU8;
    BOOL SearchHidden;
    UINT SearchId;

    // Protected by Lock
    CRITICAL_SECTION Lock;
    CSimpleArray<LPWSTR> Results;
    LPWSTR pszStatus;
    BOOL bDone;
    UINT uTotalFound;

    LONG fWakePending;
    LONG cRef;

    explicit _SearchData(UINT InitSearchId) :
        hwnd(NULL),
        uWakeMsg(0),
        hStopEvent(NULL),
        pPaths(NULL),
        SearchHidden(FALSE),
        SearchId(InitSearchId),
        pszStatus(NULL),
        bDone(FALSE),
        uTotalFound(0),
        fWakePending(FALSE),
        cRef(1)
    {
        InitializeCriticalSection(&Lock);
    }

    ~_SearchData()
    {
        FreeList(pPaths);
        for (int i = 0; i < Results.GetSize(); ++i)
            SHFree(Results[i]);
        SHFree(pszStatus);
        if (hStopEvent)
            CloseHandle(hStopEvent);
        DeleteCriticalSection(&Lock);
    }

    void AddRef()
    {
        InterlockedIncrement(&cRef);
    }

    void Release()
    {
        if (!InterlockedDecrement(&cRef))
            delete this;
    }

    BOOL IsCanceled() const
    {
        return WaitForSingleObject(hStopEvent, 0) != WAIT_TIMEOUT;
    }
};

// Only called on the thread that owns the results view; the worker uses _SearchData::uWakeMsg
UINT CFindFolder::GetSearchUpdateMessage()
{
    static UINT s_uMsg = 0;
    if (!s_uMsg)
        s_uMsg = RegisterWindowMessageW(L"ReactOS.FindFolder.SearchUpdate");
    return s_uMsg;
}

// Paths longer than MAX_PATH cannot be searched, so they are rejected as invalid
static HRESULT DuplicateLocationList(const LOCATIONITEM *pItems, LOCATIONITEM **ppList)
{
    LOCATIONITEM *pHead = NULL, **ppNext = &pHead;
    *ppList = NULL;
    for (; pItems; pItems = pItems->pNext)
    {
        size_t cch;
        if (FAILED(StringCchLengthW(pItems->szPath, MAX_PATH, &cch)))
        {
            FreeList(pHead);
            return E_INVALIDARG;
        }
        cch++;
        SIZE_T cbItem = FIELD_OFFSET(LOCATIONITEM, szPath) + cch * sizeof(WCHAR);
        LOCATIONITEM *pItem = (LOCATIONITEM *)CoTaskMemAlloc(cbItem);
        if (!pItem)
        {
            FreeList(pHead);
            return E_OUTOFMEMORY;
        }
        pItem->pNext = NULL;
        CopyMemory(pItem->szPath, pItems->szPath, cch * sizeof(WCHAR));
        *ppNext = pItem;
        ppNext = &pItem->pNext;
    }
    *ppList = pHead;
    return S_OK;
}

template<typename TChar, typename TString, int (&StrNCmp)(const TChar *, const TChar *, size_t)>
static const TChar* StrStrN(const TChar *lpFirst, const TString &lpSrch, UINT cchMax)
{
    UINT cchSrch = lpSrch.GetLength();
    if (!lpFirst || !cchSrch || cchMax < cchSrch)
        return NULL;

    // Only compare where the whole search string fits in the remaining buffer
    for (UINT i = cchMax - cchSrch + 1; i > 0 && *lpFirst; i--, lpFirst++)
    {
        if (!StrNCmp(lpFirst, lpSrch, cchSrch))
            return (const TChar*)lpFirst;
    }

    return NULL;
}

static inline BOOL
StrFindNIA(const CHAR *lpFirst, const CStringA &lpSrch, UINT cchMax)
{
    return StrStrN<CHAR, CStringA, _strnicmp>(lpFirst, lpSrch, cchMax) != NULL;
}

static inline BOOL
StrFindNIW(const WCHAR *lpFirst, const CStringW &lpSrch, UINT cchMax)
{
    return StrStrN<WCHAR, CStringW, _wcsnicmp>(lpFirst, lpSrch, cchMax) != NULL;
}

/*
 * The following code is borrowed from base/applications/cmdutils/more/more.c .
 */
typedef enum
{
    ENCODING_ANSI    =  0,
    ENCODING_UTF16LE =  1,
    ENCODING_UTF16BE =  2,
    ENCODING_UTF8    =  3
} ENCODING;

static BOOL
IsDataUnicode(
    IN PVOID Buffer,
    IN DWORD BufferSize,
    OUT ENCODING* Encoding OPTIONAL,
    OUT PDWORD SkipBytes OPTIONAL)
{
    PBYTE pBytes = (PBYTE)Buffer;
    ENCODING encFile = ENCODING_ANSI;
    DWORD dwPos = 0;

    /*
     * See http://archives.miloush.net/michkap/archive/2007/04/22/2239345.html
     * for more details about the algorithm and the pitfalls behind it.
     * Of course it would be actually great to make a nice function that
     * would work, once and for all, and put it into a library.
     */

    /* Look for Byte Order Marks */
    if ((BufferSize >= 2) && (pBytes[0] == 0xFF) && (pBytes[1] == 0xFE))
    {
        encFile = ENCODING_UTF16LE;
        dwPos = 2;
    }
    else if ((BufferSize >= 2) && (pBytes[0] == 0xFE) && (pBytes[1] == 0xFF))
    {
        encFile = ENCODING_UTF16BE;
        dwPos = 2;
    }
    else if ((BufferSize >= 3) && (pBytes[0] == 0xEF) && (pBytes[1] == 0xBB) && (pBytes[2] == 0xBF))
    {
        encFile = ENCODING_UTF8;
        dwPos = 3;
    }
    else
    {
        /*
         * Try using statistical analysis. Do not rely on the return value of
         * IsTextUnicode as we can get FALSE even if the text is in UTF-16 BE
         * (i.e. we have some of the IS_TEXT_UNICODE_REVERSE_MASK bits set).
         * Instead, set all the tests we want to perform, then just check
         * the passed tests and try to deduce the string properties.
         */

/*
 * This mask contains the 3 highest bits from IS_TEXT_UNICODE_NOT_ASCII_MASK
 * and the 1st highest bit from IS_TEXT_UNICODE_NOT_UNICODE_MASK.
 */
#define IS_TEXT_UNKNOWN_FLAGS_MASK  ((7 << 13) | (1 << 11))

        /* Flag out the unknown flags here, the passed tests will not have them either */
        INT Tests = (IS_TEXT_UNICODE_NOT_ASCII_MASK   |
                     IS_TEXT_UNICODE_NOT_UNICODE_MASK |
                     IS_TEXT_UNICODE_REVERSE_MASK | IS_TEXT_UNICODE_UNICODE_MASK)
                        & ~IS_TEXT_UNKNOWN_FLAGS_MASK;
        INT Results;

        IsTextUnicode(Buffer, BufferSize, &Tests);
        Results = Tests;

        /*
         * As the IS_TEXT_UNICODE_NULL_BYTES or IS_TEXT_UNICODE_ILLEGAL_CHARS
         * flags are expected to be potentially present in the result without
         * modifying our expectations, filter them out now.
         */
        Results &= ~(IS_TEXT_UNICODE_NULL_BYTES | IS_TEXT_UNICODE_ILLEGAL_CHARS);

        /*
         * NOTE: The flags IS_TEXT_UNICODE_ASCII16 and
         * IS_TEXT_UNICODE_REVERSE_ASCII16 are not reliable.
         *
         * NOTE2: Check for potential "bush hid the facts" effect by also
         * checking the original results (in 'Tests') for the absence of
         * the IS_TEXT_UNICODE_NULL_BYTES flag, as we may presumably expect
         * that in UTF-16 text there will be at some point some NULL bytes.
         * If not, fall back to ANSI. This shows the limitations of using the
         * IsTextUnicode API to perform such tests, and the usage of a more
         * improved encoding detection algorithm would be really welcome.
         */
        if (!(Results & IS_TEXT_UNICODE_NOT_UNICODE_MASK) &&
            !(Results & IS_TEXT_UNICODE_REVERSE_MASK)     &&
             (Results & IS_TEXT_UNICODE_UNICODE_MASK)     &&
             (Tests   & IS_TEXT_UNICODE_NULL_BYTES))
        {
            encFile = ENCODING_UTF16LE;
            dwPos = (Results & IS_TEXT_UNICODE_SIGNATURE) ? 2 : 0;
        }
        else
        if (!(Results & IS_TEXT_UNICODE_NOT_UNICODE_MASK) &&
            !(Results & IS_TEXT_UNICODE_UNICODE_MASK)     &&
             (Results & IS_TEXT_UNICODE_REVERSE_MASK)     &&
             (Tests   & IS_TEXT_UNICODE_NULL_BYTES))
        {
            encFile = ENCODING_UTF16BE;
            dwPos = (Results & IS_TEXT_UNICODE_REVERSE_SIGNATURE) ? 2 : 0;
        }
        else
        {
            /*
             * Either 'Results' has neither of those masks set, as it can be
             * the case for UTF-8 text (or ANSI), or it has both as can be the
             * case when analysing pure binary data chunk. This is therefore
             * invalid and we fall back to ANSI encoding.
             * FIXME: In case of failure, assume ANSI (as long as we do not have
             * correct tests for UTF8, otherwise we should do them, and at the
             * very end, assume ANSI).
             */
            encFile = ENCODING_ANSI; // ENCODING_UTF8;
            dwPos = 0;
        }
    }

    if (Encoding)
        *Encoding = encFile;
    if (SkipBytes)
        *SkipBytes = dwPos;

    return (encFile != ENCODING_ANSI);
}

static BOOL SearchFile(LPCWSTR lpFilePath, _SearchData *pSearchData)
{
    HANDLE hFile = CreateFileW(lpFilePath, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return FALSE;

    // FIXME: Search files of 4 GiB and larger in chunks instead of skipping them
    LARGE_INTEGER FileSize;
    if (!GetFileSizeEx(hFile, &FileSize) || FileSize.QuadPart == 0 || FileSize.HighPart != 0)
    {
        CloseHandle(hFile);
        return FALSE;
    }
    DWORD size = FileSize.LowPart;

    HANDLE hFileMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, size, NULL);
    CloseHandle(hFile);
    if (!hFileMap)
        return FALSE;

    LPBYTE pbContents = (LPBYTE)MapViewOfFile(hFileMap, FILE_MAP_READ, 0, 0, size);
    CloseHandle(hFileMap);
    if (!pbContents)
        return FALSE;

    ENCODING encoding;
    IsDataUnicode(pbContents, size, &encoding, NULL);

    BOOL bFound;
    switch (encoding)
    {
        case ENCODING_UTF16LE:
            // UTF-16
            bFound = StrFindNIW((LPCWSTR)pbContents, pSearchData->szQueryW, size / sizeof(WCHAR));
            break;
        case ENCODING_UTF16BE:
            // UTF-16 BE
            bFound = StrFindNIW((LPCWSTR)pbContents, pSearchData->szQueryU16BE, size / sizeof(WCHAR));
            break;
        case ENCODING_UTF8:
            // UTF-8
            bFound = StrFindNIA((LPCSTR)pbContents, pSearchData->szQueryU8, size / sizeof(CHAR));
            break;
        case ENCODING_ANSI:
        default:
            // ANSI or UTF-8 without BOM
            bFound = StrFindNIA((LPCSTR)pbContents, pSearchData->szQueryA, size / sizeof(CHAR));
            if (!bFound && pSearchData->szQueryA != pSearchData->szQueryU8)
                bFound = StrFindNIA((LPCSTR)pbContents, pSearchData->szQueryU8, size / sizeof(CHAR));
            break;
    }

    UnmapViewOfFile(pbContents);
    return bFound;
}

static BOOL FileNameMatch(LPCWSTR FindDataFileName, _SearchData *pSearchData)
{
    if (pSearchData->szFileName.IsEmpty() || PathMatchSpecW(FindDataFileName, pSearchData->szFileName))
    {
        return TRUE;
    }
    return FALSE;
}

static BOOL ContentsMatch(LPCWSTR szPath, _SearchData *pSearchData)
{
    if (pSearchData->szQueryA.IsEmpty() || SearchFile(szPath, pSearchData))
    {
        return TRUE;
    }
    return FALSE;
}

static BOOL AttribHiddenMatch(DWORD FileAttributes, _SearchData *pSearchData)
{
    if (!(FileAttributes & FILE_ATTRIBUTE_HIDDEN) || (pSearchData->SearchHidden))
    {
        return TRUE;
    }
    return FALSE;
}

// Called on the worker thread. Posts one wake-up at a time; the message carries no data.
static void WakeOwner(_SearchData *pSearchData)
{
    if (!InterlockedExchange(&pSearchData->fWakePending, TRUE))
    {
        if (!PostMessageW(pSearchData->hwnd, pSearchData->uWakeMsg, pSearchData->SearchId, 0))
            InterlockedExchange(&pSearchData->fWakePending, FALSE);
    }
}

static void QueueResult(_SearchData *pSearchData, LPCWSTR pszPath)
{
    LPWSTR pszDup;
    if (FAILED(SHStrDupW(pszPath, &pszDup)))
        return;

    EnterCriticalSection(&pSearchData->Lock);
    BOOL bAdded = pSearchData->Results.Add(pszDup);
    LeaveCriticalSection(&pSearchData->Lock);

    if (!bAdded)
        SHFree(pszDup);
    else
        WakeOwner(pSearchData);
}

// Only the latest status text is kept
static void QueueStatus(_SearchData *pSearchData, LPCWSTR pszStatus)
{
    LPWSTR pszDup;
    if (FAILED(SHStrDupW(pszStatus, &pszDup)))
        return;

    EnterCriticalSection(&pSearchData->Lock);
    LPWSTR pszOld = pSearchData->pszStatus;
    pSearchData->pszStatus = pszDup;
    LeaveCriticalSection(&pSearchData->Lock);

    SHFree(pszOld);
    WakeOwner(pSearchData);
}

static UINT RecursiveFind(LPCWSTR lpPath, _SearchData *pSearchData)
{
    if (pSearchData->IsCanceled())
        return 0;

    WCHAR szPath[MAX_PATH];
    WIN32_FIND_DATAW FindData;
    HANDLE hFindFile;
    BOOL bMoreFiles = TRUE;
    UINT uTotalFound = 0;

    if (!PathCombineW(szPath, lpPath, L"*"))
        return 0;

    for (hFindFile = FindFirstFileW(szPath, &FindData);
        bMoreFiles && hFindFile != INVALID_HANDLE_VALUE;
        bMoreFiles = FindNextFileW(hFindFile, &FindData))
    {
        // Check for cancellation on every entry, not only per directory
        if (pSearchData->IsCanceled())
            break;

#define IS_DOTS(psz) ((psz)[0] == L'.' && ((psz)[1] == 0 || ((psz)[1] == L'.' && (psz)[2] == 0)))
        if (IS_DOTS(FindData.cFileName))
            continue;

        // Skip entries whose full path does not fit
        if (!PathCombineW(szPath, lpPath, FindData.cFileName))
            continue;

        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            CStringW status;
            if (pSearchData->szQueryW.IsEmpty() &&
                FileNameMatch(FindData.cFileName, pSearchData) &&
                AttribHiddenMatch(FindData.dwFileAttributes, pSearchData))
            {
                QueueResult(pSearchData, szPath);
                uTotalFound++;
            }
            // Do not follow junctions and directory symbolic links: they can point to an
            // ancestor and make the search loop forever
            if (!(FindData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                status.Format(IDS_SEARCH_FOLDER, FindData.cFileName);
                QueueStatus(pSearchData, status);

                uTotalFound += RecursiveFind(szPath, pSearchData);
            }
        }
        else if (FileNameMatch(FindData.cFileName, pSearchData)
                && AttribHiddenMatch(FindData.dwFileAttributes, pSearchData)
                && !pSearchData->IsCanceled()
                && ContentsMatch(szPath, pSearchData))
        {
            QueueResult(pSearchData, szPath);
            uTotalFound++;
        }
    }

    if (hFindFile != INVALID_HANDLE_VALUE)
        FindClose(hFindFile);

    return uTotalFound;
}

struct SearchThreadParams
{
    _SearchData *pSearchData;
    HMODULE hModule; // Keeps this DLL loaded until the thread exits
};

DWORD WINAPI CFindFolder::SearchThreadProc(LPVOID lpParameter)
{
    SearchThreadParams *pParams = static_cast<SearchThreadParams*>(lpParameter);
    _SearchData *data = pParams->pSearchData;
    HMODULE hModule = pParams->hModule;
    delete pParams;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    UINT uTotalFound = 0;
    for (LOCATIONITEM *pLocation = data->pPaths; pLocation; pLocation = pLocation->pNext)
    {
        if (data->IsCanceled())
            break;
        uTotalFound += RecursiveFind(pLocation->szPath, data);
    }

    EnterCriticalSection(&data->Lock);
    data->bDone = TRUE;
    data->uTotalFound = uTotalFound;
    LeaveCriticalSection(&data->Lock);
    WakeOwner(data);

    data->Release();

    if (hModule)
        FreeLibraryAndExitThread(hModule, 0);
    return 0;
}

// Must only be called on the thread that owns the results view
void CFindFolder::NotifyConnections(DISPID id)
{
    DISPPARAMS dispatchParams = {0};
    CComDynamicUnkArray &subscribers =
        IConnectionPointImpl<CFindFolder, &DIID_DSearchCommandEvents>::m_vec;
    for (IUnknown** pSubscriber = subscribers.begin(); pSubscriber < subscribers.end(); pSubscriber++)
    {
        if (!*pSubscriber)
            continue;

        CComPtr<IDispatch> pDispatch;
        HRESULT hResult = (*pSubscriber)->QueryInterface(IID_PPV_ARG(IDispatch, &pDispatch));
        if (!FAILED_UNEXPECTEDLY(hResult))
            pDispatch->Invoke(id, GUID_NULL, 0, DISPATCH_METHOD, &dispatchParams, NULL, NULL, NULL);
    }
}

void CFindFolder::EndSearch(BOOL bNotify)
{
    _SearchData *pSearchData = m_pSearchData;
    if (!pSearchData)
        return;

    m_pSearchData = NULL;
    SetEvent(pSearchData->hStopEvent);
    pSearchData->Release();

    if (bNotify)
        NotifyConnections(DISPID_SEARCHABORT);
}

// *** IROSFindFolderControl methods ***
STDMETHODIMP CFindFolder::BeginSearch(const SearchStart *pSearchParams)
{
    HKEY hkey;
    DWORD size = sizeof(DWORD);
    DWORD result;
    DWORD SearchHiddenValue = 0;

    if (!pSearchParams || !pSearchParams->pPaths)
        return E_INVALIDARG;
    if (FAILED(StringCchLengthW(pSearchParams->szFileName, MAX_PATH, NULL)) ||
        FAILED(StringCchLengthW(pSearchParams->szQuery, MAX_PATH, NULL)))
    {
        return E_INVALIDARG;
    }

    // The caller keeps ownership of its request; take a copy of everything we need
    LOCATIONITEM *pPaths;
    HRESULT hr = DuplicateLocationList(pSearchParams->pPaths, &pPaths);
    if (FAILED(hr))
        return hr;
    if (!m_hWnd || !m_shellFolderView)
    {
        FreeList(pPaths);
        return E_UNEXPECTED;
    }

    // A new search replaces the previous one
    EndSearch(FALSE);

    // Clear all previous search results
    UINT uItemIndex;
    m_shellFolderView->RemoveObject(NULL, &uItemIndex);

    _SearchData* pSearchData = new _SearchData(++m_SearchId);
    if (!pSearchData)
    {
        FreeList(pPaths);
        return E_OUTOFMEMORY;
    }
    pSearchData->hwnd = m_hWnd;
    pSearchData->uWakeMsg = GetSearchUpdateMessage();
    pSearchData->pPaths = pPaths;
    pSearchData->hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!pSearchData->hStopEvent)
    {
        pSearchData->Release();
        return E_OUTOFMEMORY;
    }
    pSearchData->szFileName = pSearchParams->szFileName;
    pSearchData->szQueryA = pSearchParams->szQuery;
    pSearchData->szQueryW = pSearchParams->szQuery;

    // UTF-16 BE
    {
        CStringW utf16 = pSearchData->szQueryW;
        LPWSTR psz = utf16.GetBuffer();
        for (SIZE_T i = 0; psz[i]; ++i)
        {
            psz[i] = MAKEWORD(HIBYTE(psz[i]), LOBYTE(psz[i]));
        }
        utf16.ReleaseBuffer();
        pSearchData->szQueryU16BE = utf16;
    }

    // UTF-8
    {
        CStringA utf8;
        INT cch = WideCharToMultiByte(CP_UTF8, 0, pSearchData->szQueryW, -1, NULL, 0, NULL, NULL);
        if (cch > 0)
        {
            LPSTR psz = utf8.GetBuffer(cch);
            WideCharToMultiByte(CP_UTF8, 0, pSearchData->szQueryW, -1, psz, cch, NULL, NULL);
            utf8.ReleaseBuffer();
            pSearchData->szQueryU8 = utf8;
        }
        else
        {
            pSearchData->szQueryU8 = pSearchData->szQueryA;
        }
    }

    pSearchData->SearchHidden = pSearchParams->SearchHidden;

    TRACE("pSearchParams->SearchHidden is '%d'.\n", pSearchData->SearchHidden);

    if (pSearchData->SearchHidden)
        SearchHiddenValue = 1;
    else
        SearchHiddenValue = 0;

    /* Placing the code to save the changed settings to the registry here has the effect of not saving any changes */
    /* to the registry unless the user clicks on the "Search" button. This is the same as what we see in Windows.  */
    result = RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", 0, KEY_SET_VALUE, &hkey);
    if (result == ERROR_SUCCESS)
    {
        if (RegSetValueExW(hkey, L"SearchHidden", NULL, REG_DWORD, (const BYTE*)&SearchHiddenValue, size) == ERROR_SUCCESS)
        {
            TRACE("SearchHidden is '%d'.\n", SearchHiddenValue);
        }
        else
        {
            ERR("RegSetValueEx for \"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SearchHidden\" Failed.\n");
        }
        RegCloseKey(hkey);
    }
    else
    {
        ERR("RegOpenKey for \"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\" Failed.\n");
    }

    SearchThreadParams *pParams = new SearchThreadParams;
    if (!pParams)
    {
        pSearchData->Release();
        return E_OUTOFMEMORY;
    }
    pParams->pSearchData = pSearchData;
    pParams->hModule = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCWSTR)&CFindFolder::SearchThreadProc, &pParams->hModule);

    // The worker thread gets its own reference.
    // Keep the flags at 0: the worker exits through FreeLibraryAndExitThread, which skips the
    // cleanup SHCreateThread runs after the thread procedure for CTF_THREAD_REF,
    // CTF_PROCESS_REF and CTF_COINIT.
    pSearchData->AddRef();
    if (!SHCreateThread(SearchThreadProc, pParams, 0, NULL))
    {
        if (pParams->hModule)
            FreeLibrary(pParams->hModule);
        delete pParams;
        pSearchData->Release();
        pSearchData->Release();
        return E_FAIL;
    }

    m_pSearchData = pSearchData;
    NotifyConnections(DISPID_SEARCHSTART);
    return S_OK;
}

STDMETHODIMP CFindFolder::CancelSearch()
{
    EndSearch(TRUE);
    return S_OK;
}

LRESULT CFindFolder::OnSearchUpdate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL &bHandled)
{
    // The message carries no data. Ignore it unless it belongs to the active search.
    _SearchData *pSearchData = m_pSearchData;
    if (!pSearchData || wParam != pSearchData->SearchId)
        return 0;

    // Keep the session alive while calling out; a sink may end the search meanwhile
    pSearchData->AddRef();
    InterlockedExchange(&pSearchData->fWakePending, FALSE);

    CSimpleArray<LPWSTR> Results;
    EnterCriticalSection(&pSearchData->Lock);
    for (int i = 0; i < pSearchData->Results.GetSize(); ++i)
    {
        if (!Results.Add(pSearchData->Results[i]))
            SHFree(pSearchData->Results[i]);
    }
    pSearchData->Results.RemoveAll();
    LPWSTR pszStatus = pSearchData->pszStatus;
    pSearchData->pszStatus = NULL;
    BOOL bDone = pSearchData->bDone;
    UINT uTotalFound = pSearchData->uTotalFound;
    LeaveCriticalSection(&pSearchData->Lock);

    for (int i = 0; i < Results.GetSize(); ++i)
    {
        CComHeapPtr<ITEMIDLIST> lpSearchPidl(_ILCreate(Results[i]));
        if (lpSearchPidl && m_shellFolderView && m_pSearchData == pSearchData)
        {
            UINT uItemIndex;
            m_shellFolderView->AddObject(lpSearchPidl, &uItemIndex);
        }
        SHFree(Results[i]);
    }

    if (m_pSearchData == pSearchData)
    {
        if (bDone)
        {
            // Retire the session before calling out, so that a search started
            // from a callback is not mistaken for this one
            m_pSearchData = NULL;
            pSearchData->Release();

            CStringW status;
            status.Format(IDS_SEARCH_FILES_FOUND, uTotalFound);
            if (m_shellBrowser)
                m_shellBrowser->SetStatusTextSB(status);

            NotifyConnections(DISPID_SEARCHCOMPLETE);
        }
        else if (pszStatus && m_shellBrowser)
        {
            m_shellBrowser->SetStatusTextSB(pszStatus);
        }
    }

    SHFree(pszStatus);
    pSearchData->Release();
    return 0;
}

// *** IShellFolder2 methods ***
STDMETHODIMP CFindFolder::GetDefaultSearchGUID(GUID *pguid)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

STDMETHODIMP CFindFolder::EnumSearches(IEnumExtraSearch **ppenum)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

STDMETHODIMP CFindFolder::GetDefaultColumn(DWORD, ULONG *pSort, ULONG *pDisplay)
{
    if (pSort)
        *pSort = COL_NAME_INDEX;
    if (pDisplay)
        *pDisplay = COL_NAME_INDEX;
    return S_OK;
}

STDMETHODIMP CFindFolder::GetDefaultColumnState(UINT iColumn, DWORD *pcsFlags)
{
    if (!pcsFlags)
        return E_INVALIDARG;
    if (iColumn >= _countof(g_ColumnDefs))
        return m_pisfInner->GetDefaultColumnState(iColumn - _countof(g_ColumnDefs) + 1, pcsFlags);
    *pcsFlags = g_ColumnDefs[iColumn].dwDefaultState;
    return S_OK;
}

STDMETHODIMP CFindFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    // FIXME: Handle COL_LOCATION_INDEX and COL_RELEVANCE_INDEX
    CComPtr<IShellFolder2> pFolder;
    PCUITEMID_CHILD pChild;
    if (SUCCEEDED(GetFSFolder2AndChild(pidl, &pFolder, &pChild)))
        return pFolder->GetDetailsEx(pChild, pscid, pv);
    return E_FAIL;
}

STDMETHODIMP CFindFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn, SHELLDETAILS *pDetails)
{
    if (iColumn >= _countof(g_ColumnDefs))
    {
        UINT FSColumn = iColumn - _countof(g_ColumnDefs) + 1;
        if (pidl)
        {
            CComPtr<IShellFolder2> pFolder;
            PCUITEMID_CHILD pChild;
            if (SUCCEEDED(GetFSFolder2AndChild(pidl, &pFolder, &pChild)))
                return pFolder->GetDetailsOf(pChild, FSColumn, pDetails);
        }
        return m_pisfInner->GetDetailsOf(pidl, FSColumn, pDetails); // Column header info
    }

    pDetails->cxChar = g_ColumnDefs[iColumn].cxChar;
    pDetails->fmt = g_ColumnDefs[iColumn].fmt;
    if (!pidl)
        return SHSetStrRet(&pDetails->str, _AtlBaseModule.GetResourceInstance(), g_ColumnDefs[iColumn].iResource);

    if (iColumn == COL_LOCATION_INDEX)
    {
        return SHSetStrRet(&pDetails->str, _ILGetPath(pidl));
    }

    if (iColumn == COL_RELEVANCE_INDEX)
    {
        // TODO: Fill once the relevance is calculated
        return SHSetStrRetEmpty(&pDetails->str);
    }

    ATLASSERT(iColumn == COL_NAME_INDEX);
    return GetDisplayNameOf(pidl, SHGDN_NORMAL | SHGDN_INFOLDER, &pDetails->str);
}

STDMETHODIMP CFindFolder::MapColumnToSCID(UINT iColumn, SHCOLUMNID *pscid)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

// *** IShellFolder methods ***
STDMETHODIMP CFindFolder::ParseDisplayName(HWND hwndOwner, LPBC pbc, LPOLESTR lpszDisplayName, ULONG *pchEaten,
                                           PIDLIST_RELATIVE *ppidl, ULONG *pdwAttributes)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

STDMETHODIMP CFindFolder::EnumObjects(HWND hwndOwner, DWORD dwFlags, LPENUMIDLIST *ppEnumIDList)
{
    *ppEnumIDList = NULL;
    return S_FALSE;
}

STDMETHODIMP CFindFolder::BindToObject(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    HRESULT hr;
    CComPtr<IShellFolder> pInnerFolder;
    PCUITEMID_CHILD pidlChild;
    if (FAILED_UNEXPECTEDLY(hr = GetFSFolderAndChild(pidl, &pInnerFolder, &pidlChild)))
         return hr;
    return pInnerFolder->BindToObject(pidlChild, pbcReserved, riid, ppvOut);
}

STDMETHODIMP CFindFolder::BindToStorage(PCUIDLIST_RELATIVE pidl, LPBC pbcReserved, REFIID riid, LPVOID *ppvOut)
{
    UNIMPLEMENTED;
    return E_NOTIMPL;
}

STDMETHODIMP CFindFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1, PCUIDLIST_RELATIVE pidl2)
{
    HRESULT hr;
    WORD wColumn = LOWORD(lParam);
    switch (wColumn)
    {
    case COL_NAME_INDEX: // Name
        C_ASSERT(COL_NAME_INDEX == 0); // SHELL32::SHFSF_COL_NAME
        // We can have more than one item with the same filename, in this case, look at the path as well.
        // When DefView wants to identify a specific item, it will use the SHCIDS_CANONICALONLY flag.
        hr = m_pisfInner->CompareIDs(MAKELONG(0, HIWORD(lParam)), _ILGetFSPidl(pidl1), _ILGetFSPidl(pidl2));
        if (hr == S_EQUAL && (lParam & (SHCIDS_CANONICALONLY | SHCIDS_ALLFIELDS)))
            hr = CompareIDs(COL_LOCATION_INDEX, pidl1, pidl2);
        return hr;
    case COL_LOCATION_INDEX: // Path
        return MAKE_COMPARE_HRESULT(StrCmpW(_ILGetPath(pidl1), _ILGetPath(pidl2)));
    case COL_RELEVANCE_INDEX: // Relevance
        return E_NOTIMPL;
    default: // Default columns
        wColumn -= _countof(g_ColumnDefs) - 1;
        break;
    }
    return m_pisfInner->CompareIDs(MAKELONG(wColumn, HIWORD(lParam)), _ILGetFSPidl(pidl1), _ILGetFSPidl(pidl2));
}

STDMETHODIMP CFindFolder::CreateViewObject(HWND hwndOwner, REFIID riid, LPVOID *ppvOut)
{
    if (riid == IID_IShellView)
    {
        SFV_CREATE sfvparams = {};
        sfvparams.cbSize = sizeof(SFV_CREATE);
        sfvparams.pshf = this;
        sfvparams.psfvcb = this;
        HRESULT hr = SHCreateShellFolderView(&sfvparams, (IShellView **) ppvOut);
        if (FAILED_UNEXPECTEDLY(hr))
        {
            return hr;
        }

        return ((IShellView * ) * ppvOut)->QueryInterface(IID_PPV_ARG(IShellFolderView, &m_shellFolderView));
    }
    return E_NOINTERFACE;
}

STDMETHODIMP CFindFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl, DWORD *rgfInOut)
{
    if (!cidl)
    {
        *rgfInOut &= SFGAO_BROWSABLE; // TODO: SFGAO_CANRENAME?
        return S_OK;
    }

    HRESULT hr = E_INVALIDARG;
    for (UINT i = 0; i < cidl; ++i)
    {
        CComPtr<IShellFolder> pFolder;
        PCUITEMID_CHILD pidlChild;
        if (FAILED_UNEXPECTEDLY(hr = GetFSFolderAndChild(apidl[i], &pFolder, &pidlChild)))
            break;
        if (FAILED(hr = pFolder->GetAttributesOf(1, &pidlChild, rgfInOut)))
            break;
    }
    return hr;
}

class CFindFolderContextMenu :
        public IContextMenu,
        public IObjectWithSite,
        public CComObjectRootEx<CComMultiThreadModelNoCS>
{
    CComPtr<IContextMenu> m_pInner;
    CComPtr<IShellFolderView> m_shellFolderView;
    IUnknown *m_pUnkSite = NULL;
    UINT m_cidl;
    UINT m_MyFirstId = 0;
    static const UINT ADDITIONAL_MENU_ITEMS = 2;

    // *** IContextMenu ***
    STDMETHODIMP QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
    {
        if (m_cidl > 1)
            uFlags &= ~CMF_CANRENAME;

        m_MyFirstId = 0;
        if (idCmdLast - idCmdFirst > ADDITIONAL_MENU_ITEMS)
        {
            // We use the last available id. For DefView, this places us at
            // DVIDM_CONTEXTMENU_LAST which should not collide with anything.
            // This is just a temporary fix until we are moved to shell32 and
            // can use DFM_MERGECONTEXTMENU.
            _InsertMenuItemW(hMenu, indexMenu++, TRUE, idCmdLast--, MFT_STRING,
                             MAKEINTRESOURCEW(IDS_SEARCH_OPEN_FOLDER), MFS_ENABLED);
            _InsertMenuItemW(hMenu, indexMenu++, TRUE, idCmdLast--, MFT_SEPARATOR, NULL, 0);
            m_MyFirstId = idCmdLast + 1;
        }
        return m_pInner->QueryContextMenu(hMenu, indexMenu, idCmdFirst, idCmdLast, uFlags);
    }

    STDMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO lpcmi)
    {
        WORD idCmd = IS_INTRESOURCE(lpcmi->lpVerb) ? LOWORD(lpcmi->lpVerb) : 0;
        if (m_MyFirstId && idCmd >= m_MyFirstId && idCmd < m_MyFirstId + ADDITIONAL_MENU_ITEMS)
        {
            PCUITEMID_CHILD *apidl;
            UINT cidl;
            HRESULT hResult = m_shellFolderView->GetSelectedObjects(&apidl, &cidl);
            if (FAILED_UNEXPECTEDLY(hResult))
                return hResult;

            for (UINT i = 0; i < cidl; i++)
            {
                CComHeapPtr<ITEMIDLIST> folderPidl(ILCreateFromPathW(_ILGetPath(apidl[i])));
                if (!folderPidl)
                {
                    hResult = E_OUTOFMEMORY;
                    break;
                }
                LPCITEMIDLIST child = _ILGetFSPidl(apidl[i]);
                SHOpenFolderAndSelectItems(folderPidl, 1, &child, 0);
            }
            LocalFree(apidl); // Yes, LocalFree
            return hResult;
        }

        // TODO: Use GetDfmCmd instead after moving all of this to shell32
        CHAR szVerb[42];
        PCSTR pszVerb = !idCmd ? lpcmi->lpVerb : NULL;
        if (!pszVerb && SUCCEEDED(GetCommandStringA(m_pInner, LOWORD(lpcmi->lpVerb), GCS_VERBA, szVerb, _countof(szVerb))))
            pszVerb = szVerb;

        if (pszVerb && !lstrcmpiA(pszVerb, "rename"))
        {
            // Note: We can't invoke m_pInner (CDefaultContextMenu::DoRename) because the pidl is incorrect and SelectItem will fail.
            return BeginRenameOfShellViewSelection(m_pUnkSite);
        }

        // FIXME: We can't block FCIDM_SHVIEW_REFRESH here, add items on SFVM_LISTREFRESHED instead
        return m_pInner->InvokeCommand(lpcmi);
    }

    STDMETHODIMP GetCommandString(UINT_PTR idCommand, UINT uFlags, UINT *lpReserved, LPSTR lpszName, UINT uMaxNameLen)
    {
        return m_pInner->GetCommandString(idCommand, uFlags, lpReserved, lpszName, uMaxNameLen);
    }

    // *** IObjectWithSite ***
    STDMETHODIMP SetSite(IUnknown *pUnkSite) override
    {
        IUnknown_Set(&m_pUnkSite, pUnkSite);
        IUnknown_SetSite(m_pInner, pUnkSite);
        return S_OK;
    }

    STDMETHODIMP GetSite(REFIID riid, void **ppvSite) override
    {
        *ppvSite = NULL;
        return m_pUnkSite ? m_pUnkSite->QueryInterface(riid, ppvSite) : E_FAIL;
    }

public:
    ~CFindFolderContextMenu()
    {
        SetSite(NULL);
    }

    static HRESULT Create(IShellFolderView *pShellFolderView, UINT cidl, IContextMenu *pInnerContextMenu, IContextMenu **pContextMenu)
    {
        CComObject<CFindFolderContextMenu> *pObj;
        HRESULT hResult = CComObject<CFindFolderContextMenu>::CreateInstance(&pObj);
        if (FAILED_UNEXPECTEDLY(hResult))
            return hResult;
        pObj->m_shellFolderView = pShellFolderView;
        pObj->m_pInner = pInnerContextMenu;
        pObj->m_cidl = cidl;
        return pObj->QueryInterface(IID_PPV_ARG(IContextMenu, pContextMenu));
    }

    BEGIN_COM_MAP(CFindFolderContextMenu)
        COM_INTERFACE_ENTRY_IID(IID_IContextMenu, IContextMenu)
        COM_INTERFACE_ENTRY_IID(IID_IObjectWithSite, IObjectWithSite)
    END_COM_MAP()
};

int CALLBACK CFindFolder::SortItemsForDataObject(void *p1, void *p2, LPARAM lparam)
{
    // For Delete/Move operations, a subfolder/file needs to come before the parent folder.
    // A descendant's absolute pidl extends its ancestor's, so sort the longest pidls first.
    return (int)::ILGetSize((LPCITEMIDLIST)p2) - (int)::ILGetSize((LPCITEMIDLIST)p1);
}

STDMETHODIMP CFindFolder::GetUIObjectOf(HWND hwndOwner, UINT cidl, PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                        UINT *prgfInOut, LPVOID *ppvOut)
{
    HRESULT hr;
    if (!ppvOut)
        return E_POINTER;
    *ppvOut = NULL;
    if (cidl <= 0 || !apidl)
        return E_INVALIDARG;

    CComHeapPtr<PCITEMID_CHILD> aFSPidlAlloc;
    PCITEMID_CHILD pidlSingleBuffer, *aFSPidl = &pidlSingleBuffer; // Optimize for single item callers
    if (cidl != 1)
    {
        if (riid == IID_IDataObject)
        {
            if (HDPA hDpa = CreateAbsolutePidlArray(cidl, apidl))
            {
                DPA_Sort(hDpa, SortItemsForDataObject, NULL);
                ITEMIDLIST pidlRoot = {};
                hr = SHCreateFileDataObject(&pidlRoot, cidl, (PCUITEMID_CHILD_ARRAY)DPA_GetPtrPtr(hDpa),
                                            NULL, (IDataObject**)ppvOut);
                FreePidlArray(hDpa);
                return hr;
            }
        }

        // The remaining objects are created by the items' parent folder, which can only
        // interpret its own children. Never hand items from other folders to it.
        PCWSTR pszFirstParent = _ILGetPath(apidl[0]);
        for (UINT i = 1; i < cidl; i++)
        {
            PCWSTR pszParent = _ILGetPath(apidl[i]);
            if (!pszFirstParent || !pszParent || lstrcmpiW(pszFirstParent, pszParent))
            {
                // FIXME: Build an aggregate object for items from different folders
                WARN("Selection spans more than one folder\n");
                return E_FAIL;
            }
        }

        if (!aFSPidlAlloc.Allocate(cidl))
            return E_OUTOFMEMORY;
        aFSPidl = aFSPidlAlloc;
    }
    for (UINT i = 0; i < cidl; i++)
    {
        aFSPidl[i] = _ILGetFSPidl(apidl[i]);
    }

    if (riid == IID_IContextMenu)
    {
        // FIXME: Use CDefFolderMenu_Create2(..., AddFSClassKeysToArray())
        CComHeapPtr<ITEMIDLIST> folderPidl(ILCreateFromPathW(_ILGetPath(apidl[0])));
        if (!folderPidl)
            return E_OUTOFMEMORY;
        CComPtr<IShellFolder> pDesktopFolder;
        if (FAILED_UNEXPECTEDLY(hr = SHGetDesktopFolder(&pDesktopFolder)))
            return hr;
        CComPtr<IShellFolder> pShellFolder;
        hr = pDesktopFolder->BindToObject(folderPidl, NULL, IID_PPV_ARG(IShellFolder, &pShellFolder));
        if (FAILED_UNEXPECTEDLY(hr))
            return hr;
        CComPtr<IContextMenu> pContextMenu;
        hr = pShellFolder->GetUIObjectOf(hwndOwner, cidl, aFSPidl, riid, prgfInOut, (void**)&pContextMenu);
        if (FAILED_UNEXPECTEDLY(hr))
            return hr;
        return CFindFolderContextMenu::Create(m_shellFolderView, cidl, pContextMenu, (IContextMenu **)ppvOut);
    }

    CComPtr<IShellFolder> pFolder;
    if (FAILED_UNEXPECTEDLY(hr = GetFSFolderAndChild(apidl[0], &pFolder)))
        return hr;
    return pFolder->GetUIObjectOf(hwndOwner, cidl, aFSPidl, riid, prgfInOut, ppvOut);
}

STDMETHODIMP CFindFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, DWORD dwFlags, LPSTRRET pName)
{
    HRESULT hr;
    CComPtr<IShellFolder> pFolder;
    PCUITEMID_CHILD pidlChild;
    if (FAILED_UNEXPECTEDLY(hr = GetFSFolderAndChild(pidl, &pFolder, &pidlChild)))
        return hr;
    return pFolder->GetDisplayNameOf(pidlChild, dwFlags, pName);
}

STDMETHODIMP CFindFolder::SetNameOf(HWND hwndOwner, PCUITEMID_CHILD pidl, LPCOLESTR lpName, DWORD dwFlags,
                                    PITEMID_CHILD *pPidlOut)
{
    if (!pPidlOut)
        return E_INVALIDARG; // Our pidls are special, the caller must get it from us and not from parsing
    *pPidlOut = NULL;

    HRESULT hr;
    CComPtr<IShellFolder> pFolder;
    PCUITEMID_CHILD pidlChild;
    if (FAILED_UNEXPECTEDLY(hr = GetFSFolderAndChild(pidl, &pFolder, &pidlChild)))
        return hr;

    PCWSTR pszDir = _ILGetPath(pidl);
    PWSTR pszFull = (PWSTR)SHAlloc((wcslen(pszDir) + MAX_PATH) * sizeof(*pszDir));
    if (!pszFull)
        return E_OUTOFMEMORY;

    PITEMID_CHILD pidlRawNew = NULL;
    hr = pFolder->SetNameOf(hwndOwner, pidlChild, lpName, dwFlags, &pidlRawNew);
    if (SUCCEEDED(hr) && pidlRawNew)
    {
        WCHAR szFileName[MAX_PATH];
        hr = DisplayNameOfW(pFolder, pidlRawNew, SHGDN_FORPARSING | SHGDN_INFOLDER, szFileName, _countof(szFileName));
        ILFree(pidlRawNew);
        PathCombineW(pszFull, pszDir, szFileName);
        if (SUCCEEDED(hr))
            hr = ((*pPidlOut = _ILCreate(pszFull)) != NULL) ? S_OK : E_OUTOFMEMORY;
    }
    SHFree(pszFull);
    return hr;
}

//// *** IShellFolderViewCB method ***
STDMETHODIMP CFindFolder::MessageSFVCB(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case SFVM_DEFVIEWMODE:
        {
            FOLDERVIEWMODE *pViewMode = (FOLDERVIEWMODE *) lParam;
            *pViewMode = FVM_DETAILS;
            return S_OK;
        }
        case SFVM_WINDOWCREATED:
        {
            // Subclass window to receive window messages
            SubclassWindow((HWND) wParam);

            // Get shell browser for updating status bar text
            HRESULT hr = IUnknown_QueryService(m_shellFolderView, SID_SShellBrowser, IID_PPV_ARG(IShellBrowser, &m_shellBrowser));
            if (FAILED_UNEXPECTEDLY(hr))
                return hr;

            // Open search bar
            CComPtr<IWebBrowser2> pWebBrowser2;
            hr = m_shellBrowser->QueryInterface(IID_PPV_ARG(IWebBrowser2, &pWebBrowser2));
            if (FAILED_UNEXPECTEDLY(hr))
                return hr;
            WCHAR wszGuid[39];
            StringFromGUID2(CLSID_FileSearchBand, wszGuid, _countof(wszGuid));
            CComVariant searchBar(wszGuid);
            return pWebBrowser2->ShowBrowserBar(&searchBar, NULL, NULL);
        }
        case SFVM_WINDOWCLOSING:
        {
            EndSearch(TRUE);

            // [CORE-20504] LVN_ITEMACTIVATE in the DefView ListView can trigger a navigation that destroys this IShellFolder,
            // unsubclass now so we don't get a message after our "this" is dead.
            UnsubclassWindow();
            m_shellFolderView = NULL;
            m_shellBrowser = NULL;
            return S_OK;
        }
        case SFVM_GETCOMMANDDIR:
        {
            HRESULT hr = E_FAIL;
            if (m_shellFolderView)
            {
                PCUITEMID_CHILD *apidl;
                UINT cidl = 0;
                if (SUCCEEDED(hr = m_shellFolderView->GetSelectedObjects(&apidl, &cidl)))
                {
                    if (cidl)
                        hr = StringCchCopyW((PWSTR)lParam, wParam, _ILGetPath(apidl[0]));
                    LocalFree(apidl);
                }
            }
            return hr;
        }
        // TODO: SFVM_GETCOLUMNSTREAM
    }
    return E_NOTIMPL;
}

//// *** IItemNameLimits methods ***
STDMETHODIMP CFindFolder::GetMaxLength(LPCWSTR pszName, int *piMaxNameLen)
{
    CComPtr<IItemNameLimits> pLimits;
    HRESULT hr = m_pisfInner->QueryInterface(IID_PPV_ARG(IItemNameLimits, &pLimits));
    return FAILED_UNEXPECTEDLY(hr) ? hr : pLimits->GetMaxLength(pszName, piMaxNameLen);;
}

STDMETHODIMP CFindFolder::GetValidCharacters(LPWSTR *ppwszValidChars, LPWSTR *ppwszInvalidChars)
{
    CComPtr<IItemNameLimits> pLimits;
    HRESULT hr = m_pisfInner->QueryInterface(IID_PPV_ARG(IItemNameLimits, &pLimits));
    return FAILED_UNEXPECTEDLY(hr) ? hr : pLimits->GetValidCharacters(ppwszValidChars, ppwszInvalidChars);
}

//// *** IPersistFolder2 methods ***
STDMETHODIMP CFindFolder::GetCurFolder(PIDLIST_ABSOLUTE *pidl)
{
    if (!pidl)
        return E_POINTER;
    if (!m_pidl)
    {
        *pidl = NULL;
        return S_FALSE;
    }
    return SHILClone(m_pidl, pidl);
}

// *** IPersistFolder methods ***
STDMETHODIMP CFindFolder::Initialize(PCIDLIST_ABSOLUTE pidl)
{
    if (m_pidl)
        return E_UNEXPECTED;
    HRESULT hr;
    if (FAILED(hr = SHGetDesktopFolder((IShellFolder**)&m_pSfDesktop)))
        return hr;
    if (FAILED(hr = SHILClone(pidl, &m_pidl)))
        return hr;
    return SHELL32_CoCreateInitSF(m_pidl, NULL, NULL, &CLSID_ShellFSFolder,
                                  IID_PPV_ARG(IShellFolder2, &m_pisfInner));
}

// *** IPersist methods ***
STDMETHODIMP CFindFolder::GetClassID(CLSID *pClassId)
{
    if (pClassId == NULL)
        return E_INVALIDARG;
    *pClassId = CLSID_FindFolder;
    return S_OK;
}
