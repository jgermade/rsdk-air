# The rsdk-air executable. On Android it is libmain.so, which SDLActivity loads and whose
# SDL_main it calls; on desktop it is a normal executable.
if(ANDROID)
    add_library(rsdk-air SHARED main.cpp)
    set_target_properties(rsdk-air PROPERTIES OUTPUT_NAME main)
    target_link_libraries(rsdk-air PRIVATE android log)
else()
    add_executable(rsdk-air main.cpp)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(rsdk-air PRIVATE SDL2::SDL2main)
    endif()
endif()

target_link_libraries(rsdk-air PRIVATE rsdkair_core SDL2::SDL2)
rsdkair_warnings(rsdk-air)
