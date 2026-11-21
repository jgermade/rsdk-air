cmake_minimum_required(VERSION 3.22)

# Version: release.yml passes -DRSDKAIR_VERSION=X.Y.Z; other builds are "0.0.0-dev".
set(RSDKAIR_VERSION "0.0.0-dev" CACHE STRING "rsdk-air version string")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" RSDKAIR_VERSION_NUMBER "${RSDKAIR_VERSION}")
if(NOT RSDKAIR_VERSION_NUMBER)
    message(FATAL_ERROR "RSDKAIR_VERSION must start with X.Y.Z, got '${RSDKAIR_VERSION}'")
endif()

project(rsdk-air VERSION ${RSDKAIR_VERSION_NUMBER} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

# Plus DLC. OFF passes RETRO_DISABLE_PLUS=ON to the RSDK engines, as their licenses
# require for any pre-built executable given to anyone. Only turn it ON for builds you
# keep for yourself (see ROADMAP/2026-11-20/upstreams-and-licensing.md).
option(RSDKAIR_ENABLE_PLUS "Enable Plus DLC content in the RSDK engines (personal builds only)" OFF)
option(RSDKAIR_BUILD_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

if(RSDKAIR_ENABLE_PLUS)
    set(RSDKAIR_RETRO_DISABLE_PLUS OFF)
    message(WARNING "RSDKAIR_ENABLE_PLUS is ON: do not distribute this build.")
else()
    set(RSDKAIR_RETRO_DISABLE_PLUS ON)
endif()

# ---------------------------------------------------------------------------
# Output layout (build tree mirrors the installed layout)
#   bin/rsdk-air(.exe), bin/SDL2.*, bin/engines/<engine>.*
# ---------------------------------------------------------------------------

set(RSDKAIR_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(RSDKAIR_ENGINES_OUTPUT_DIR "${RSDKAIR_OUTPUT_DIR}/engines")
# The "$<0:>" stops multi-config generators from adding a per-config subdirectory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RSDKAIR_OUTPUT_DIR}/$<0:>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${RSDKAIR_OUTPUT_DIR}/$<0:>")

if(APPLE)
    set(CMAKE_INSTALL_RPATH "@executable_path")
elseif(UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RsdkAir)

# ---------------------------------------------------------------------------
# Third party
# ---------------------------------------------------------------------------

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
    message(FATAL_ERROR "third_party/SDL is missing: run 'git submodule update --init --recursive'")
endif()

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------

# The engine ABI header, used by the launcher and every engine module.
add_library(rsdkair_abi INTERFACE)
target_include_directories(rsdkair_abi INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(src/core)
add_subdirectory(src/launcher)
add_subdirectory(engines)

# The engines this build includes, for the launcher (generated after engines/ has run).
get_property(rsdkair_engine_modules GLOBAL PROPERTY RSDKAIR_ENGINE_MODULES)
list(LENGTH rsdkair_engine_modules RSDKAIR_ENGINE_COUNT)
set(RSDKAIR_ENGINE_LIST "")
foreach(module IN LISTS rsdkair_engine_modules)
    string(APPEND RSDKAIR_ENGINE_LIST "\"${module}\", ")
endforeach()
configure_file(src/core/engines.hpp.in "${PROJECT_BINARY_DIR}/src/core/generated/rsdkair/engines.hpp" @ONLY)

if(RSDKAIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install (desktop): <prefix>/rsdk-air, SDL2, engines/, art/, fonts/, licenses/
# ---------------------------------------------------------------------------

if(NOT ANDROID)
    install(TARGETS rsdk-air RUNTIME DESTINATION .)
    install(TARGETS SDL2 LIBRARY DESTINATION . NAMELINK_SKIP RUNTIME DESTINATION .)

    install(FILES LICENSE DESTINATION licenses RENAME rsdk-air.txt)
    install(FILES third_party/SDL/LICENSE.txt DESTINATION licenses RENAME SDL2.txt)
    install(FILES third_party/RSDKv3-Decompilation/LICENSE.md DESTINATION licenses RENAME RSDKv3-Decompilation.md)
    install(FILES third_party/RSDKv4-Decompilation/LICENSE.md DESTINATION licenses RENAME RSDKv4-Decompilation.md)
    install(FILES third_party/RSDKv5-Decompilation/LICENSE.md DESTINATION licenses RENAME RSDKv5-Decompilation.md)
    install(FILES third_party/Sonic-Mania-Decompilation/LICENSE.md DESTINATION licenses RENAME Sonic-Mania-Decompilation.md)
    install(FILES third_party/sonic3air/LICENSE DESTINATION licenses RENAME sonic3air-GPL-3.0.txt)
    install(FILES third_party/ogg/COPYING DESTINATION licenses RENAME libogg.txt)
    install(FILES third_party/vorbis/COPYING DESTINATION licenses RENAME libvorbis.txt)
    install(FILES third_party/theora/COPYING DESTINATION licenses RENAME libtheora.txt)
    install(FILES third_party/RSDKv5-Decompilation/dependencies/all/iniparser/LICENSE DESTINATION licenses RENAME iniparser.txt)
    install(FILES third_party/RSDKv5-Decompilation/dependencies/all/miniz/LICENSE.md DESTINATION licenses RENAME miniz.md)
    install(FILES third_party/RSDKv5-Decompilation/dependencies/all/stb_vorbis/LICENSE DESTINATION licenses RENAME stb_vorbis.txt)
    install(FILES third_party/RSDKv3-Decompilation/dependencies/all/theoraplay/LICENSE.txt DESTINATION licenses RENAME theoraplay.txt)
    install(FILES third_party/glew-cmake/doc/glew.txt DESTINATION licenses RENAME GLEW.txt)
    install(FILES third_party/glew-cmake/doc/mesa.txt DESTINATION licenses RENAME GLEW-mesa.txt)
    install(FILES third_party/glew-cmake/doc/khronos.txt DESTINATION licenses RENAME GLEW-khronos.txt)
    install(FILES third_party/RSDKv4-Decompilation/dependencies/all/tinyxml2/LICENSE.txt DESTINATION licenses RENAME tinyxml2.txt)
    install(FILES third_party/stb/LICENSE DESTINATION licenses RENAME stb.txt)
    install(FILES third_party/imgui/LICENSE.txt DESTINATION licenses RENAME imgui.txt)
    install(FILES assets/fonts/Inter-LICENSE.txt DESTINATION licenses RENAME Inter-OFL.txt)

    # Picker art (see art/README.md for where each file comes from) and fonts.
    install(DIRECTORY art/ DESTINATION art)
    install(DIRECTORY assets/fonts/ DESTINATION fonts)
endif()
