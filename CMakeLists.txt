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
