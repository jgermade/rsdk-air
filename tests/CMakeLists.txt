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

foreach(name catalog cli rom detect detect_engines windows_quoting module_hidden_symbols run_engine run_engine_errors child_process
        ini saves_rsdkv4)
    add_test(NAME ${name} COMMAND rsdkair_tests ${name})
endforeach()

# The real executable, without a display: --version and --list never open a window.
add_test(NAME launcher_version COMMAND rsdk-air --version)
set_tests_properties(launcher_version PROPERTIES PASS_REGULAR_EXPRESSION "rsdk-air .* \\(engine ABI 1, Plus o(n|ff)\\)")

set(empty_root "${CMAKE_CURRENT_BINARY_DIR}/empty-root")
add_test(NAME launcher_list COMMAND rsdk-air --list --data-root "${empty_root}")
set_tests_properties(launcher_list PROPERTIES PASS_REGULAR_EXPRESSION "sonic1 +Sonic the Hedgehog +not available")

# Sonic Mania's data is there, but no build includes the rsdkv5 engine yet.
set(nomod_root "${CMAKE_CURRENT_BINARY_DIR}/no-engine-root")
file(WRITE "${nomod_root}/games/mania/Data.rsdk" "not real data")
if(NOT TARGET rsdkv5)
    add_test(NAME launcher_list_no_engine COMMAND rsdk-air --list --data-root "${nomod_root}")
    set_tests_properties(launcher_list_no_engine PROPERTIES
        PASS_REGULAR_EXPRESSION "mania +Sonic Mania +not available: this build of rsdk-air does not include the rsdkv5 engine")
    add_test(NAME launcher_game_no_engine COMMAND rsdk-air --game mania --data-root "${nomod_root}")
    set_tests_properties(launcher_game_no_engine PROPERTIES
        ENVIRONMENT "SDL_VIDEODRIVER=dummy" WILL_FAIL ON)
endif()

add_test(NAME launcher_bad_args COMMAND rsdk-air --nope)
set_tests_properties(launcher_bad_args PROPERTIES WILL_FAIL ON)

if(NOT RSDKAIR_ENABLE_PLUS)
    # Guard: a default build must report Plus off.
    add_test(NAME launcher_plus_off COMMAND rsdk-air --version)
    set_tests_properties(launcher_plus_off PROPERTIES PASS_REGULAR_EXPRESSION "Plus off")
endif()
