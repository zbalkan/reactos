/*
 * RPC generic binding handle conformance tests
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>

#include "rpc.h"
#include "rpcndr.h"
#include "generic_handle_c.h"

#include "wine/test.h"

static LONG bind_calls;
static LONG unbind_calls;

handle_t __RPC_USER generic_handle_t_bind(generic_handle_t binding)
{
    trace("generic_handle_t_bind(%s)\n", wine_dbgstr_a(binding));
    InterlockedIncrement(&bind_calls);
    return NULL;
}

void __RPC_USER generic_handle_t_unbind(generic_handle_t binding, handle_t handle)
{
    trace("generic_handle_t_unbind(%s, %p)\n", wine_dbgstr_a(binding), handle);
    InterlockedIncrement(&unbind_calls);
}

static void test_null_generic_binding(void)
{
    ULONG exception = 0;
    int result = 0;

    bind_calls = 0;
    unbind_calls = 0;

    RpcTryExcept
    {
        result = generic_handle_call("null-binding", 42);
        trace("generic_handle_call returned %d\n", result);
    }
    RpcExcept(TRUE)
    {
        exception = RpcExceptionCode();
        trace("generic_handle_call raised exception %lu\n", exception);
    }
    RpcEndExcept

    ok(bind_calls == 1, "bind routine called %ld times, expected 1\n", bind_calls);
    ok(unbind_calls == 0,
       "unbind routine called %ld times after bind returned NULL, expected 0\n",
       unbind_calls);

    trace("NULL generic binding result: exception=%lu bind_calls=%ld unbind_calls=%ld\n",
          exception, bind_calls, unbind_calls);
}

START_TEST(generic_handle)
{
    test_null_generic_binding();
}
