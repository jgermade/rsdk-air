# Engine modules. Each one is added with rsdkair_engine_module() (cmake/RsdkAir.cmake).
# Every RSDK module passes RSDK_AUTOBUILD=${RSDKAIR_RETRO_DISABLE_PLUS}, which is what the
# upstream RETRO_DISABLE_PLUS option sets (Plus DLC off unless RSDKAIR_ENABLE_PLUS).

option(RSDKAIR_ENGINE_RSDKV3 "Build the RSDKv3 module (Sonic CD)" ON)
option(RSDKAIR_ENGINE_RSDKV4 "Build the RSDKv4 module (Sonic 1, Sonic 2)" ON)
# Desktop only for now: upstream RSDKv5 on Android needs its own GameActivity setup.
if(ANDROID)
    set(rsdkv5_default OFF)
else()
    set(rsdkv5_default ON)
endif()
option(RSDKAIR_ENGINE_RSDKV5 "Build the RSDKv5U module with Sonic Mania's logic" ${rsdkv5_default})

add_subdirectory(deps)

if(RSDKAIR_ENGINE_RSDKV3)
    add_subdirectory(rsdkv3)
endif()
if(RSDKAIR_ENGINE_RSDKV4)
    add_subdirectory(rsdkv4)
endif()
if(RSDKAIR_ENGINE_RSDKV5)
    add_subdirectory(rsdkv5)
endif()

if(RSDKAIR_BUILD_TESTS)
    # Test-only engine used by the loader tests. Never installed or packaged.
    add_subdirectory(dummy)
endif()
