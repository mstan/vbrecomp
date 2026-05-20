cmake_minimum_required(VERSION 3.20)
project(vb_runtime C CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/runtime.cmake")

# vb-runtime target. By default builds with the `no_game_linked.c`
# stub so the binary links without any P3 codegen present. Pass
# -DVBRECOMP_GAME=marios_tennis at configure-time to link the
# generated dispatch from generated/ instead.
set(VBRECOMP_GAME "" CACHE STRING "Module name (e.g. marios_tennis) of a P3 codegen output to link into vb-runtime. Empty = use no_game_linked.c stub.")

if(VBRECOMP_GAME STREQUAL "")
    vb_add_runtime_target(vb-runtime
        DEBUG_PORT 4390
        WINDOW_TITLE "vbrecomp - runtime"
        NO_GAME_LINKED
    )
else()
    message(STATUS "vb-runtime: linking generated dispatch for "
                   "module='${VBRECOMP_GAME}'")
    vb_add_runtime_target(vb-runtime
        DEBUG_PORT 4390
        WINDOW_TITLE "vbrecomp - runtime"
        GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated"
        GENERATED_MODULE "${VBRECOMP_GAME}"
    )
endif()

# Oracle target — only built when the beetle-vb static archive is present.
# Mirrors psxrecomp's pattern (Rule 14: two independent processes, same
# wire protocol on a different port).
#
# Libretro's Makefile in STATIC_LINKING=1 mode produces an `ar rcs`
# archive but names it `mednafen_vb_libretro.dll` on Windows (or `.so`
# on Unix). We accept either the conventional libmednafen_vb.a name or
# the libretro static-mode output verbatim — both are real static
# archives, the suffix is cosmetic.
set(_beetle_candidates
    "${CMAKE_SOURCE_DIR}/beetle-vb/libmednafen_vb.a"
    "${CMAKE_SOURCE_DIR}/beetle-vb/mednafen_vb_libretro.dll"
    "${CMAKE_SOURCE_DIR}/beetle-vb/mednafen_vb_libretro.so"
)
set(BEETLE_VB_LIB "")
foreach(_cand IN LISTS _beetle_candidates)
    if(EXISTS "${_cand}")
        set(BEETLE_VB_LIB "${_cand}")
        break()
    endif()
endforeach()

if(BEETLE_VB_LIB)
    message(STATUS "vb-beetle: found ${BEETLE_VB_LIB}")

    # SDL2 is required only for vb-beetle (the runtime currently runs
    # headless). Skip the target gracefully if SDL2 isn't installed.
    find_package(SDL2 QUIET)
    if(NOT SDL2_FOUND)
        message(STATUS "vb-beetle: SDL2 not found; skipping target "
                       "(install via mingw-w64-x86_64-SDL2 or sdl2-msvc/).")
    else()
        # mednafen-vb's STATIC_LINKING archive doesn't bundle the
        # libretro-common compat shims (the frontend is expected to
        # provide them). Compile the ones state.c references — strl
        # is the only stripped symbol that surfaces in the link.
        add_executable(vb-beetle
            ${CMAKE_CURRENT_SOURCE_DIR}/src/beetle_main.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/beetle_libretro.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/beetle_debug_server.c
            ${CMAKE_SOURCE_DIR}/beetle-vb/libretro-common/compat/compat_strl.c
        )
        target_include_directories(vb-beetle PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/beetle-vb/libretro-common/include
            ${SDL2_INCLUDE_DIRS}
        )
        # Beetle's static archive is just a bag of objects; link it
        # whole-archive-style so the libretro entry-point symbols
        # don't get stripped before our beetle_libretro.cpp can
        # reference them.
        if(MSVC)
            target_link_libraries(vb-beetle PRIVATE
                ${BEETLE_VB_LIB} ${SDL2_LIBRARIES})
        else()
            target_link_libraries(vb-beetle PRIVATE
                -Wl,--whole-archive ${BEETLE_VB_LIB} -Wl,--no-whole-archive
                ${SDL2_LIBRARIES})
        endif()
        if(WIN32)
            target_link_libraries(vb-beetle PRIVATE ws2_32)
        endif()
        # Beetle uses C++ stdlib; force C++17 across the target so the
        # standard library bits compiled into the archive resolve.
        set_target_properties(vb-beetle PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON)
        if(MSVC)
            target_compile_options(vb-beetle PRIVATE /W4 /permissive- /utf-8)
        else()
            target_compile_options(vb-beetle PRIVATE
                -Wall -Wextra -Wno-unused-parameter)
        endif()
        message(STATUS "vb-beetle: target added "
                       "(SDL2 ${SDL2_VERSION_STRING})")
    endif()
else()
    message(STATUS "vb-beetle: no static archive under beetle-vb/; skipping "
                   "oracle target (see docs/BRINGUP.md to build beetle-vb)")
endif()
