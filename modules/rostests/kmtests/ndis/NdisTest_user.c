/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     User-mode launcher for NDIS kernel-mode tests
 */

#include <kmt_test.h>

#include "NdisTest.h"

static
VOID
RunNdisTest(
    _In_ ULONG ControlCode)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"NdisTest", TRUE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(ControlCode);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    KmtCloseDriver();
    KmtUnloadDriver();
}

/**
 * @brief Verifies creation and destruction of an NDIS buffer pool.
 */
START_TEST(NdisBufferPoolCreate)
{
    RunNdisTest(IOCTL_NDIS_TEST_BUFFER_POOL_CREATE);
}

/**
 * @brief Verifies descriptor accounting and reuse in an NDIS buffer pool.
 */
START_TEST(NdisBufferPoolAccounting)
{
    RunNdisTest(IOCTL_NDIS_TEST_BUFFER_POOL_ACCOUNTING);
}
