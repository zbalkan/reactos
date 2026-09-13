/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Kernel-mode entry points for NDIS contract tests
 */

#include <kmt_test.h>

#include "NdisTest.h"

static KMT_MESSAGE_HANDLER TestMessageHandler;

VOID
TestNdisBufferPoolDescriptorAccounting(VOID);

/**
 * @brief Initializes the standalone NDIS test driver.
 *
 * The test driver is intentionally separate from kmtest_drv.sys so that the
 * NDIS dependency is isolated to tests that exercise ndis.sys directly.
 */
NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    PAGED_CODE();

    *DeviceName = L"NdisTest";
    KmtRegisterMessageHandler(IOCTL_NDIS_TEST_BUFFER_POOL, NULL, TestMessageHandler);

    return STATUS_SUCCESS;
}

/**
 * @brief Unloads the standalone NDIS test driver.
 */
VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE();
}

/**
 * @brief Dispatches user-mode requests to the corresponding NDIS contract test.
 */
static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    PAGED_CODE();

    if (ControlCode != IOCTL_NDIS_TEST_BUFFER_POOL)
        return STATUS_INVALID_DEVICE_REQUEST;

    TestNdisBufferPoolDescriptorAccounting();
    return STATUS_SUCCESS;
}
