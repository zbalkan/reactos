/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     User-mode launcher for NDIS kernel-mode tests
 */

#include <kmt_test.h>

#include "NdisTest.h"

/**
 * @brief Runs the NDIS buffer-pool contract tests in the standalone test driver.
 */
START_TEST(NdisBufferPool)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"NdisTest", TRUE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_NDIS_TEST_BUFFER_POOL);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    KmtCloseDriver();
    KmtUnloadDriver();
}
