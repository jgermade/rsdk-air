# Engine modules. Each one is added with rsdkair_engine_module() (cmake/RsdkAir.cmake).
#
# M1 adds rsdkv4, M3 adds rsdkv3, rsdkv5 and sonic3air. Every RSDK module must pass
# RETRO_DISABLE_PLUS=${RSDKAIR_RETRO_DISABLE_PLUS} to its upstream build.

if(RSDKAIR_BUILD_TESTS)
    # Test-only engine used by the loader tests. Never installed or packaged.
    add_subdirectory(dummy)
endif()
