if(RSDKAIR_ENABLE_PLUS)
    set(RSDKAIR_PLUS_ENABLED_CXX true)
else()
    set(RSDKAIR_PLUS_ENABLED_CXX false)
endif()
configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/rsdkair/version.hpp" @ONLY)

# Platform-independent launcher logic. No SDL here, so it can be tested headless.
add_library(rsdkair_core STATIC
    catalog.cpp
    cli.cpp
    detect.cpp
    module.cpp
    paths.cpp
    process.cpp
    rom.cpp
    run_engine.cpp)
target_include_directories(rsdkair_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rsdkair_core PUBLIC rsdkair_abi ${CMAKE_DL_LIBS})
rsdkair_warnings(rsdkair_core)
