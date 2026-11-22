# rsdkv3: RSDKv3 (Sonic CD, 2011 decompilation) as an rsdk-air engine module. Sources come
# unmodified from the pinned submodule, except the patched copy below (paths), and are
# built with our own target instead of the upstream CMakeLists.txt, which makes an
# executable.

set(UP "${PROJECT_SOURCE_DIR}/third_party/RSDKv3-Decompilation")
if(NOT EXISTS "${UP}/dependencies/all/tinyxml2/tinyxml2.cpp" OR NOT EXISTS "${UP}/dependencies/all/theoraplay/theoraplay.c")
    message(FATAL_ERROR "RSDKv3 submodules missing: run 'git submodule update --init --recursive'")
endif()

# --- patches ------------------------------------------------------------------------
# InitUserdata: where settings.ini and the saves live. On macOS and Android upstream asks
# the app bundle / the Java activity; here the module decides (rsdkair_rsdkv3_game_path,
# rsdkv3_module.cpp), so every platform uses the game's save folder.
rsdkair_patch_begin(userdata "${UP}/RSDKv3/Userdata.cpp")
rsdkair_patch_replace(userdata [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

const char *rsdkair_rsdkv3_game_path(); // rsdk-air: rsdkv3_module.cpp]=])
rsdkair_patch_replace(userdata [=[    getResourcesPath(gamePath, sizeof(gamePath));]=]
[=[    sprintf(gamePath, "%s", rsdkair_rsdkv3_game_path()); // rsdk-air]=])
rsdkair_patch_replace(userdata [=[        JNIEnv *env      = (JNIEnv *)SDL_AndroidGetJNIEnv();
        jobject activity = (jobject)SDL_AndroidGetActivity();
        jclass cls(env->GetObjectClass(activity));
        jmethodID method = env->GetMethodID(cls, "getBasePath", "()Ljava/lang/String;");
        auto ret         = env->CallObjectMethod(activity, method);

        strcpy(buffer, env->GetStringUTFChars((jstring)ret, NULL));]=]
[=[        strcpy(buffer, rsdkair_rsdkv3_game_path()); // rsdk-air]=])
rsdkair_patch_replace(userdata [=[
        env->DeleteLocalRef(activity);
        env->DeleteLocalRef(cls);
    }
#endif]=]
[=[
    }
#endif]=])
rsdkair_patch_end(userdata RSDKV3_USERDATA_CPP)

# The in-game menu (engines/common/menu_glue.hpp): every polled SDL event goes to the
# menu first, and each frame calls it just before the swap.
rsdkair_patch_begin(engine "${UP}/RSDKv3/RetroEngine.cpp")
rsdkair_patch_replace(engine [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

int rsdkair_rsdkv3_menu_event(const void *event); // rsdk-air: rsdkv3_module.cpp
void rsdkair_rsdkv3_menu_frame();]=])
rsdkair_patch_replace(engine [=[    while (SDL_PollEvent(&Engine.sdlEvents)) {]=]
[=[    while (SDL_PollEvent(&Engine.sdlEvents)) {
        if (rsdkair_rsdkv3_menu_event(&Engine.sdlEvents)) // rsdk-air: the in-game menu took it
            continue;]=])
rsdkair_patch_replace(engine [=[        SDL_GL_SwapWindow(Engine.window);]=]
[=[        rsdkair_rsdkv3_menu_frame(); // rsdk-air: may open the in-game menu
        SDL_GL_SwapWindow(Engine.window);]=])
# The frame cap stays on with VSync too (upstream drops it, so a 120/144 Hz monitor would
# run the game 2-2.4 times too fast); VSync then only removes tearing.
rsdkair_patch_replace(engine [=[        if (!vsync) {]=]
[=[        { // rsdk-air: 60 FPS cap with or without VSync]=])
rsdkair_patch_end(engine RSDKV3_RETROENGINE_CPP)

# On-screen controls (drawn by the in-game menu): their buttons count as held, like a key
# or a pad button. The game's own touch controls go with its disableTouchControls flag.
rsdkair_patch_begin(input "${UP}/RSDKv3/Input.cpp")
rsdkair_patch_replace(input [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

bool rsdkair_rsdkv3_pad_held(int button); // rsdk-air: rsdkv3_module.cpp]=])
rsdkair_patch_replace(input [=[            if (keyState[inputDevice[i].keyMappings]) {
                inputDevice[i].setHeld();
                if (!inputDevice[INPUT_ANY].hold)]=]
[=[            if (keyState[inputDevice[i].keyMappings] || rsdkair_rsdkv3_pad_held(i)) { // rsdk-air
                inputDevice[i].setHeld();
                if (!inputDevice[INPUT_ANY].hold)]=])
rsdkair_patch_replace(input [=[            if (getControllerButton(inputDevice[i].contMappings)) {]=]
[=[            if (getControllerButton(inputDevice[i].contMappings) || rsdkair_rsdkv3_pad_held(i)) { // rsdk-air]=])
rsdkair_patch_end(input RSDKV3_INPUT_CPP)

set(RSDKV3_SOURCES
    ${UP}/dependencies/all/tinyxml2/tinyxml2.cpp
    ${UP}/dependencies/all/theoraplay/theoraplay.c
    ${UP}/RSDKv3/Animation.cpp
    ${UP}/RSDKv3/Audio.cpp
    ${UP}/RSDKv3/Collision.cpp
    ${UP}/RSDKv3/Debug.cpp
    ${UP}/RSDKv3/Drawing.cpp
    ${UP}/RSDKv3/Ini.cpp
    ${RSDKV3_INPUT_CPP}
    ${UP}/RSDKv3/fcaseopen.c
    ${UP}/RSDKv3/Math.cpp
    ${UP}/RSDKv3/ModAPI.cpp
    ${UP}/RSDKv3/Object.cpp
    ${UP}/RSDKv3/Palette.cpp
    ${UP}/RSDKv3/Player.cpp
    ${UP}/RSDKv3/Reader.cpp
    ${RSDKV3_RETROENGINE_CPP}
    ${UP}/RSDKv3/Scene.cpp
    ${UP}/RSDKv3/Scene3D.cpp
    ${UP}/RSDKv3/Script.cpp
    ${UP}/RSDKv3/Sprite.cpp
    ${UP}/RSDKv3/String.cpp
    ${UP}/RSDKv3/Text.cpp
    ${UP}/RSDKv3/Video.cpp
    ${RSDKV3_USERDATA_CPP}
)

rsdkair_engine_module(rsdkv3
    rsdkv3_module.cpp
    ${RSDKV3_SOURCES})

# Upstream builds with C++17 (the mod loader needs std::filesystem).
set_target_properties(rsdkv3 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_include_directories(rsdkv3 PRIVATE
    "${PROJECT_SOURCE_DIR}/engines/common"
    "${UP}/RSDKv3"
    "${UP}/dependencies/all/theoraplay"
    "${UP}/dependencies/all/tinyxml2")
if(APPLE)
    target_include_directories(rsdkv3 PRIVATE "${UP}/dependencies/mac")  # cocoaHelpers.hpp
endif()

target_compile_definitions(rsdkv3 PRIVATE
    RETRO_USE_MOD_LOADER=1
    RETRO_USING_OPENGL=1
    RETRO_USE_SDL2=1
    FORCE_CASE_INSENSITIVE=0
    RETRO_USE_ORIGINAL_CODE=0
    RSDK_AUTOBUILD=$<BOOL:${RSDKAIR_RETRO_DISABLE_PLUS}>
    "RETRO_DEV_EXTRA=\"rsdk-air ${RSDKAIR_VERSION}\""
    "DECOMP_VERSION=\"1.3.3\"")

find_package(Threads REQUIRED)  # theoraplay decodes on its own thread
target_link_libraries(rsdkv3 PRIVATE rsdkair_core rsdkair_vorbis rsdkair_theora SDL2::SDL2 Threads::Threads)
if(ANDROID)
    target_link_libraries(rsdkv3 PRIVATE android log EGL GLESv1_CM OpenSLES z)
elseif(APPLE)
    target_link_libraries(rsdkv3 PRIVATE "-framework OpenGL" "-framework Foundation")
else()
    target_link_libraries(rsdkv3 PRIVATE rsdkair_glew)
endif()
if(WIN32)
    target_link_libraries(rsdkv3 PRIVATE winmm comctl32)
    target_compile_definitions(rsdkv3 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Upstream code is not ours to fix: no warnings from it.
if(MSVC)
    target_compile_options(rsdkv3 PRIVATE /w /utf-8)
else()
    target_compile_options(rsdkv3 PRIVATE -w)
endif()
