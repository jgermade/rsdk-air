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
    # Export list enforced by the linker too: some upstream headers (tinyxml2) force
    # default visibility on their classes.
    set(exports_dir "${PROJECT_BINARY_DIR}/rsdkair-exports")
    if(NOT EXISTS "${exports_dir}/engine.map")
        file(WRITE "${exports_dir}/engine.map" "{\n  global: rsdkair_engine_*;\n  local: *;\n};\n")
        file(WRITE "${exports_dir}/engine.exp" "_rsdkair_engine_abi_version\n_rsdkair_engine_games\n_rsdkair_engine_main\n")
    endif()
    if(APPLE)
        set_target_properties(${name} PROPERTIES INSTALL_RPATH "@loader_path/..")
        target_link_options(${name} PRIVATE "LINKER:-undefined,error"
            "LINKER:-exported_symbols_list,${exports_dir}/engine.exp")
    elseif(UNIX)
        set_target_properties(${name} PROPERTIES INSTALL_RPATH "$ORIGIN/..")
        target_link_options(${name} PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined"
            "LINKER:--version-script=${exports_dir}/engine.map")
    endif()
    if(NOT ANDROID)
        install(TARGETS ${name} LIBRARY DESTINATION engines RUNTIME DESTINATION engines)
    endif()
endfunction()

# Patching upstream sources without touching the submodules:
#
#   rsdkair_patch_begin(text "${upstream}/RSDKv4/Userdata.cpp")
#   rsdkair_patch_replace(text [=[old code]=] [=[new code]=])
#   rsdkair_patch_end(text patched_path)
#
# Each <old> must occur exactly once, so a changed upstream fails at configure time
# instead of silently building something different. The patched copy is written to
# <build>/patched/<file name>. (One replacement per call: CMake would split a list of
# pairs at every ";" in the code.)
function(rsdkair_patch_begin text_var source)
    file(READ "${source}" content)
    set(${text_var} "${content}" PARENT_SCOPE)
    set(${text_var}_SOURCE "${source}" PARENT_SCOPE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source}")
endfunction()

function(rsdkair_patch_replace text_var old new)
    set(content "${${text_var}}")
    string(FIND "${content}" "${old}" first)
    string(FIND "${content}" "${old}" again REVERSE)
    if(first EQUAL -1)
        message(FATAL_ERROR "patch anchor not found in ${${text_var}_SOURCE}:\n${old}")
    endif()
    if(NOT first EQUAL again)
        message(FATAL_ERROR "patch anchor found more than once in ${${text_var}_SOURCE}:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" content "${content}")
    set(${text_var} "${content}" PARENT_SCOPE)
endfunction()

function(rsdkair_patch_end text_var out_var)
    get_filename_component(name "${${text_var}_SOURCE}" NAME)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/patched/${name}")
    file(WRITE "${out}.tmp" "${${text_var}}")
    configure_file("${out}.tmp" "${out}" COPYONLY)  # touches the file only when it changed
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()
