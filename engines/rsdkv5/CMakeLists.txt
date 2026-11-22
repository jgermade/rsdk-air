# rsdkv5: RSDKv5U with Sonic Mania's game logic linked in, as an rsdk-air engine module.
# Sources come unmodified from the pinned submodules (RSDKv5-Decompilation and
# Sonic-Mania-Decompilation), except the patched copy below (paths), and are built with
# our own target: upstream makes an executable that loads the game logic as Game.dll /
# libGame.so; here the logic is linked statically (upstream's GAME_STATIC mode).
#
# Subsystem: SDL2 for video, audio and input everywhere. On Android upstream only supports
# its own EGL + GameActivity + Paddleboat + Oboe path, which does not fit our
# SDLActivity-based app, so there the engine is built as its Linux flavour (SDL2 subsystem,
# plain files in the save folder, no JNI): see the RetroEngine.hpp patch below.

set(UP "${PROJECT_SOURCE_DIR}/third_party/RSDKv5-Decompilation")
set(MANIA "${PROJECT_SOURCE_DIR}/third_party/Sonic-Mania-Decompilation")
if(NOT EXISTS "${UP}/dependencies/all/tinyxml2/tinyxml2.cpp" OR NOT EXISTS "${UP}/dependencies/all/stb_vorbis/stb_vorbis.c")
    message(FATAL_ERROR "RSDKv5 submodules missing: run 'git submodule update --init --recursive'")
endif()
if(NOT EXISTS "${MANIA}/SonicMania/Game.c")
    message(FATAL_ERROR "Sonic-Mania-Decompilation missing: run 'git submodule update --init --recursive'")
endif()

# --- patches ------------------------------------------------------------------------
# InitUserDirectory: on macOS upstream uses <app resources>/RSDKv5/; here the module's
# save folder (rsdkair_rsdkv5_user_dir, rsdkv5_module.cpp). Linux and Windows already use
# the working directory, which the module sets to the save folder. Android (built as
# Linux) gets the absolute folder too: the engine opens files with SDL_RWFromFile, and SDL
# on Android resolves relative paths against the app's internal storage, not the working
# directory, so "./" + "../../games/mania/Data.rsdk" pointed outside the data root.
rsdkair_patch_begin(storage "${UP}/RSDKv5/RSDK/User/Core/UserStorage.cpp")
rsdkair_patch_replace(storage [=[void RSDK::SKU::InitUserDirectory()]=]
[=[const char *rsdkair_rsdkv5_user_dir(); // rsdk-air: rsdkv5_module.cpp

void RSDK::SKU::InitUserDirectory()]=])
rsdkair_patch_replace(storage [=[    sprintf_s(buffer, sizeof(buffer), "%s/RSDKv5/", getResourcesPath());]=]
[=[    sprintf_s(buffer, sizeof(buffer), "%s", rsdkair_rsdkv5_user_dir()); // rsdk-air]=])
rsdkair_patch_replace(storage [=[    SKU::SetUserFileCallbacks("./", NULL, NULL);]=]
[=[#if defined(RSDKAIR_ANDROID_AS_LINUX) // rsdk-air: SDL reads relative paths from internal storage
    SKU::SetUserFileCallbacks(rsdkair_rsdkv5_user_dir(), NULL, NULL);
#else
    SKU::SetUserFileCallbacks("./", NULL, NULL);
#endif]=])
rsdkair_patch_end(storage RSDKV5_USERSTORAGE_CPP)

# RetroEngine.hpp (Android only): pick the Linux platform instead of Android, so the SDL2
# render, audio and input devices are used and none of upstream's GameActivity/JNI code
# (androidHelpers.cpp, Java-side file access) is needed. The patched copy shadows the
# original through an include directory searched first; both share the RETROENGINE_H guard.
if(ANDROID)
    rsdkair_patch_begin(engine_hpp "${UP}/RSDKv5/RSDK/Core/RetroEngine.hpp")
    rsdkair_patch_replace(engine_hpp [=[#elif defined __ANDROID__
#define RETRO_PLATFORM   (RETRO_ANDROID)]=]
[=[#elif defined __ANDROID__ && !defined RSDKAIR_ANDROID_AS_LINUX // rsdk-air
#define RETRO_PLATFORM   (RETRO_ANDROID)]=])
    rsdkair_patch_end(engine_hpp RSDKV5_ENGINE_HPP)
    # iniparser.cpp keeps upstream's Android fOpen (Java-side, androidHelpers.cpp) on
    # Android; as on Linux, plain fopen is what reads Settings.ini here.
    rsdkair_patch_begin(iniparser "${UP}/dependencies/all/iniparser/iniparser.cpp")
    rsdkair_patch_replace(iniparser [=[#ifndef __ANDROID__
#undef fOpen]=]
[=[#if !defined(__ANDROID__) || defined(RSDKAIR_ANDROID_AS_LINUX) // rsdk-air
#undef fOpen]=])
    rsdkair_patch_end(iniparser RSDKV5_INIPARSER_CPP)

    set(RSDKV5_SHADOW_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/shadow-include")
    configure_file("${RSDKV5_ENGINE_HPP}" "${RSDKV5_SHADOW_INCLUDE}/RSDK/Core/RetroEngine.hpp" COPYONLY)
endif()

# The in-game menu (engines/common/menu_glue.hpp): every polled SDL event goes to the menu
# first, and each frame calls it just before SDL_RenderPresent. The SDL2 render device is
# #included by Drawing.cpp, so Drawing.cpp is patched to include the patched copy.
rsdkair_patch_begin(sdl2rd "${UP}/RSDKv5/RSDK/Graphics/SDL2/SDL2RenderDevice.cpp")
rsdkair_patch_replace(sdl2rd [=[SDL_Window *RenderDevice::window     = nullptr;]=]
[=[int rsdkair_rsdkv5_menu_event(const void *event); // rsdk-air: rsdkv5_module.cpp
void rsdkair_rsdkv5_menu_frame();

SDL_Window *RenderDevice::window     = nullptr;]=])
rsdkair_patch_replace(sdl2rd [=[    while (SDL_PollEvent(&sdlEvent)) {
        ProcessEvent(sdlEvent);]=]
[=[    while (SDL_PollEvent(&sdlEvent)) {
        if (rsdkair_rsdkv5_menu_event(&sdlEvent)) // rsdk-air: the in-game menu took it
            continue;
        ProcessEvent(sdlEvent);]=])
rsdkair_patch_replace(sdl2rd [=[    // no change here
    SDL_RenderPresent(renderer);]=]
[=[    // no change here
    rsdkair_rsdkv5_menu_frame(); // rsdk-air: may open the in-game menu
    SDL_RenderPresent(renderer);]=])
rsdkair_patch_end(sdl2rd RSDKV5_SDL2RENDERDEVICE_CPP)
rsdkair_patch_begin(drawing "${UP}/RSDKv5/RSDK/Graphics/Drawing.cpp")
rsdkair_patch_replace(drawing [=[#include "SDL2/SDL2RenderDevice.cpp"]=]
"#include \"${RSDKV5_SDL2RENDERDEVICE_CPP}\" // rsdk-air: patched copy")
rsdkair_patch_end(drawing RSDKV5_DRAWING_CPP)

set(V5 "${UP}/RSDKv5")
set(RSDKV5_SOURCES
    ${V5}/RSDK/Core/RetroEngine.cpp
    ${V5}/RSDK/Core/Math.cpp
    ${V5}/RSDK/Core/Reader.cpp
    ${V5}/RSDK/Core/Link.cpp
    ${V5}/RSDK/Core/ModAPI.cpp
    ${V5}/RSDK/Dev/Debug.cpp
    ${V5}/RSDK/Storage/Storage.cpp
    ${V5}/RSDK/Storage/Text.cpp
    ${RSDKV5_DRAWING_CPP}
    ${V5}/RSDK/Graphics/Scene3D.cpp
    ${V5}/RSDK/Graphics/Animation.cpp
    ${V5}/RSDK/Graphics/Sprite.cpp
    ${V5}/RSDK/Graphics/Palette.cpp
    ${V5}/RSDK/Graphics/Video.cpp
    ${V5}/RSDK/Audio/Audio.cpp
    ${V5}/RSDK/Input/Input.cpp
    ${V5}/RSDK/Scene/Scene.cpp
    ${V5}/RSDK/Scene/Collision.cpp
    ${V5}/RSDK/Scene/Object.cpp
    ${V5}/RSDK/Scene/Objects/DefaultObject.cpp
    ${V5}/RSDK/Scene/Objects/DevOutput.cpp
    ${V5}/RSDK/User/Core/UserAchievements.cpp
    ${V5}/RSDK/User/Core/UserCore.cpp
    ${V5}/RSDK/User/Core/UserLeaderboards.cpp
    ${V5}/RSDK/User/Core/UserPresence.cpp
    ${V5}/RSDK/User/Core/UserStats.cpp
    ${RSDKV5_USERSTORAGE_CPP}
    ${UP}/dependencies/all/tinyxml2/tinyxml2.cpp
    ${UP}/dependencies/all/iniparser/iniparser.cpp
    ${UP}/dependencies/all/iniparser/dictionary.cpp
    ${UP}/dependencies/all/miniz/miniz.c
)
if(ANDROID)
    list(TRANSFORM RSDKV5_SOURCES REPLACE ".*/iniparser/iniparser\\.cpp$" "${RSDKV5_INIPARSER_CPP}")
endif()

# Sonic Mania's game logic: plain C, every object in one translation unit (All.c).
add_library(rsdkair_mania_logic STATIC
    ${MANIA}/SonicMania/Game.c
    ${MANIA}/SonicMania/Objects/All.c)
target_include_directories(rsdkair_mania_logic PRIVATE
    ${MANIA}/SonicMania
    ${MANIA}/SonicMania/Objects)
target_compile_definitions(rsdkair_mania_logic PRIVATE
    RETRO_REVISION=3
    RETRO_USE_MOD_LOADER=1
    RETRO_MOD_LOADER_VER=2
    GAME_INCLUDE_EDITOR=0
    MANIA_PREPLUS=0
    MANIA_FIRST_RELEASE=0
    GAME_VERSION=6)
set_target_properties(rsdkair_mania_logic PROPERTIES
    C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(rsdkair_mania_logic PRIVATE /w)
    target_compile_definitions(rsdkair_mania_logic PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(rsdkair_mania_logic PRIVATE -w)
endif()

rsdkair_engine_module(rsdkv5
    rsdkv5_module.cpp
    ${RSDKV5_SOURCES})

# The mod loader needs C++17 (std::filesystem).
set_target_properties(rsdkv5 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    # The copy's own relative includes ("Link.hpp", "Legacy/...") resolve from RSDK/Core.
    target_include_directories(rsdkv5 BEFORE PRIVATE "${RSDKV5_SHADOW_INCLUDE}" "${V5}/RSDK/Core")
    target_compile_definitions(rsdkv5 PRIVATE RSDKAIR_ANDROID_AS_LINUX=1)
endif()
target_include_directories(rsdkv5 PRIVATE
    "${PROJECT_SOURCE_DIR}/engines/common"
    "${V5}/RSDK/Graphics"  # the patched Drawing.cpp's relative includes (Legacy/, SDL2/…)
    "${V5}"
    "${UP}/dependencies/all"
    "${UP}/dependencies/all/tinyxml2"
    "${UP}/dependencies/all/iniparser"
    "${MANIA}/SonicMania")  # GameMain.h
if(APPLE)
    target_include_directories(rsdkv5 PRIVATE "${UP}/dependencies/mac")  # cocoaHelpers.hpp
endif()

target_compile_definitions(rsdkv5 PRIVATE
    RETRO_REVISION=3
    RSDK_USE_SDL2=1
    RETRO_USE_MOD_LOADER=1
    RETRO_MOD_LOADER_VER=2
    RETRO_STANDALONE=0
    RSDK_AUTOBUILD=$<BOOL:${RSDKAIR_RETRO_DISABLE_PLUS}>
    RETRO_DISABLE_LOG=0
    "RETRO_DEV_EXTRA=\"rsdk-air ${RSDKAIR_VERSION}\"")

find_package(Threads REQUIRED)  # audio and video threads
target_link_libraries(rsdkv5 PRIVATE rsdkair_core rsdkair_mania_logic rsdkair_theora SDL2::SDL2 Threads::Threads)
if(APPLE)
    target_link_libraries(rsdkv5 PRIVATE "-framework Foundation")
endif()
if(WIN32)
    target_compile_definitions(rsdkv5 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Upstream code is not ours to fix: no warnings from it.
if(MSVC)
    target_compile_options(rsdkv5 PRIVATE /w /utf-8)
else()
    target_compile_options(rsdkv5 PRIVATE -w)
endif()
