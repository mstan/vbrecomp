cmake_minimum_required(VERSION 3.20)
project(vbrecomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Always-on ring buffer rule (per ~/.claude/CLAUDE.md) is a hard requirement;
# the build never disables ring buffers in release. We expose a knob anyway
# in case someone wants to drop the heartbeat thread on embedded targets.
option(VBRECOMP_DEBUG_TOOLS "Build the runtime with the TCP debug server + rings" ON)
option(VBRECOMP_CPUHOOK "Record per-instruction CPU state for oracle comparisons" OFF)

# Position-independent code, useful when generated/ ships .o for relocation.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(runtime)

include(CTest)
if(BUILD_TESTING)
    add_executable(vb-rom-patch-test runtime/tests/rom_patch_test.c runtime/src/rom_patch.c)
    target_include_directories(vb-rom-patch-test PRIVATE runtime/include)
    if(MSVC)
        target_compile_options(vb-rom-patch-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(vb-rom-patch-test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME vb-rom-patch COMMAND vb-rom-patch-test)
    add_executable(vb-input-test runtime/tests/input_test.c runtime/src/input.c)
    target_include_directories(vb-input-test PRIVATE runtime/include)
    if(MSVC)
        target_compile_options(vb-input-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(vb-input-test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME vb-input COMMAND vb-input-test)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        add_executable(vb-controller-test runtime/tests/controller_test.cpp)
        target_include_directories(vb-controller-test PRIVATE runtime/include ${SDL2_INCLUDE_DIRS})
        target_compile_definitions(vb-controller-test PRIVATE SDL_MAIN_HANDLED)
        target_link_libraries(vb-controller-test PRIVATE SDL2::SDL2)
        if(MSVC)
            target_compile_options(vb-controller-test PRIVATE /UNDEBUG)
        else()
            target_compile_options(vb-controller-test PRIVATE -UNDEBUG)
        endif()
        add_test(NAME vb-controller COMMAND vb-controller-test)
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_bridge_dir "${CMAKE_CURRENT_BINARY_DIR}/bridge-fixture")
    add_custom_command(OUTPUT "${_bridge_dir}/bridge_full.c" "${_bridge_dir}/bridge_dispatch.c" "${_bridge_dir}/bridge.h" "${_bridge_dir}/bridge.rom"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime/tests/generate_bridge_fixture.py" "${_bridge_dir}"
        DEPENDS runtime/tests/generate_bridge_fixture.py recompiler/v810/emitter.py recompiler/v810/analysis.py)
    add_executable(vb-interpreter-test runtime/tests/interpreter_test.c
        runtime/src/v810_interpreter.c runtime/src/v810_extended.c runtime/src/interrupts.c
        "${_bridge_dir}/bridge_full.c" "${_bridge_dir}/bridge_dispatch.c")
    target_include_directories(vb-interpreter-test PRIVATE runtime/include "${_bridge_dir}")
    if(MSVC)
        target_compile_options(vb-interpreter-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(vb-interpreter-test PRIVATE -UNDEBUG)
        target_link_libraries(vb-interpreter-test PRIVATE m)
    endif()
    add_test(NAME vb-interpreter COMMAND vb-interpreter-test "${_bridge_dir}/bridge.rom")
    add_executable(vb-vip-source-test runtime/tests/vip_source_test.c runtime/tests/vip_source_unreachable.c
        runtime/src/viewport.c)
    target_include_directories(vb-vip-source-test PRIVATE runtime/include)
    if(MSVC)
        target_compile_options(vb-vip-source-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(vb-vip-source-test PRIVATE -UNDEBUG)
        target_link_libraries(vb-vip-source-test PRIVATE m)
    endif()
    add_test(NAME vb-vip-source COMMAND vb-vip-source-test)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(vb-mod-runtime-test runtime/tests/mod_runtime_test.cpp
        runtime/src/mod_runtime.cpp runtime/src/vb_sha256.c runtime/src/vb_crc32.c)
    target_include_directories(vb-mod-runtime-test PRIVATE runtime/include)
    if(EXISTS "${RECOMP_UI_ROOT}/src/recomp_launcher.h")
        target_compile_definitions(vb-mod-runtime-test PRIVATE RECOMP_LAUNCHER=1)
        target_include_directories(vb-mod-runtime-test PRIVATE "${RECOMP_UI_ROOT}/src")
    endif()
    add_test(NAME vb-mod-runtime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime/tests/run_mod_test.py"
        "$<TARGET_FILE:vb-mod-runtime-test>" "${CMAKE_CURRENT_BINARY_DIR}")
endif()
