/* Automatically generated file; DO NOT EDIT!! */

#define STANDALONE
#include <wine/test.h>

extern void func_cstub(void);
extern void func_generated(void);
extern void func_generic_handle(void);
extern void func_ndr_marshall(void);
extern void func_ndr_union(void);
extern void func_rpc(void);
extern void func_rpc_async(void);
extern void func_server(void);

const struct test winetest_testlist[] =
{
    { "cstub", func_cstub },
    { "generated", func_generated },
    { "generic_handle", func_generic_handle },
    { "ndr_marshall", func_ndr_marshall },
    { "ndr_union", func_ndr_union },
    { "rpc", func_rpc },
    { "rpc_async", func_rpc_async },
    { "server", func_server },
    { 0, 0 }
};
