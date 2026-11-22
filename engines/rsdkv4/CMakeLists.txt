# rsdkv4: RSDKv4 (Sonic the Hedgehog / Sonic the Hedgehog 2, 2013 decompilation) as an
# rsdk-air engine module. Sources come unmodified from the pinned submodule, except the
# few patched copies below (paths), and are built with our own target instead of the
# upstream CMakeLists.txt, which makes an executable.

set(UP "${PROJECT_SOURCE_DIR}/third_party/RSDKv4-Decompilation")
if(NOT EXISTS "${UP}/dependencies/all/tinyxml2/tinyxml2.cpp" OR NOT EXISTS "${UP}/dependencies/all/stb-image/stb_image.h")
    message(FATAL_ERROR "RSDKv4 submodules missing: run 'git submodule update --init --recursive'")
endif()

# --- patches ------------------------------------------------------------------------
# InitUserdata: where settings.ini and the saves live. On macOS and Android upstream asks
# the app bundle / the Java activity; here the module decides (rsdkair_rsdkv4_game_path,
# rsdkv4_module.cpp), so every platform uses the game's save folder.
rsdkair_patch_begin(userdata "${UP}/RSDKv4/Userdata.cpp")
rsdkair_patch_replace(userdata [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

const char *rsdkair_rsdkv4_game_path(); // rsdk-air: rsdkv4_module.cpp]=])
rsdkair_patch_replace(userdata [=[    sprintf(gamePath, "%s/RSDKv4", getResourcesPath());
    sprintf(modsPath, "%s/RSDKv4/", getResourcesPath());]=]
[=[    sprintf(gamePath, "%s", rsdkair_rsdkv4_game_path()); // rsdk-air
    sprintf(modsPath, "%s/", rsdkair_rsdkv4_game_path());]=])
rsdkair_patch_replace(userdata [=[        JNIEnv *env      = (JNIEnv *)SDL_AndroidGetJNIEnv();
        jobject activity = (jobject)SDL_AndroidGetActivity();
        jclass cls(env->GetObjectClass(activity));
        jmethodID method = env->GetMethodID(cls, "getBasePath", "()Ljava/lang/String;");
        auto ret         = env->CallObjectMethod(activity, method);

        strcpy(buffer, env->GetStringUTFChars((jstring)ret, NULL));]=]
[=[        strcpy(buffer, rsdkair_rsdkv4_game_path()); // rsdk-air]=])
rsdkair_patch_replace(userdata [=[        env->DeleteLocalRef(activity);
        env->DeleteLocalRef(cls);
    }
#endif

    char buffer[0x100];]=]
[=[    }
#endif

    char buffer[0x100];]=])
rsdkair_patch_end(userdata RSDKV4_USERDATA_CPP)

# The in-game menu (engines/common/menu_glue.hpp): every polled SDL event goes to the
# menu first, and each frame calls it just before the swap.
rsdkair_patch_begin(engine "${UP}/RSDKv4/RetroEngine.cpp")
rsdkair_patch_replace(engine [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

int rsdkair_rsdkv4_menu_event(const void *event); // rsdk-air: rsdkv4_module.cpp
void rsdkair_rsdkv4_menu_frame();]=])
rsdkair_patch_replace(engine [=[    while (SDL_PollEvent(&Engine.sdlEvents)) {]=]
[=[    while (SDL_PollEvent(&Engine.sdlEvents)) {
        if (rsdkair_rsdkv4_menu_event(&Engine.sdlEvents)) // rsdk-air: the in-game menu took it
            continue;]=])
rsdkair_patch_replace(engine [=[                SDL_GL_SwapWindow(Engine.window);]=]
[=[                rsdkair_rsdkv4_menu_frame(); // rsdk-air: may open the in-game menu
                SDL_GL_SwapWindow(Engine.window);]=])
rsdkair_patch_end(engine RSDKV4_RETROENGINE_CPP)

# The game's own pause (the touch pause button, Start, losing focus) sets ENGINE_INITPAUSE,
# which RetroGameLoop answers with the native vertical pause menu. It opens the in-game
# menu instead; the last frame is drawn again so the menu has it as its backdrop.
rsdkair_patch_begin(gameloop "${UP}/RSDKv4/NativeObjects/RetroGameLoop.cpp")
rsdkair_patch_replace(gameloop [=[#include "RetroEngine.hpp"]=]
[=[#include "RetroEngine.hpp"

void rsdkair_rsdkv4_menu_open(); // rsdk-air: rsdkv4_module.cpp]=])
rsdkair_patch_replace(gameloop [=[        case ENGINE_INITPAUSE:
            mixFiltersOnJekyll = false;
            InitPauseMenu();
            break;]=]
[=[        case ENGINE_INITPAUSE: // rsdk-air: the in-game menu, not the native pause menu
            Engine.gameMode = ENGINE_MAINGAME;
            TransferRetroBuffer();
            RenderRetroBuffer(64, 160.0);
            rsdkair_rsdkv4_menu_open();
            break;]=])
rsdkair_patch_end(gameloop RSDKV4_RETROGAMELOOP_CPP)

set(RSDKV4_SOURCES
    ${UP}/dependencies/all/tinyxml2/tinyxml2.cpp
    ${UP}/RSDKv4/Animation.cpp
    ${UP}/RSDKv4/Audio.cpp
    ${UP}/RSDKv4/Collision.cpp
    ${UP}/RSDKv4/Debug.cpp
    ${UP}/RSDKv4/Drawing.cpp
    ${UP}/RSDKv4/Ini.cpp
    ${UP}/RSDKv4/Input.cpp
    ${UP}/RSDKv4/fcaseopen.c
    ${UP}/RSDKv4/Math.cpp
    ${UP}/RSDKv4/ModAPI.cpp
    ${UP}/RSDKv4/Networking.cpp
    ${UP}/RSDKv4/Object.cpp
    ${UP}/RSDKv4/Palette.cpp
    ${UP}/RSDKv4/Reader.cpp
    ${UP}/RSDKv4/Renderer.cpp
    ${RSDKV4_RETROENGINE_CPP}
    ${UP}/RSDKv4/Scene.cpp
    ${UP}/RSDKv4/Scene3D.cpp
    ${UP}/RSDKv4/Script.cpp
    ${UP}/RSDKv4/Sprite.cpp
    ${UP}/RSDKv4/String.cpp
    ${UP}/RSDKv4/Text.cpp
    ${RSDKV4_USERDATA_CPP}
)
foreach(object
        AboutScreen AchievementDisplay AchievementsButton AchievementsMenu BackButton CWSplash
        CreditText DialogPanel FadeScreen InstructionsScreen LeaderboardsButton MenuBG MenuControl
        ModInfoButton ModsButton ModsMenu MultiplayerButton MultiplayerHandler MultiplayerScreen
        OptionsButton OptionsMenu PauseMenu PlayerSelectScreen PushButton RecordsScreen
        RetroGameLoop SaveSelect SegaIDButton SegaSplash SettingsScreen StaffCredits
        StartGameButton SubMenuButton TextLabel TimeAttack TimeAttackButton TitleScreen VirtualDPad
        VirtualDPadM ZoneButton)
    list(APPEND RSDKV4_SOURCES "${UP}/RSDKv4/NativeObjects/${object}.cpp")
endforeach()
list(TRANSFORM RSDKV4_SOURCES REPLACE ".*/NativeObjects/RetroGameLoop\\.cpp$" "${RSDKV4_RETROGAMELOOP_CPP}")

rsdkair_engine_module(rsdkv4
    rsdkv4_module.cpp
    rsdkv4_bridge.cpp
    ${RSDKV4_SOURCES})

# Upstream builds with C++17 (the mod loader needs std::filesystem).
set_target_properties(rsdkv4 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_include_directories(rsdkv4 PRIVATE
    "${PROJECT_SOURCE_DIR}/engines/common"
    "${UP}/RSDKv4"
    "${UP}/RSDKv4/NativeObjects"
    "${UP}/dependencies/all/stb-image"
    "${UP}/dependencies/all/tinyxml2")
if(APPLE)
    target_include_directories(rsdkv4 PRIVATE "${UP}/dependencies/mac")  # cocoaHelpers.hpp
endif()

target_compile_definitions(rsdkv4 PRIVATE
    RSDK_REVISION=3
    RETRO_USE_MOD_LOADER=1
    RETRO_USE_NETWORKING=0
    RETRO_USING_OPENGL=1
    RETRO_USE_SDL2=1
    FORCE_CASE_INSENSITIVE=0
    RETRO_USE_ORIGINAL_CODE=0
    RSDK_AUTOBUILD=$<BOOL:${RSDKAIR_RETRO_DISABLE_PLUS}>
    "RETRO_DEV_EXTRA=\"rsdk-air ${RSDKAIR_VERSION}\""
    "DECOMP_VERSION=\"1.3.3\"")

target_link_libraries(rsdkv4 PRIVATE rsdkair_core rsdkair_vorbis SDL2::SDL2)
if(ANDROID)
    target_link_libraries(rsdkv4 PRIVATE android log EGL GLESv1_CM OpenSLES z)
elseif(APPLE)
    target_link_libraries(rsdkv4 PRIVATE "-framework OpenGL" "-framework Foundation")
else()
    target_link_libraries(rsdkv4 PRIVATE rsdkair_glew)
endif()
if(WIN32)
    target_link_libraries(rsdkv4 PRIVATE winmm comctl32)
    target_compile_definitions(rsdkv4 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Upstream code is not ours to fix: no warnings from it.
if(MSVC)
    target_compile_options(rsdkv4 PRIVATE /w /utf-8)
else()
    target_compile_options(rsdkv4 PRIVATE -w)
endif()
