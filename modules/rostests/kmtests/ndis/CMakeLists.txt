include_directories(../include)

list(APPEND NDISTEST_DRV_SOURCE
    ../kmtest_drv/kmtest_standalone.c
    BufferPool.c
    NdisTest_drv.c)

add_library(ndistest_drv MODULE ${NDISTEST_DRV_SOURCE})
set_module_type(ndistest_drv kernelmodedriver)
target_link_libraries(ndistest_drv kmtest_printf ${PSEH_LIB})
add_importlibs(ndistest_drv ndis ntoskrnl hal)
target_compile_definitions(ndistest_drv PRIVATE KMT_STANDALONE_DRIVER)
#add_pch(ndistest_drv ../include/kmt_test.h)
add_rostests_file(TARGET ndistest_drv)
