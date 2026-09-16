/*
 * NDR union conformance tests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This test intentionally does not assume that Wine defines Windows
 * semantics.  Where ReactOS and Wine currently disagree, the probe
 * accepts the known candidate results and reports the observed result
 * so that a native Windows run can establish the compatibility target.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <windef.h>
#include <winbase.h>
#include <winnt.h>

#include "rpc.h"
#include "rpcndr.h"
#include "ndrtypes.h"

#include "wine/test.h"

struct union_memory
{
    LONG discriminant;
    union
    {
        LONG scalar;
        LONG *pointer;
    } arm;
};

union format_buffer
{
    ULONGLONG alignment;
    unsigned char bytes[24];
};

static void * CALLBACK test_alloc(SIZE_T size)
{
    return malloc(size);
}

static void CALLBACK test_free(void *ptr)
{
    free(ptr);
}

static const MIDL_STUB_DESC test_stub_desc =
{
    NULL,
    test_alloc,
    test_free,
    { 0 },
    0,
    0,
    0,
    0,
    NULL,
    1,
    0x20000,
    0,
    0x50100a4,
    0,
    NULL,
    0,
    1,
    0,
    0,
    0
};

static void put_u16(unsigned char *dst, USHORT value)
{
    dst[0] = value & 0xff;
    dst[1] = value >> 8;
}

static void put_u32(unsigned char *dst, ULONG value)
{
    dst[0] = value & 0xff;
    dst[1] = (value >> 8) & 0xff;
    dst[2] = (value >> 16) & 0xff;
    dst[3] = value >> 24;
}

static const unsigned char *build_union_format(union format_buffer *format)
{
    unsigned char *bytes = format->bytes;
    const ULONG arm_offset = FIELD_OFFSET(struct union_memory, arm);

    memset(format, 0, sizeof(*format));

    /* WIDL emits referenced pointer descriptions before the union. */
    bytes[0] = FC_UP;
    bytes[1] = FC_SIMPLE_POINTER;
    bytes[2] = FC_LONG;
    bytes[3] = FC_PAD;

    ok(arm_offset <= 0x0f, "union arm offset %lu does not fit the format byte\n", arm_offset);
    ok(sizeof(((struct union_memory *)0)->arm) <= 0xffff,
       "union arm size %Iu is too large\n", sizeof(((struct union_memory *)0)->arm));

    bytes[4] = FC_ENCAPSULATED_UNION;
    bytes[5] = (arm_offset << 4) | FC_LONG;
    put_u16(bytes + 6, sizeof(((struct union_memory *)0)->arm));
    put_u16(bytes + 8, 2); /* two arms */

    put_u32(bytes + 10, 0);
    put_u16(bytes + 14, 0x8000 | FC_LONG);

    put_u32(bytes + 16, 1);
    /* Pointer description starts at offset 0; this field starts at 20. */
    put_u16(bytes + 20, (USHORT)(SHORT)-20);

    put_u16(bytes + 22, 0xffff); /* no default arm */

    return bytes + 4;
}

static void size_union(struct union_memory *memory, ULONG initial_length,
                       ULONG *buffer_length, ULONG *pointer_length)
{
    RPC_MESSAGE rpc_message;
    MIDL_STUB_MESSAGE stub_message;
    MIDL_STUB_DESC stub_desc = test_stub_desc;
    union format_buffer format;
    const unsigned char *union_format = build_union_format(&format);

    stub_desc.pFormatTypes = format.bytes;
    NdrClientInitializeNew(&rpc_message, &stub_message, &stub_desc, 0);

    stub_message.BufferLength = initial_length;
    stub_message.PointerLength = 0;

    NdrEncapsulatedUnionBufferSize(&stub_message, (unsigned char *)memory,
                                   (PFORMAT_STRING)union_format);

    *buffer_length = stub_message.BufferLength;
    *pointer_length = stub_message.PointerLength;
}

static void test_encapsulated_union_buffer_size(void)
{
    static const ULONG initial_lengths[] = { 0, 16 };
    struct union_memory memory;
    LONG pointee = 0x12345678;
    ULONG buffer_length, pointer_length;
    ULONG expected_inline;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(initial_lengths); ++i)
    {
        const ULONG initial = initial_lengths[i];

        memory.discriminant = 0;
        memory.arm.scalar = 0x11223344;
        size_union(&memory, initial, &buffer_length, &pointer_length);
        ok(buffer_length == initial + 8,
           "scalar arm, initial %lu: BufferLength %lu, expected %lu\n",
           initial, buffer_length, initial + 8);
        ok(pointer_length == 0,
           "scalar arm, initial %lu: PointerLength %lu, expected 0\n",
           initial, pointer_length);

        memory.discriminant = 1;
        memory.arm.pointer = NULL;
        size_union(&memory, initial, &buffer_length, &pointer_length);
        ok(buffer_length == initial + 8,
           "null pointer arm, initial %lu: BufferLength %lu, expected %lu\n",
           initial, buffer_length, initial + 8);
        ok(pointer_length == 0,
           "null pointer arm, initial %lu: PointerLength %lu, expected 0\n",
           initial, pointer_length);

        memory.arm.pointer = &pointee;
        size_union(&memory, initial, &buffer_length, &pointer_length);
        expected_inline = initial + 8;

        /*
         * ReactOS currently adds the deferred pointee size to BufferLength,
         * while current Wine restores the already-counted inline length.
         * Accept both known candidates until native Windows establishes which
         * behavior is compatible; PointerLength should contain the pointee.
         */
        ok(buffer_length == expected_inline ||
           buffer_length == expected_inline + sizeof(pointee),
           "pointer arm, initial %lu: unexpected BufferLength %lu\n",
           initial, buffer_length);
        ok(pointer_length == sizeof(pointee),
           "pointer arm, initial %lu: PointerLength %lu, expected %Iu\n",
           initial, pointer_length, sizeof(pointee));

        trace("encapsulated union pointer arm: initial=%lu BufferLength=%lu "
              "PointerLength=%lu candidate=%s\n",
              initial, buffer_length, pointer_length,
              buffer_length == expected_inline ? "restore-inline" : "accumulate-deferred");
    }
}

START_TEST(ndr_union)
{
    test_encapsulated_union_buffer_size();
}
