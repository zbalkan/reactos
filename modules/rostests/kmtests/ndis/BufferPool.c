/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Tests for NDIS 3.x-5.x buffer-pool descriptor accounting
 */

#include <kmt_test.h>
#include <ndis.h>

static UCHAR BufferStorage[64];

/**
 * @brief Verifies that an NDIS buffer pool owns and accounts its descriptors.
 *
 * NdisAllocateBufferPool must return a usable opaque pool handle when the
 * allocation succeeds. A pool created for one descriptor must allow one
 * NdisAllocateBuffer call, reject a second concurrent allocation with
 * NDIS_STATUS_RESOURCES, and make the descriptor available again after
 * NdisFreeBuffer.
 *
 * This is deliberately a contract test rather than an implementation test:
 * it makes no assumptions about the internal representation of the pool or
 * of NDIS_BUFFER. The scenario mirrors the descriptor-pool behaviour relied
 * upon by packet-based NDIS 3.x-5.x miniports and protocols.
 */
VOID
TestNdisBufferPoolDescriptorAccounting(VOID)
{
    NDIS_STATUS Status;
    NDIS_HANDLE PoolHandle = NULL;
    PNDIS_BUFFER FirstBuffer = NULL;
    PNDIS_BUFFER SecondBuffer = NULL;
    PNDIS_BUFFER ReusedBuffer = NULL;

    NdisAllocateBufferPool(&Status, &PoolHandle, 1);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    ok(PoolHandle != NULL,
       "NdisAllocateBufferPool succeeded but returned a NULL pool handle\n");

    if (skip(Status == NDIS_STATUS_SUCCESS && PoolHandle != NULL,
             "A valid buffer pool is required for descriptor accounting tests\n"))
    {
        return;
    }

    NdisAllocateBuffer(&Status,
                       &FirstBuffer,
                       PoolHandle,
                       &BufferStorage[0],
                       16);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    ok(FirstBuffer != NULL, "First buffer allocation returned NULL\n");

    NdisAllocateBuffer(&Status,
                       &SecondBuffer,
                       PoolHandle,
                       &BufferStorage[16],
                       16);
    ok_eq_hex(Status, NDIS_STATUS_RESOURCES);
    ok_eq_pointer(SecondBuffer, NULL);

    if (FirstBuffer != NULL)
    {
        NdisFreeBuffer(FirstBuffer);
        FirstBuffer = NULL;
    }

    NdisAllocateBuffer(&Status,
                       &ReusedBuffer,
                       PoolHandle,
                       &BufferStorage[32],
                       16);
    ok_eq_hex(Status, NDIS_STATUS_SUCCESS);
    ok(ReusedBuffer != NULL,
       "A descriptor freed to the pool could not be allocated again\n");

    if (ReusedBuffer != NULL)
        NdisFreeBuffer(ReusedBuffer);

    NdisFreeBufferPool(PoolHandle);
}
