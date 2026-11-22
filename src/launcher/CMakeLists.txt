# The rsdk-air executable. On Android it is libmain.so, which SDLActivity loads and whose
# SDL_main it calls; on desktop it is a normal executable.

# Dear ImGui (SDL2 + SDL_Renderer backends) and stb_image, for the picker. Third-party
# code, so no rsdkair_warnings() here.
set(imgui_dir "${PROJECT_SOURCE_DIR}/third_party/imgui")
add_library(rsdkair_ui_deps STATIC
    "${imgui_dir}/imgui.cpp"
    "${imgui_dir}/imgui_draw.cpp"
    "${imgui_dir}/imgui_tables.cpp"
    "${imgui_dir}/imgui_widgets.cpp"
    "${imgui_dir}/backends/imgui_impl_sdl2.cpp"
    "${imgui_dir}/backends/imgui_impl_sdlrenderer2.cpp"
    stb_image_impl.c)
target_include_directories(rsdkair_ui_deps PUBLIC
    "${imgui_dir}" "${imgui_dir}/backends" "${PROJECT_SOURCE_DIR}/third_party/stb")
target_compile_definitions(rsdkair_ui_deps PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(rsdkair_ui_deps PUBLIC SDL2::SDL2)
set_target_properties(rsdkair_ui_deps PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(launcher_sources main.cpp picker.cpp game_menu.cpp)
if(ANDROID)
    add_library(rsdk-air SHARED ${launcher_sources})
    set_target_properties(rsdk-air PROPERTIES OUTPUT_NAME main)
    target_link_libraries(rsdk-air PRIVATE android log)
else()
    add_executable(rsdk-air ${launcher_sources})
    if(TARGET SDL2::SDL2main)
        target_link_libraries(rsdk-air PRIVATE SDL2::SDL2main)
    endif()
endif()

target_link_libraries(rsdk-air PRIVATE rsdkair_core rsdkair_ui_deps SDL2::SDL2)

# The in-game menu (game_menu.cpp) draws through the engine's fixed-function OpenGL 1.x
# context (RSDKv3/v4): desktop OpenGL, or OpenGL ES 1 (GLESv1_CM) on Android.
if(ANDROID)
    target_link_libraries(rsdk-air PRIVATE GLESv1_CM)
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(rsdk-air PRIVATE OpenGL::GL)
endif()

# The picker's art and fonts, next to the executable in the build tree (the install rules
# and the Android assets put them in the same relative places).
if(NOT ANDROID)
    add_custom_command(TARGET rsdk-air POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/art" "$<TARGET_FILE_DIR:rsdk-air>/art"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:rsdk-air>/fonts"
        VERBATIM)
endif()
rsdkair_warnings(rsdk-air)

# A stand-in engine for trying the in-game menu without game data (menu_demo.cpp).
if(RSDKAIR_BUILD_TESTS AND NOT ANDROID)
    add_executable(rsdkair_menu_demo menu_demo.cpp game_menu.cpp)
    target_link_libraries(rsdkair_menu_demo PRIVATE rsdkair_core rsdkair_ui_deps SDL2::SDL2 OpenGL::GL)
    rsdkair_warnings(rsdkair_menu_demo)
endif()
