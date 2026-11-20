# Libraries the engine modules link statically: libogg, libvorbis and (desktop
# Windows/Linux) GLEW. Built from the pinned submodules in third_party/, as static PIC
# libraries; each module hides their symbols (cmake/RsdkAir.cmake).

set(RSDKAIR_THIRD_PARTY "${PROJECT_SOURCE_DIR}/third_party")

# --- libogg -------------------------------------------------------------------------
set(INCLUDE_INTTYPES_H 0)
set(INCLUDE_STDINT_H 1)
set(INCLUDE_SYS_TYPES_H 0)
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
set(SIZE64 int64_t)
set(USIZE64 uint64_t)
configure_file("${RSDKAIR_THIRD_PARTY}/ogg/include/ogg/config_types.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/ogg/include/ogg/config_types.h" @ONLY)

add_library(rsdkair_ogg STATIC
    "${RSDKAIR_THIRD_PARTY}/ogg/src/bitwise.c"
    "${RSDKAIR_THIRD_PARTY}/ogg/src/framing.c")
target_include_directories(rsdkair_ogg PUBLIC
    "${RSDKAIR_THIRD_PARTY}/ogg/include"
    "${CMAKE_CURRENT_BINARY_DIR}/ogg/include")

# --- libvorbis ----------------------------------------------------------------------
set(VORBIS_LIB "${RSDKAIR_THIRD_PARTY}/vorbis/lib")
add_library(rsdkair_vorbis STATIC
    ${VORBIS_LIB}/analysis.c ${VORBIS_LIB}/bitrate.c ${VORBIS_LIB}/block.c
    ${VORBIS_LIB}/codebook.c ${VORBIS_LIB}/envelope.c ${VORBIS_LIB}/floor0.c
    ${VORBIS_LIB}/floor1.c ${VORBIS_LIB}/info.c ${VORBIS_LIB}/lookup.c ${VORBIS_LIB}/lpc.c
    ${VORBIS_LIB}/lsp.c ${VORBIS_LIB}/mapping0.c ${VORBIS_LIB}/mdct.c ${VORBIS_LIB}/psy.c
    ${VORBIS_LIB}/registry.c ${VORBIS_LIB}/res0.c ${VORBIS_LIB}/sharedbook.c
    ${VORBIS_LIB}/smallft.c ${VORBIS_LIB}/synthesis.c ${VORBIS_LIB}/vorbisenc.c
    ${VORBIS_LIB}/vorbisfile.c ${VORBIS_LIB}/window.c)
target_include_directories(rsdkair_vorbis
    PUBLIC "${RSDKAIR_THIRD_PARTY}/vorbis/include"
    PRIVATE "${VORBIS_LIB}")
target_link_libraries(rsdkair_vorbis PUBLIC rsdkair_ogg)
if(NOT WIN32)
    target_link_libraries(rsdkair_vorbis PUBLIC m)
endif()

foreach(lib rsdkair_ogg rsdkair_vorbis)
    set_target_properties(${lib} PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(${lib} PRIVATE /w)
        target_compile_definitions(${lib} PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    else()
        target_compile_options(${lib} PRIVATE -w)
    endif()
endforeach()

# --- GLEW (Windows and Linux desktop OpenGL) ----------------------------------------
if(NOT ANDROID AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    add_library(rsdkair_glew STATIC "${RSDKAIR_THIRD_PARTY}/glew-cmake/src/glew.c")
    target_include_directories(rsdkair_glew PUBLIC "${RSDKAIR_THIRD_PARTY}/glew-cmake/include")
    target_compile_definitions(rsdkair_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
    target_link_libraries(rsdkair_glew PUBLIC OpenGL::GL)
    set_target_properties(rsdkair_glew PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(rsdkair_glew PRIVATE /w)
    else()
        target_compile_options(rsdkair_glew PRIVATE -w)
    endif()
endif()
