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

# Position-independent code, useful when generated/ ships .o for relocation.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(runtime)
