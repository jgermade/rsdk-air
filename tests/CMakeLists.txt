add_executable(rsdkair_tests tests.cpp)
target_link_libraries(rsdkair_tests PRIVATE rsdkair_core)
target_compile_definitions(rsdkair_tests PRIVATE
    "RSDKAIR_DUMMY_MODULE=\"$<TARGET_FILE:rsdkair_dummy>\"")
add_dependencies(rsdkair_tests rsdkair_dummy)
rsdkair_warnings(rsdkair_tests)

# One ctest entry per test case in tests.cpp.
if(TARGET rsdkv4)
    target_compile_definitions(rsdkair_tests PRIVATE "RSDKAIR_RSDKV4_MODULE=\"$<TARGET_FILE:rsdkv4>\"")
    add_dependencies(rsdkair_tests rsdkv4)
    add_test(NAME rsdkv4_module COMMAND rsdkair_tests rsdkv4_module)
endif()

foreach(name catalog cli rom detect detect_engines sha256 s3air windows_quoting module_hidden_symbols run_engine run_engine_errors child_process
        ini saves_rsdkv4)
    add_test(NAME ${name} COMMAND rsdkair_tests ${name})
endforeach()

# The real executable, without a display: --version and --list never open a window.
add_test(NAME launcher_version COMMAND rsdk-air --version)
set_tests_properties(launcher_version PROPERTIES PASS_REGULAR_EXPRESSION "rsdk-air .* \\(engine ABI 1, Plus o(n|ff)\\)")

set(empty_root "${CMAKE_CURRENT_BINARY_DIR}/empty-root")
add_test(NAME launcher_list COMMAND rsdk-air --list --data-root "${empty_root}")
set_tests_properties(launcher_list PROPERTIES PASS_REGULAR_EXPRESSION "sonic1 +Sonic the Hedgehog +not available")

# Sonic 3 A.I.R.'s ROM is there, but its official build is not installed: --list says so
# (or that there is no official build for this system), and --game fails.
set(s3air_root "${CMAKE_CURRENT_BINARY_DIR}/s3air-root")
file(WRITE "${s3air_root}/games/s3air/Sonic_Knuckles_wSonic3.bin" "not a real ROM")
add_test(NAME launcher_list_s3air_not_installed COMMAND rsdk-air --list --data-root "${s3air_root}")
set_tests_properties(launcher_list_s3air_not_installed PROPERTIES
    PASS_REGULAR_EXPRESSION "s3air +Sonic 3 A.I.R. +not available: (Sonic 3 A.I.R. is not installed|there is no official Sonic 3 A.I.R. build)")
add_test(NAME launcher_game_s3air_not_installed COMMAND rsdk-air --game s3air --data-root "${s3air_root}")
set_tests_properties(launcher_game_s3air_not_installed PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" WILL_FAIL ON)

add_test(NAME launcher_bad_args COMMAND rsdk-air --nope)
set_tests_properties(launcher_bad_args PROPERTIES WILL_FAIL ON)

if(NOT RSDKAIR_ENABLE_PLUS)
    # Guard: a default build must report Plus off.
    add_test(NAME launcher_plus_off COMMAND rsdk-air --version)
    set_tests_properties(launcher_plus_off PROPERTIES PASS_REGULAR_EXPRESSION "Plus off")
endif()
