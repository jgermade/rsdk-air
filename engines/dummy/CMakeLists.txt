add_library(rsdkair_dummy MODULE dummy.c)
target_link_libraries(rsdkair_dummy PRIVATE rsdkair_abi)
set_target_properties(rsdkair_dummy PROPERTIES
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/engines/$<0:>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/engines/$<0:>")
