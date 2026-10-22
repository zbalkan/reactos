/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Test for the search results folder (CLSID_FindFolder)
 * COPYRIGHT:   Copyright 2026 Zafer Balkan <feldrim@gmail.com>
 */

#include "shelltest.h"
#define NDEBUG
#include <debug.h>
#include <shellutils.h>
#include <shlguid_undoc.h>
#include <versionhelpers.h>

/* Windows XP and Server 2003 serve the search results folder from shell32 */
static void TestServer(void)
{
    WCHAR szPath[MAX_PATH];
    DWORD cbPath = sizeof(szPath);
    LSTATUS Error = RegGetValueW(HKEY_CLASSES_ROOT,
                                 L"CLSID\\{E17D4FC0-5564-11D1-83F2-00A0C90DC849}\\InprocServer32",
                                 NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                                 NULL, szPath, &cbPath);
    ok(Error == ERROR_SUCCESS, "Error = %ld\n", Error);
    if (Error != ERROR_SUCCESS)
        return;
    ok(!lstrcmpiW(PathFindFileNameW(szPath), L"shell32.dll"), "Server is '%S'\n", szPath);
}

static void TestPersistFolder(IPersistFolder2 *ppf2)
{
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

    HRESULT hr = ppf2->Initialize(pidlTemp);
    ok(hr == S_OK, "hr = %lx\n", hr);
    if (FAILED(hr))
        return;

    CComHeapPtr<ITEMIDLIST> pidl;
    hr = ppf2->GetCurFolder(&pidl);
    ok(hr == S_OK, "hr = %lx\n", hr);
    ok(pidl && ILIsEqual(pidl, pidlTemp), "Wrong folder pidl\n");
}

START_TEST(CFindFolder)
{
    CCoInit init;
    ok_hex(init.hr, S_OK);
    if (FAILED(init.hr))
        return;

    CComPtr<IPersistFolder2> ppf2;
    HRESULT hr = CoCreateInstance(CLSID_FindFolder, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARG(IPersistFolder2, &ppf2));
    if (!IsReactOS() && GetNTVersion() >= _WIN32_WINNT_VISTA)
    {
        // The legacy search results folder is not registered on Windows Vista and later
        ok(hr == REGDB_E_CLASSNOTREG, "hr = %lx\n", hr);
        return;
    }
    ok(hr == S_OK, "hr = %lx\n", hr);

    TestServer();
    if (SUCCEEDED(hr))
        TestPersistFolder(ppf2);
}
