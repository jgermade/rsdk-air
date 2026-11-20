# Helpers shared by the launcher and the engine modules.

# Warnings for our own code (never applied to third-party sources).
function(rsdkair_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# rsdkair_engine_module(<name> <sources>...)
#
# Builds an engine module: a shared library loaded with dlopen/LoadLibrary that
# exports only the rsdkair_engine_* functions (include/rsdkair/engine.h). Everything
# else, including statically linked dependencies, stays hidden, so two engines with the
# same global names never clash.
#
# Output: bin/engines/<name>.{dll,so,dylib} on desktop, lib<name>.so on Android (the name
# Android packages and System.loadLibrary expect).
function(rsdkair_engine_module name)
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE rsdkair_abi)
    set_target_properties(${name} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY "${RSDKAIR_ENGINES_OUTPUT_DIR}/$<0:>"
        RUNTIME_OUTPUT_DIRECTORY "${RSDKAIR_ENGINES_OUTPUT_DIR}/$<0:>")
    if(NOT ANDROID)
        set_target_properties(${name} PROPERTIES PREFIX "")
    endif()
    if(APPLE)
        set_target_properties(${name} PROPERTIES INSTALL_RPATH "@loader_path/..")
        target_link_options(${name} PRIVATE "LINKER:-undefined,error")
    elseif(UNIX)
        set_target_properties(${name} PROPERTIES INSTALL_RPATH "$ORIGIN/..")
        target_link_options(${name} PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined")
    endif()
    if(NOT ANDROID)
        install(TARGETS ${name} LIBRARY DESTINATION engines RUNTIME DESTINATION engines)
    endif()
endfunction()
