/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for CMergedFolder COM contracts
 * COPYRIGHT:   Copyright 2026 Zafer Balkan <feldrim@gmail.com>
 */

#include "shelltest.h"
#define NDEBUG
#include <debug.h>
#include <shellutils.h>
#include <shlguid_undoc.h>
#include <versionhelpers.h>
#include <strsafe.h>

/* Undocumented, see dll/win32/shell32/shellmenu/CMergedFolder.h */
interface IAugmentedShellFolder : public IShellFolder
{
    STDMETHOD(AddNameSpace)(LPGUID, IShellFolder *, LPCITEMIDLIST, ULONG) PURE;
    STDMETHOD(GetNameSpaceID)(LPCITEMIDLIST, LPGUID) PURE;
    STDMETHOD(QueryNameSpace)(ULONG, LPGUID, IShellFolder **) PURE;
    STDMETHOD(EnumNameSpace)(ULONG, PULONG) PURE;
};

#define CANARY_MASK ((SFGAOF)0xDEADBEEF)
#define CANARY_PTR  ((void *)(ULONG_PTR)0xDEADBEEF)

#define TEST_ATTRIBUTES (SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_CANCOPY | \
                         SFGAO_CANRENAME | SFGAO_STREAM)

/* Directory 0 is the user source, 1 the common source; both have same.txt and dup */
static const struct
{
    UINT iDir;
    LPCWSTR pszName;
    BOOL bDir;
} s_Items[] =
{
    { 0, L"u1.txt", FALSE }, { 0, L"u2.txt", FALSE }, { 0, L"same.txt", FALSE },
    { 0, L"dup", TRUE }, { 1, L"c1.txt", FALSE }, { 1, L"same.txt", FALSE }, { 1, L"dup", TRUE },
};

static WCHAR s_szDir[2][MAX_PATH];
static BOOL s_bDirCreated[2];

static BOOL SetupFixture(void)
{
    WCHAR szTemp[MAX_PATH], szName[64], szPath[MAX_PATH];
    if (!GetTempPathW(_countof(szTemp), szTemp))
        return FALSE;

    // Per-process names, and remember what this run created so cleanup leaves the rest alone
    for (UINT i = 0; i < 2; i++)
    {
        StringCchPrintfW(szName, _countof(szName), L"CMergedFolder_%u_%lu",
                         i, GetCurrentProcessId());
        PathCombineW(s_szDir[i], szTemp, szName);
        s_bDirCreated[i] = CreateDirectoryW(s_szDir[i], NULL);
        if (!s_bDirCreated[i])
            return FALSE;
    }

    for (UINT i = 0; i < _countof(s_Items); i++)
    {
        PathCombineW(szPath, s_szDir[s_Items[i].iDir], s_Items[i].pszName);
        if (s_Items[i].bDir)
        {
            if (!CreateDirectoryW(szPath, NULL))
                return FALSE;
            continue;
        }
        HANDLE hFile = CreateFileW(szPath, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return FALSE;
        CloseHandle(hFile);
    }
    return TRUE;
}

static void CleanupFixture(void)
{
    WCHAR szPath[MAX_PATH];
    for (UINT i = 0; i < _countof(s_Items); i++)
    {
        if (!s_bDirCreated[s_Items[i].iDir])
            continue;
        PathCombineW(szPath, s_szDir[s_Items[i].iDir], s_Items[i].pszName);
        if (s_Items[i].bDir)
            RemoveDirectoryW(szPath);
        else
            DeleteFileW(szPath);
    }
    for (UINT i = 0; i < 2; i++)
    {
        if (s_bDirCreated[i])
            RemoveDirectoryW(s_szDir[i]);
    }
}

static HRESULT BindToPath(LPCWSTR pszPath, IShellFolder **ppsf, LPITEMIDLIST *ppidl)
{
    CComPtr<IShellFolder> psfDesktop;
    HRESULT hr = SHGetDesktopFolder(&psfDesktop);
    if (FAILED(hr))
        return hr;

    hr = psfDesktop->ParseDisplayName(NULL, NULL, const_cast<LPWSTR>(pszPath), NULL, ppidl, NULL);
    if (FAILED(hr))
        return hr;

    hr = psfDesktop->BindToObject(*ppidl, NULL, IID_PPV_ARG(IShellFolder, ppsf));
    if (FAILED(hr))
    {
        ILFree(*ppidl);
        *ppidl = NULL;
    }
    return hr;
}

/*
 * Counts the merged folder's items with the given in-folder parsing name.
 * If ppidlFirst is not NULL, it receives the merged folder's own pidl of the first match.
 */
static UINT FindItems(IShellFolder *psf, LPCWSTR pszName, LPITEMIDLIST *ppidlFirst)
{
    if (ppidlFirst)
        *ppidlFirst = NULL;

    CComPtr<IEnumIDList> penum;
    HRESULT hr = psf->EnumObjects(NULL, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &penum);
    if (FAILED(hr) || !penum)
        return 0;

    UINT cItems = 0;
    LPITEMIDLIST pidl;
    while (penum->Next(1, &pidl, NULL) == S_OK)
    {
        STRRET strret;
        WCHAR szName[MAX_PATH];
        if (SUCCEEDED(psf->GetDisplayNameOf(pidl, SHGDN_FORPARSING | SHGDN_INFOLDER, &strret)) &&
            SUCCEEDED(StrRetToBufW(&strret, pidl, szName, _countof(szName))) &&
            !lstrcmpiW(szName, pszName))
        {
            cItems++;
            if (ppidlFirst && !*ppidlFirst)
            {
                *ppidlFirst = pidl;
                continue;
            }
        }
        ILFree(pidl);
    }
    return cItems;
}

/* Two items share one output mask: a canary after it catches writes for the second item */
static SFGAOF GetAttributesOf2(IShellFolder *psf, LPCITEMIDLIST pidl1, LPCITEMIDLIST pidl2)
{
    struct
    {
        SFGAOF sfgao;
        SFGAOF sfgaoCanary;
    } Mask = { TEST_ATTRIBUTES, CANARY_MASK };
    LPCITEMIDLIST apidl[2] = { pidl1, pidl2 };

    HRESULT hr = psf->GetAttributesOf(pidl2 ? 2 : 1, apidl, &Mask.sfgao);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(Mask.sfgaoCanary == CANARY_MASK, "Output mask overflowed: %lx\n", Mask.sfgaoCanary);
    return Mask.sfgao;
}

static void TestGetAttributesOf(IShellFolder *psf, LPCITEMIDLIST pidlU1, LPCITEMIDLIST pidlU2,
                                LPCITEMIDLIST pidlC1, LPCITEMIDLIST pidlDup)
{
    SFGAOF sfgaoU1 = GetAttributesOf2(psf, pidlU1, NULL);
    SFGAOF sfgaoU2 = GetAttributesOf2(psf, pidlU2, NULL);
    SFGAOF sfgaoC1 = GetAttributesOf2(psf, pidlC1, NULL);
    SFGAOF sfgaoDup = GetAttributesOf2(psf, pidlDup, NULL);
    SFGAOF sfgao;
    ok(!(sfgaoU1 & SFGAO_FOLDER), "u1.txt has SFGAO_FOLDER: %lx\n", sfgaoU1);
    ok(sfgaoDup & SFGAO_FOLDER, "dup lacks SFGAO_FOLDER: %lx\n", sfgaoDup);

    // The result is the attributes common to all items, from one source or both
    sfgao = GetAttributesOf2(psf, pidlU1, pidlU2);
    ok(sfgao == (sfgaoU1 & sfgaoU2), "mask = %lx, expected %lx\n", sfgao, sfgaoU1 & sfgaoU2);
    sfgao = GetAttributesOf2(psf, pidlU1, pidlC1);
    ok(sfgao == (sfgaoU1 & sfgaoC1), "mask = %lx, expected %lx\n", sfgao, sfgaoU1 & sfgaoC1);
    sfgao = GetAttributesOf2(psf, pidlU1, pidlDup);
    ok(!(sfgao & SFGAO_FOLDER), "mask = %lx\n", sfgao);
}

static void TestGetUIObjectOf(IShellFolder *psf, LPCITEMIDLIST pidlU1, LPCITEMIDLIST pidlU2,
                              LPCITEMIDLIST pidlC1)
{
    // One output object for the whole selection: a canary after it catches a second write
    void *apvOut[2] = { NULL, CANARY_PTR };
    LPCITEMIDLIST apidl[2] = { pidlU1, pidlU2 };
    HRESULT hr = psf->GetUIObjectOf(NULL, 2, apidl, IID_IDataObject, NULL, &apvOut[0]);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(apvOut[0] != NULL, "No object returned\n");
    ok(apvOut[1] == CANARY_PTR, "Output pointer overflowed: %p\n", apvOut[1]);
    if (apvOut[0])
        ((IUnknown *)apvOut[0])->Release();

    // Items from both sources: Windows may build an aggregate object, ReactOS fails
    apvOut[0] = CANARY_PTR;
    apidl[1] = pidlC1;
    hr = psf->GetUIObjectOf(NULL, 2, apidl, IID_IDataObject, NULL, &apvOut[0]);
    if (IsReactOS())
        ok(hr == E_FAIL, "hr = %lx\n", hr);
    if (FAILED(hr))
        ok(apvOut[0] == NULL, "hr = %lx, out = %p\n", hr, apvOut[0]);
    ok(apvOut[1] == CANARY_PTR, "Output pointer overflowed: %p\n", apvOut[1]);
    if (SUCCEEDED(hr) && apvOut[0] && apvOut[0] != CANARY_PTR)
        ((IUnknown *)apvOut[0])->Release();
}

static void TestParseDisplayName(IShellFolder *psf)
{
    LPITEMIDLIST pidl = NULL;
    ULONG sfgao = SFGAO_FILESYSTEM | SFGAO_FOLDER;
    HRESULT hr = psf->ParseDisplayName(NULL, NULL, const_cast<LPWSTR>(L"u1.txt"), NULL,
                                       &pidl, &sfgao);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(pidl != NULL, "No pidl returned\n");
    ok(sfgao & SFGAO_FILESYSTEM, "u1.txt lacks SFGAO_FILESYSTEM: %lx\n", sfgao);
    ok(!(sfgao & SFGAO_FOLDER), "u1.txt has SFGAO_FOLDER: %lx\n", sfgao);
    ILFree(pidl);

    pidl = NULL;
    sfgao = SFGAO_FOLDER;
    hr = psf->ParseDisplayName(NULL, NULL, const_cast<LPWSTR>(L"dup"), NULL, &pidl, &sfgao);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(pidl != NULL, "No pidl returned\n");
    ok(sfgao & SFGAO_FOLDER, "dup lacks SFGAO_FOLDER: %lx\n", sfgao);
    ILFree(pidl);
}

/* IPersistFolder2 is exposed on NT5 and NT6, so this runs without IAugmentedShellFolder */
static void TestPersistFolder(LPCITEMIDLIST pidlFolder)
{
    CComPtr<IPersistFolder2> ppf2;
    HRESULT hr = CoCreateInstance(CLSID_MergedFolder, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARG(IPersistFolder2, &ppf2));
    ok(hr == S_OK, "hr = %lx\n", hr);
    if (FAILED(hr))
        return;

    // Documented: S_FALSE and a NULL pidl before Initialize
    LPITEMIDLIST pidl1 = (LPITEMIDLIST)CANARY_PTR, pidl2 = NULL;
    hr = ppf2->GetCurFolder(&pidl1);
    ok(hr == S_FALSE, "hr = %lx\n", hr);
    ok(pidl1 == NULL, "pidl = %p\n", pidl1);

    // Initializing twice must not leak, and each call returns a copy owned by the caller
    hr = ppf2->Initialize(pidlFolder);
    ok(hr == S_OK, "hr = %lx\n", hr);
    hr = ppf2->Initialize(pidlFolder);
    ok(hr == S_OK, "hr = %lx\n", hr);
    pidl1 = NULL;
    hr = ppf2->GetCurFolder(&pidl1);
    ok(hr == S_OK, "hr = %lx\n", hr);
    hr = ppf2->GetCurFolder(&pidl2);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(pidl1 != NULL && ILIsEqual(pidl1, pidlFolder), "Wrong folder pidl\n");
    ok(pidl1 != pidl2, "GetCurFolder returned the same pointer twice\n");
    ILFree(pidl1);
    if (pidl2 != pidl1)
        ILFree(pidl2);
}

START_TEST(CMergedFolder)
{
    CCoInit init;
    ok_hex(init.hr, S_OK);
    if (FAILED(init.hr))
        return;

    if (!SetupFixture())
    {
        skip("Cannot create test directories\n");
        CleanupFixture();
        return;
    }

    {
        CComPtr<IShellFolder> psfUser, psfCommon;
        CComHeapPtr<ITEMIDLIST> pidlUser, pidlCommon;
        CComPtr<IAugmentedShellFolder> pasf;

        HRESULT hr = BindToPath(s_szDir[0], &psfUser, &pidlUser);
        ok(hr == S_OK, "hr = %lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = BindToPath(s_szDir[1], &psfCommon, &pidlCommon);
            ok(hr == S_OK, "hr = %lx\n", hr);
        }

        if (SUCCEEDED(hr))
        {
            TestPersistFolder(pidlUser);

            hr = CoCreateInstance(CLSID_MergedFolder, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IAugmentedShellFolder, (void **)&pasf);
            if (IsReactOS() || GetNTVersion() < _WIN32_WINNT_VISTA)
                ok(hr == S_OK, "hr = %lx\n", hr);
            else if (FAILED(hr))
                skip("IAugmentedShellFolder is not exposed on NT6: %lx\n", hr);
        }

        if (SUCCEEDED(hr))
        {
            hr = pasf->AddNameSpace(NULL, psfUser, pidlUser, 0xFF00);
            ok(hr == S_OK, "hr = %lx\n", hr);
            hr = pasf->AddNameSpace(NULL, psfCommon, pidlCommon, 0);
            ok(hr == S_OK, "hr = %lx\n", hr);
        }

        if (SUCCEEDED(hr))
        {
            CComHeapPtr<ITEMIDLIST> pidlU1, pidlU2, pidlC1, pidlDup;
            FindItems(pasf, L"u1.txt", &pidlU1);
            FindItems(pasf, L"u2.txt", &pidlU2);
            FindItems(pasf, L"c1.txt", &pidlC1);

            // Items with the same name in both sources are merged into one
            ok(FindItems(pasf, L"dup", &pidlDup) == 1, "dup is not merged\n");
            ok(FindItems(pasf, L"same.txt", NULL) == 1, "same.txt is not merged\n");

            ok(pidlU1 && pidlU2 && pidlC1 && pidlDup, "Missing items\n");
            if (pidlU1 && pidlU2 && pidlC1 && pidlDup)
            {
                TestGetAttributesOf(pasf, pidlU1, pidlU2, pidlC1, pidlDup);
                TestGetUIObjectOf(pasf, pidlU1, pidlU2, pidlC1);
                TestParseDisplayName(pasf);
            }
        }
    }

    CleanupFixture();
}
