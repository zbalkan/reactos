/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the search results folder (CLSID_FindFolder)
 * COPYRIGHT:   Copyright 2026 Zafer Balkan <feldrim@gmail.com>
 */

#define _UNICODE
#define UNICODE
#include <apitest.h>
#include <shlobj.h>
#include <atlbase.h>
#include <atlcom.h>
#include <shellutils.h>
#include <shlguid_undoc.h>
#include <versionhelpers.h>

#define CANARY_PTR ((void *)(ULONG_PTR)0xDEADBEEF)

/* ReactOS-private, see dll/win32/browseui/shellfind/shellfind.h */
static const IID IID_IROSFindFolderControl =
    { 0x31375a94, 0x9fbb, 0x40ea, { 0xae, 0x48, 0x32, 0x39, 0x93, 0xd7, 0xb3, 0x26 } };

/* Mirrors SearchStart; only pPaths is read before the folder checks for its view */
struct TEST_SEARCH_START
{
    void *pPaths;
    WCHAR szFileName[MAX_PATH];
    WCHAR szQuery[MAX_PATH];
    BOOL SearchHidden;
};

/* Mirrors LOCATIONITEM with a fixed-size path */
struct TEST_LOCATION
{
    void *pNext;
    WCHAR szPath[MAX_PATH];
};

struct IROSFindFolderControl : public IUnknown
{
    STDMETHOD(BeginSearch)(const TEST_SEARCH_START *pRequest) PURE;
    STDMETHOD(CancelSearch)() PURE;
};

static void TestPersistFolder(IPersistFolder2 *ppf2)
{
    HRESULT hr;

    // Documented: S_FALSE and a NULL pidl when the folder is not initialized
    LPITEMIDLIST pidl = (LPITEMIDLIST)CANARY_PTR;
    hr = ppf2->GetCurFolder(&pidl);
    ok(hr == S_FALSE, "hr = %lx\n", hr);
    ok(pidl == NULL, "pidl = %p\n", pidl);
    if (pidl && pidl != CANARY_PTR)
        ILFree(pidl);

    WCHAR szTemp[MAX_PATH];
    DWORD cchTemp = GetTempPathW(_countof(szTemp), szTemp);
    if (!cchTemp || cchTemp >= _countof(szTemp))
    {
        skip("GetTempPathW failed: %lu\n", cchTemp);
        return;
    }
    CComHeapPtr<ITEMIDLIST> pidlTemp(ILCreateFromPathW(szTemp));
    ok(pidlTemp != NULL, "ILCreateFromPathW failed\n");
    if (!pidlTemp)
        return;

    hr = ppf2->Initialize(pidlTemp);
    if (IsReactOS())
        ok(hr == S_OK, "hr = %lx\n", hr);
    else
        trace("Initialize with a file system pidl: %lx\n", hr);
    if (FAILED(hr))
        return;

    LPITEMIDLIST pidl1 = NULL, pidl2 = NULL;
    hr = ppf2->GetCurFolder(&pidl1);
    ok(hr == S_OK, "hr = %lx\n", hr);
    hr = ppf2->GetCurFolder(&pidl2);
    ok(hr == S_OK, "hr = %lx\n", hr);
    // Documented as "typically" a copy of the Initialize pidl, so only ReactOS must match
    if (IsReactOS())
        ok(pidl1 != NULL && ILIsEqual(pidl1, pidlTemp), "Wrong folder pidl\n");

    // Each call returns a copy owned by the caller
    ok(pidl1 != pidl2, "GetCurFolder returned the same pointer twice\n");
    ILFree(pidl1);
    if (pidl2 != pidl1)
        ILFree(pidl2);
}

static void TestGetUIObjectOf(IShellFolder *psf)
{
    void *pv = CANARY_PTR;
    HRESULT hr = psf->GetUIObjectOf(NULL, 0, NULL, IID_IContextMenu, NULL, &pv);
    if (IsReactOS())
    {
        ok(hr == E_INVALIDARG, "hr = %lx\n", hr);
        ok(pv == NULL, "pv = %p\n", pv);
    }
    else
    {
        trace("GetUIObjectOf without items: %lx, %p\n", hr, pv);
    }
    if (SUCCEEDED(hr) && pv && pv != CANARY_PTR)
        ((IUnknown *)pv)->Release();
}

static void TestFindFolderControl(IUnknown *punk)
{
    CComPtr<IROSFindFolderControl> pControl;
    HRESULT hr = punk->QueryInterface(IID_IROSFindFolderControl, (void **)&pControl);
    ok(hr == S_OK, "hr = %lx\n", hr);
    if (FAILED(hr))
        return;

    hr = pControl->BeginSearch(NULL);
    ok(hr == E_INVALIDARG, "hr = %lx\n", hr);

    TEST_SEARCH_START Request = { NULL };
    hr = pControl->BeginSearch(&Request);
    ok(hr == E_INVALIDARG, "hr = %lx\n", hr);

    // A valid request without a results view must fail without starting a search
    TEST_LOCATION Location = { NULL, L"C:\\" };
    Request.pPaths = &Location;
    hr = pControl->BeginSearch(&Request);
    ok(hr == E_UNEXPECTED, "hr = %lx\n", hr);

    // Strings without a terminator are rejected without reading past their buffers
    for (UINT i = 0; i < _countof(Request.szFileName); i++)
        Request.szFileName[i] = L'a';
    hr = pControl->BeginSearch(&Request);
    ok(hr == E_INVALIDARG, "hr = %lx\n", hr);
    Request.szFileName[0] = UNICODE_NULL;

    for (UINT i = 0; i < _countof(Location.szPath); i++)
        Location.szPath[i] = L'a';
    hr = pControl->BeginSearch(&Request);
    ok(hr == E_INVALIDARG, "hr = %lx\n", hr);

    hr = pControl->CancelSearch();
    ok(hr == S_OK, "hr = %lx\n", hr);
}

START_TEST(CFindFolder)
{
    CCoInit init;
    ok_hex(init.hr, S_OK);
    if (FAILED(init.hr))
    {
        skip("CoInitialize failed with 0x%08lX\n", init.hr);
        return;
    }

    CComPtr<IPersistFolder2> ppf2;
    HRESULT hr = CoCreateInstance(CLSID_FindFolder, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARG(IPersistFolder2, &ppf2));
    if (!IsReactOS() && GetNTVersion() >= _WIN32_WINNT_VISTA)
    {
        // The legacy search results folder is not registered on NT6
        ok(hr == REGDB_E_CLASSNOTREG, "hr = %lx\n", hr);
        return;
    }
    ok(hr == S_OK, "hr = %lx\n", hr);
    if (FAILED(hr))
        return;

    TestPersistFolder(ppf2);

    CComPtr<IShellFolder> psf;
    hr = ppf2->QueryInterface(IID_PPV_ARG(IShellFolder, &psf));
    ok(hr == S_OK, "hr = %lx\n", hr);
    if (SUCCEEDED(hr))
        TestGetUIObjectOf(psf);

    if (IsReactOS())
        TestFindFolderControl(ppf2);
}
